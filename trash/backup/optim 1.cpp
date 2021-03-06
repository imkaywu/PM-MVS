//
//  optim.cpp
//  PMMVPS
//
//  Created by KaiWu on Oct/24/16.
//  Copyright © 2016 KaiWu. All rights reserved.
//

#include <numeric>
#include "optim.hpp"
#include "pmmvps.hpp"
#include "nlopt.hpp"

using std::min;
using std::max;
using Eigen::Vector3f;

Optim* Optim::m_inst = NULL;

Optim::Optim(PmMvps& pmmvps) : m_pmmvps(pmmvps) {
    m_inst = this;
}

void Optim::init() {
    m_weights.resize(m_pmmvps.m_nviews);
    
    m_texs.resize(m_pmmvps.m_nviews);
    for (int i = 0; i < m_pmmvps.m_tau; ++i) {
        m_texs[i].resize(m_pmmvps.m_wsize * m_pmmvps.m_wsize);
    }
    
    m_textures.resize(m_pmmvps.m_nviews);
    for (int i = 0; i < m_pmmvps.m_tau; ++i) {
        m_textures[i].resize(m_pmmvps.m_nillums);
        for (int illum = 0; illum < m_pmmvps.m_nillums; ++illum) {
            m_textures[i][illum].resize(m_pmmvps.m_wsize * m_pmmvps.m_wsize);
        }
    }
    
    setAxesScales();
}

void Optim::setAxesScales() {
    m_xaxes.resize(m_pmmvps.m_nviews);
    m_yaxes.resize(m_pmmvps.m_nviews);
    m_zaxes.resize(m_pmmvps.m_nviews);
    for (int index = 0; index < m_pmmvps.m_nviews; ++index) {
        Vector4f oaxis = m_pmmvps.m_photoSet.m_photos[index].m_oaxis;
        m_zaxes[index] = Vector3f(oaxis(0), oaxis(1), oaxis(2));
        Vector4f xaxis = m_pmmvps.m_photoSet.m_photos[index].m_projections[0].row(0);
        m_xaxes[index] = Vector3f(xaxis(0), xaxis(1), xaxis(2));
        m_yaxes[index] = m_zaxes[index].cross(m_xaxes[index]);
        m_yaxes[index] /= m_yaxes[index].norm();
        m_xaxes[index] = m_yaxes[index].cross(m_zaxes[index]);
    }
    
    m_ipscales.resize(m_pmmvps.m_nviews);
    for (int index = 0; index < m_pmmvps.m_nviews; ++index) {
        const Vector4f xaxis(m_xaxes[index](0), m_xaxes[index](1), m_xaxes[index](2), 0.0f);
        const Vector4f yaxis(m_yaxes[index](0), m_yaxes[index](1), m_yaxes[index](2), 0.0f);
        const float fx = m_pmmvps.m_photoSet.m_photos[index].m_projections[0].row(0).dot(xaxis);
        const float fy = m_pmmvps.m_photoSet.m_photos[index].m_projections[0].row(1).dot(yaxis);
        m_ipscales[index] = fx + fy;
    }
}

void Optim::getPAxes(const int index, const Vector4f& coord, const Vector4f& normal, Vector4f& pxaxis, Vector4f& pyaxis) {
    const float pscale = getUnit(index, coord);
    
    Vector3f normal3(normal(0), normal(1), normal(2));
    Vector3f yaxis3 = normal3.cross(m_xaxes[index]);
    yaxis3 /= yaxis3.norm();
    Vector3f xaxis3 = yaxis3.cross(normal3);
    
    pxaxis << xaxis3(0), xaxis3(1), xaxis3(2), 0.0f;
    pyaxis << yaxis3(0), yaxis3(1), yaxis3(2), 0.0f;
    pxaxis *= pscale;
    pyaxis *= pscale;
    
    const float xdis = (m_pmmvps.m_photoSet.project(index, coord + pxaxis, m_pmmvps.m_level) - m_pmmvps.m_photoSet.project(index, coord, m_pmmvps.m_level)).norm();
    const float ydis = (m_pmmvps.m_photoSet.project(index, coord + pyaxis, m_pmmvps.m_level) - m_pmmvps.m_photoSet.project(index, coord, m_pmmvps.m_level)).norm();
    pxaxis /= xdis;
    pyaxis /= ydis;
}

float Optim::getUnit(const int index, const Vector4f &coord) const {
    const float fz = (coord - m_pmmvps.m_photoSet.m_photos[index].m_center).norm();
    const float ipscale = m_ipscales[index];
    if (ipscale == 0.0f) {
        return 1.0;
    }
    return 2.0 * fz * (0x0001 << m_pmmvps.m_level) / ipscale; // why multiply by 2.0
}

void Optim::computeUnits(const Patch& patch, vector<int>& indexes, vector<float>& units, vector<Vector4f, Eigen::aligned_allocator<Vector4f> >& rays) const {
    vector<int>::const_iterator bimage = patch.m_images.begin();
    vector<int>::const_iterator eimage = patch.m_images.end();
    
    while (bimage != eimage) {
        Vector4f ray = m_pmmvps.m_photoSet.m_photos[*bimage].m_center - patch.m_coord;
        ray /= ray.norm();
        const float dot = ray.dot(patch.m_normal);
        if (dot <= 0.0f) {
            ++bimage;
            continue;
        }
        
        const float scale = getUnit(*bimage, patch.m_coord);
        const float unit = scale / dot;
        
        indexes.push_back(*bimage);
        units.push_back(unit);
        rays.push_back(ray);
        ++bimage;
    }
}

void Optim::computeUnits(const Patch& patch, vector<float>& units) const {
    const int sz = static_cast<int>(patch.m_images.size());
    units.resize(sz);
    
    vector<int>::const_iterator bimage = patch.m_images.begin();
    vector<int>::const_iterator eimage = patch.m_images.end();
    vector<float>::iterator bunit = units.begin();
    
    while (bimage != eimage) {
        *bunit = INT_MAX / 2;
        *bunit = getUnit(*bimage, patch.m_coord);
        Vector4f ray = m_pmmvps.m_photoSet.m_photos[*bimage].m_center - patch.m_coord;
        ray /= ray.norm();
        const float dot = ray.dot(patch.m_normal);
        if (0.0f < dot) {
            *bunit /= dot;
        }
        else {
            *bunit = INT_MAX / 2;
        }
        ++bimage;
        ++bunit;
    }
}

//-----------------------------------------------------------------
// Optimization related -- pre and post optimization processing
//-----------------------------------------------------------------
int Optim::preProcess(Patch& patch) {
    
    addImage(patch);
    
    constraintImages(patch, m_pmmvps.m_nccThresholdBefore);
    
    setRefImage(patch);
    
    sortImages(patch);
    
    if (static_cast<int>(patch.m_images.size()) > 0) {
        m_pmmvps.m_patchManager.setScales(patch);
    }
    
    if (static_cast<int>(patch.m_images.size()) < m_pmmvps.m_minImageNumThreshold) {
        return -1;
    }
    
    const int flag = m_pmmvps.m_photoSet.checkAngles(patch.m_coord, patch.m_images,
                                                     m_pmmvps.m_maxAngleThreshold,
                                                     m_pmmvps.m_angleThreshold1,
                                                     m_pmmvps.m_minImageNumThreshold);
    if (flag) {
        patch.m_images.clear();
        return -1;
    }
    
    return 0;
}

void Optim::addImage(Patch& patch) {
    patch.m_images.clear();
    const float angleThreshold = cosf(m_pmmvps.m_angleThreshold0);
    
    for (int i = 0; i < m_pmmvps.m_nviews; ++i) {
        const Vector3f icoord = m_pmmvps.m_photoSet.project(i, patch.m_coord, m_pmmvps.m_level);
        if (m_pmmvps.m_photoSet.getMask(i, icoord(0), icoord(1), m_pmmvps.m_level) == 0) {
            continue;
        }
        
        Vector4f ray = m_pmmvps.m_photoSet.m_photos[i].m_center - patch.m_coord;
        ray /= ray.norm();
        const float cosAng = ray.dot(patch.m_normal);
        
        if (angleThreshold <= cosAng) {
            patch.m_images.push_back(i);
        }
    }
}

void Optim::constraintImages(Patch& patch, const float nccThreshold) {
    vector<float> inccs;
    setINCCs(patch, inccs, patch.m_images, 0);
    
    vector<int> newimages;
    newimages.push_back(patch.m_images[0]);
    for (int i = 1; i < static_cast<int>(patch.m_images.size()); ++i) {
        if (inccs[i] < 1.0f - nccThreshold) {
            newimages.push_back(patch.m_images[i]);
        }
    }
    patch.m_images.swap(newimages);
}

void Optim::sortImages(Patch &patch, const int isFixed) const {
    const float threshold = 1.0f - cos(10.0f * M_PI / 180.0f);
    vector<int> indexes0, indexes1;
    vector<float> units0, units1;
    vector<Vector4f, Eigen::aligned_allocator<Vector4f> > rays0, rays1;
    
    computeUnits(patch, indexes0, units0, rays0);
    
    patch.m_images.clear();
    if (indexes0.size() < 2) {
        return;
    }
    if (isFixed) {
        units0[0] = 0.0f;
    }
    
    while (!indexes0.empty()) {
        vector<float>::iterator iter = min_element(units0.begin(), units0.end());
        const int index = static_cast<int>(iter - units0.begin());
        patch.m_images.push_back(indexes0[index]);
        
        indexes1.clear();
        units1.clear();
        rays1.clear();
        for (int i = 0; i < static_cast<int>(rays0.size()); ++i) {
            if (i == index) {
                continue;
            }
            indexes1.push_back(indexes0[i]);
            rays1.push_back(rays0[i]);
            const float ftmp = min(threshold, max(threshold / 2.0f, 1.0f - rays0[index].dot(rays0[i])));
            units1.push_back(units0[i] * threshold / ftmp);
        }
        indexes1.swap(indexes0);
        units1.swap(units0);
        rays1.swap(rays0);
    }
}

int Optim::postProcess(Patch& patch) {
    if (static_cast<int>(patch.m_images.size()) < m_pmmvps.m_minImageNumThreshold) {
        return -1;
    }
    addImage(patch);
    constraintImages(patch, m_pmmvps.m_nccThreshold);
    filterImagesByAngle(patch);
    
    if (static_cast<int>(patch.m_images.size()) < m_pmmvps.m_minImageNumThreshold) {
        return -1;
    }
    
    m_pmmvps.m_patchManager.setGrids(patch);
    setRefImage(patch);
    constraintImages(patch, m_pmmvps.m_nccThreshold);
    
    if (static_cast<int>(patch.m_images.size()) < m_pmmvps.m_minImageNumThreshold) {
        return -1;
    }
    m_pmmvps.m_patchManager.setGrids(patch);
    
    patch.m_nimages = static_cast<int>(patch.m_images.size());
    patch.m_tmp = patch.score2(m_pmmvps.m_nccThreshold);
    
    return 0;
}

void Optim::filterImagesByAngle(Patch& patch) {
    vector<int> newimages;
    vector<int>::iterator bimage = patch.m_images.begin();
    vector<int>::iterator eimage = patch.m_images.end();
    
    while (bimage != eimage) {
        const int index = *bimage;
        Vector4f ray = m_pmmvps.m_photoSet.m_photos[index].m_center - patch.m_coord;
        ray /= ray.norm();
        if (ray.dot(patch.m_normal) < cos(m_pmmvps.m_angleThreshold1)) {
            if (bimage == patch.m_images.begin()) {
                patch.m_images.clear();
                return;
            }
        }
        else {
            newimages.push_back(index);
            ++bimage;
        }
    }
    patch.m_images.swap(newimages);
}

void Optim::setRefImage(Patch& patch) {
    vector<int> indexes;
    vector<int>::const_iterator bimage = patch.m_images.begin();
    vector<int>::const_iterator eimage = patch.m_images.end();
    while (bimage != eimage) {
        indexes.push_back(*bimage);
        ++bimage;
    }
    
    if (indexes.empty()) {
        patch.m_images.clear();
        return;
    }
    
    vector<vector<float> > inccs;
    setINCCs(patch, inccs, indexes, 1);
    
    int refindex = -1;
    float refncc = INT_MAX / 2;
    for (int i = 0; i < static_cast<int>(indexes.size()); ++i) {
        const float sum = accumulate(inccs[i].begin(), inccs[i].end(), 0.0f);
        if (sum < refncc) {
            refncc = sum;
            refindex = i;
        }
    }
    const int refIndex = indexes[refindex];
    for (int i = 0; i < static_cast<int>(patch.m_images.size()); ++i) {
        if (patch.m_images[i] == refIndex) {
            const int itmp = patch.m_images[0];
            patch.m_images[0] = refIndex;
            patch.m_images[i] = itmp;
            break;
        }
    }
}

void Optim::swapImage(Patch& patch, const int image) {
    for (int i = 0; i < patch.m_nimages; ++i) {
        if (patch.m_images[0] == image) {
            break;
        }
        if (patch.m_images[i] == image) {
            std::swap(patch.m_images[0], patch.m_images[i]);
            std::swap(patch.m_grids[0], patch.m_grids[i]);
        }
    }
}

//------------------------------------------------
// Patch optimization
//------------------------------------------------

double Optim::cost_func(unsigned n, const double* x, double* grad, void* func_data) {
    double xs[3] = {x[0], x[1], x[2]};
    
//    const float angle1 = xs[1] * m_inst->m_ascale;
//    const float angle2 = xs[2] * m_inst->m_ascale;
    
    double cost = 0.0;
    const double bias = 0.0f;
    
    Vector4f coord, normal;
    m_inst->decode(coord, normal, xs);
    
    const int index = m_inst->m_indexes[0];
    Vector4f pxaxis, pyaxis;
    m_inst->getPAxes(index, coord, normal, pxaxis, pyaxis);
    
    const int sz = min(m_inst->m_pmmvps.m_tau, static_cast<int>(m_inst->m_indexes.size()));
    const int minimum = min(m_inst->m_pmmvps.m_minImageNumThreshold, sz);
    
    for (int i = 0; i < sz; ++i) {
        int flag = m_inst->getTex(coord, pxaxis, pyaxis, normal, m_inst->m_indexes[i], m_inst->m_pmmvps.m_wsize, m_inst->m_textures[i]);
        if (flag == 0) {
            for (int illum = 0; illum < m_inst->m_textures[i].size(); ++illum) {
                m_inst->normalize(m_inst->m_textures[i][illum]);
            }
        }
    }
    
    int nillums = static_cast<int>(m_inst->m_textures[0].size());
    
    const int pairwise = 0;
    if (pairwise) {
        double ans = 0.0;
        int denom = 0;
        for (int i = 0; i < sz; ++i) {
            for (int j = i + 1; j < sz; ++j) {
                if (m_inst->m_texs[i].empty() || m_inst->m_texs[j].empty())
                    continue;
                
                ans += robustincc(1.0 - m_inst->dot(m_inst->m_texs[i], m_inst->m_texs[j]));
                denom++;
            }
        }
        if (denom < minimum * (minimum - 1) / 2) {
            cost = 2.0f;
        }
        else {
            cost = ans / denom + bias;
        }
    }
    else {
        if (m_inst->m_textures[0].empty()) {
            return 2.0;
        }
        double ans = 0.0f;
        int denom = 0;
        for (int i = 1; i < sz; ++i) {
            if (m_inst->m_textures[i].empty()) {
                continue;
            }
            double incc = 0.0f;
            for (int illum = 0; illum < nillums; ++illum) {
                incc += robustincc(1.0 - m_inst->dot(m_inst->m_textures[0][illum], m_inst->m_textures[i][illum]));
            }
            incc /= nillums;
            ans += incc;
            denom++;
        }
        if (denom < minimum - 1) {
            cost = 2.0f;
        }
        else {
            cost = ans / denom + bias;
        }
    }
    return cost;
}

void Optim::refinePatch(Patch& patch, const int time) {
    const int TIME = 500;
    if (refinePatch(patch, TIME, 1) == -1)
        cerr << "refinePatch failed!" << endl;
    
    if (patch.m_images.empty()) {
        return;
    }
}

int Optim::refinePatch(Patch& patch, const int time, const int ncc) {
    m_center = patch.m_coord;
    m_ray = patch.m_coord - m_pmmvps.m_photoSet.m_photos[patch.m_images[0]].m_center;
    m_ray /= m_ray.norm();
    m_indexes = patch.m_images;
    
    m_dscale = patch.m_dscale;
    m_ascale = M_PI / 48.0f; // patch.m_ascale
    
    // compute units
    computeWeights(patch);
    
    // encode;
    double p[3];
    encode(patch.m_coord, patch.m_normal, p);
    
    double min_angle = -23.99999;
    double max_angle = 23.99999;
    
    vector<double> lowerBound(3);
    lowerBound[0] = -HUGE_VALF;
    lowerBound[1] = min_angle;
    lowerBound[2] = min_angle;
    vector<double> upperBound(3);
    upperBound[0] = HUGE_VALF;
    upperBound[1] = max_angle;
    upperBound[2] = max_angle;
    
    bool isSuccess = false;
    
    try {
        nlopt::opt opt(nlopt::LN_BOBYQA, 3);
        opt.set_min_objective(cost_func, NULL); // *f_data NULL
        opt.set_xtol_rel(1.e-7);
        opt.set_maxeval(time);
        opt.set_lower_bounds(lowerBound);
        opt.set_upper_bounds(upperBound);
        
        vector<double> x(3);
        for (int i = 0; i < 3; ++i) {
            x[i] = max(min(p[i], upperBound[i]), lowerBound[i]);
        }
        
        double minf;
        nlopt::result result = opt.optimize(x, minf);
        
        p[0] = x[0];
        p[1] = x[1];
        p[2] = x[2];
        
        isSuccess = (result == nlopt::SUCCESS || result == nlopt::STOPVAL_REACHED || result == nlopt::FTOL_REACHED || result == nlopt::XTOL_REACHED);
    } catch (std::exception& e) {
        isSuccess = false;
    }
    
    if (isSuccess) {
        // decode
        decode(patch.m_coord, patch.m_normal, p);
        patch.m_normal(3) = -patch.m_coord.dot(patch.m_normal);
//        for (int v = 0; v < m_pmmvps.m_nviews; ++v) {
//            Vector3f icoord = m_pmmvps.m_photoSet.project(patch.m_images[v], patch.m_coord, 0);
//            cerr << "(x, y) after optimization: " << icoord(0) << ", " << icoord(1) << endl;
//        }
        cerr << "ncc before: " << patch.m_ncc << endl;
        patch.m_ncc = 1.0 - unrobustincc(computeINCC(patch.m_coord, patch.m_normal, patch.m_images, 1));
        cerr << "ncc after: " << patch.m_ncc << endl;
    }
    else {
        return -1;
    }
    
    return 0;
}

void Optim::encode(const Vector4f& coord, double* const vect) const {
    vect[0] = (coord - m_center).dot(m_ray) / m_dscale;
}

void Optim::encode(const Vector4f& coord, const Vector4f& normal, double* const vect) {
    encode(coord, vect);
    
    const int image = m_indexes[0];
//    Vector3f normal3 = normal.head(3) / normal(3);
    Vector3f normal3 = normal.head(3);
    const float fx = m_xaxes[image].dot(normal3);
    const float fy = m_yaxes[image].dot(normal3);
    const float fz = m_zaxes[image].dot(normal3);
    
    vect[2] = asinf(std::max(-1.0f, std::min(1.0f, fy)));
    const float cosb = cos(vect[2]);
    
    if (cosb == 0.0f) {
        vect[1] = 0.0f;
    }
    else {
        const float sina = fx / cosb;
        const float cosa = -fz / cosb;
        vect[1] = acosf(std::max(-1.0f, std::min(1.0f, cosa)));
        if (sina < 0.0f) {
            vect[1] = -vect[1];
        }
    }
    
    vect[1] = vect[1] / m_ascale;
    vect[2] = vect[2] / m_ascale;
}

void Optim::decode(Vector4f& coord, Vector4f& normal, const double* const vect) const {
    decode(coord, vect);
    const int image = m_indexes[0];
    
    const float angle1 = vect[1] * m_ascale;
    const float angle2 = vect[2] * m_ascale;
    
    const float fx = sinf(angle1) * cosf(angle2);
    const float fy = sinf(angle2);
    const float fz = -cosf(angle1) * cosf(angle2);
    
    Vector3f normal3 = m_xaxes[image] * fx + m_yaxes[image] * fy + m_zaxes[image] * fz;
    normal << normal3(0), normal3(1), normal3(2), 0.0f;
}

void Optim::decode(Vector4f& coord, const double* const vect) const {
    coord = m_center + m_dscale * vect[0] * m_ray;
}

float Optim::dot(const vector<Vector3f>& tex0, const vector<Vector3f>& tex1) const {
    const int sz = static_cast<int>(tex0.size());
    float ssd = 0.0f;
    for (int i = 0; i < sz; ++i) {
        ssd += tex0[i].dot(tex1[i]);
    }
    return ssd / sz;
}

float Optim::ssd(const vector<Vector3f> &tex0, const vector<Vector3f> &tex1) const{
    const float scale = 0.01f;
    const int sz = static_cast<int>(tex0.size());
    float ssd = 0.0f;
    for (int i = 0; i < sz; ++i) {
        ssd += (tex0[i].array() - tex1[i].array()).abs().sum();
    }
    
    return scale * ssd / sz;
}

float Optim::robustincc(const float incc) {
    return incc / (1 + 3 * incc);
}

float Optim::unrobustincc(const float rincc) {
    return rincc / (1 - 3 * rincc);
}

float Optim::computeINCC(const Vector4f& coord, const Vector4f& normal, const vector<int>& indexes, const int isRobust) {
    if (static_cast<int>(indexes.size()) < 2) {
        return 2.0;
    }
    
    const int index = indexes[0];
    Vector4f pxaxis, pyaxis;
    getPAxes(index, coord, normal, pxaxis, pyaxis);
    
    return computeINCC(coord, normal, indexes, pxaxis, pyaxis, isRobust);
}

float Optim::computeINCC(const Vector4f& coord, const Vector4f& normal, const vector<int> indexes, const Vector4f& pxaxis, const Vector4f& pyaxis, const int isRobust) {
    if ((int)indexes.size() < 2)
        return 2.0;
    
    const int sz = min(m_pmmvps.m_tau, static_cast<int>(indexes.size()));
    vector<vector<vector<Vector3f> > >& texs = m_textures;
    
    for (int i = 0; i < sz; ++i) {
        int flag = getTex(coord, pxaxis, pyaxis, normal, indexes[i], m_pmmvps.m_wsize, texs[i]);
        
        if (flag == 0) {
            for (int illum = 0; illum < m_pmmvps.m_nillums; ++illum) {
                normalize(texs[i][illum]);
            }
        }
    }
    
    if (texs[0].empty())
        return 2.0;
    
    float score = 0.0f;
#ifdef PMMVPS_PAIRNCC
    float totalWeight = 0.0f;
    for (int i = 0; i < sz; ++i) {
        for (int j = i + 1; j < sz; ++i) {
            if (!texs[i].empty() && !texs[j].empty()) {
                const float wtmp = m_weights[i] + m_weights[j];
                totalWeight += wtmp;
                if (isRobust) {
                    score += robustincc(1.0 - doc(texs[i], texs[j])) * wtmp;
                }
                else {
                    score += (1.0 - doc(texs[i], texs[j])) * wtmp;
                }
            }
        }
    }
    
    if (totalWeight == 0.0f) {
        score = 2.0f;
    }
    else {
        score /= totalWeight;
    }
#else
    float totalWeight = 0.0f;
    for (int i = 1; i < sz; ++i) {
        if (!texs[i].empty()) {
            totalWeight += m_weights[i];
            for (int illum = 0; illum < m_pmmvps.m_nillums; ++illum) {
                if (isRobust) {
                    score += robustincc(1.0 - dot(texs[0][illum], texs[i][illum])) * m_weights[i];
                }
                else {
                    score += (1.0 - dot(texs[0][illum], texs[i][illum])) * m_weights[i];
                }
            }
            score /= m_pmmvps.m_nillums;
        }
    }
    if (totalWeight == 0.0f) {
        score = 2.0f;
    }
    else {
        score /= totalWeight;
    }
#endif
    
    return score;
}

void Optim::setINCCs(const Patch& patch, vector<float>& inccs, const vector<int>& indexes, const int isRobust) {
    const int index = indexes[0];
    Vector4f pxaxis, pyaxis;
    getPAxes(index, patch.m_coord, patch.m_normal, pxaxis, pyaxis);
    
    vector<vector<vector<Vector3f> > >& texs = m_textures;
    
    // indexes stores visible views
    const int sz = static_cast<int>(indexes.size());
    for (int i = 0; i < sz; ++i) {
        const int flag = getTex(patch.m_coord, pxaxis, pyaxis, patch.m_normal, indexes[i], m_pmmvps.m_wsize, texs[i]);
        if (flag == 0) {
            for (int illum = 0; illum < m_pmmvps.m_nillums; ++illum) {
                normalize(texs[i][illum]);
            }
        }
    }
    
    inccs.resize(sz);
    if (texs[0].empty()) {
        fill(inccs.begin(), inccs.end(), 2.0f);
        return;
    }
    else {
        fill(inccs.begin(), inccs.end(), 0.0f);
    }
    
    for (int i = 0; i < sz; ++i) {
        if (i == 0) {
            inccs[i] = 0.0f;
        }
        else if (!texs[i].empty()) {
            for (int illum = 0; illum < m_pmmvps.m_nillums; ++illum) {
                if (isRobust == 0) {
                    inccs[i] += 1.0f - dot(texs[0][illum], texs[i][illum]);
                }
                else {
                    inccs[i] += robustincc(1.0f - dot(texs[0][illum], texs[i][illum]));
                }
            }
            inccs[i] /= m_pmmvps.m_nillums;
        }
        else {
            inccs[i] = 2.0f;
        }
    }
}

void Optim::setINCCs(const Patch& patch, vector<vector<float> >& inccs, const vector<int>& indexes, const int isRobust) {
    const int index = indexes[0];
    Vector4f pxaxis, pyaxis;
    getPAxes(index, patch.m_coord, patch.m_normal, pxaxis, pyaxis);
    
    vector<vector<vector<Vector3f> > >& texs = m_textures;
    
    const int sz = static_cast<int>(indexes.size());
    for (int i = 0; i < sz; ++i) {
        const int flag = getTex(patch.m_coord, pxaxis, pyaxis, patch.m_normal, indexes[i], m_pmmvps.m_wsize, texs[i]);
        if (flag == 0) {
            for (int illum = 0; illum < m_pmmvps.m_nillums; ++illum) {
                normalize(texs[i][illum]);
            }
        }
    }
    inccs.resize(sz);
    for (int i = 0; i < sz; ++i) {
        inccs[i].resize(sz);
        fill(inccs[i].begin(), inccs[i].end(), 0.0f);
    }
    
    for (int i = 0; i < sz; ++i) {
        inccs[i][i] = 0.0f;
        for (int j = i + 1; j < sz; ++j) {
            if (!texs[i].empty() && !texs[j].empty()) {
                for (int illum = 0; illum < m_pmmvps.m_nillums; ++illum) {
                    if (isRobust == 0) {
                        inccs[i][j] += 1.0f - dot(texs[i][illum], texs[j][illum]);
                    }
                    
                    else {
                        inccs[i][j] += robustincc(1.0f - dot(texs[i][illum], texs[j][illum]));
                    }
                }
                inccs[i][j] /= m_pmmvps.m_nillums;
                inccs[j][i] = inccs[i][j];
            }
            else {
                inccs[i][j] = inccs[j][i] = 2.0f;
            }
        }
    }
}

float Optim::myPow2(int levelDiff) const {
    const float scales[] = {0.0625, 0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    return scales[levelDiff + 4];
}

int Optim::getTex(const Vector4f& coord, const Vector4f& pxaxis, const Vector4f& pyaxis, const Vector4f& pzaxis, const int index, const int size, vector<vector<Vector3f> >& texs) const {
    
    texs.clear();
    
    Vector4f ray = m_pmmvps.m_photoSet.m_photos[index].m_center - coord;
    ray /= ray.norm();
    const float weight = std::max(0.0f, ray.dot(pzaxis)); // pzaxis is the patch normal
    if (weight < std::cosf(m_pmmvps.m_angleThreshold1)) {
        return -1;
    }
    
    const int margin = size / 2;
    Vector3f center = m_pmmvps.m_photoSet.project(index, coord, m_pmmvps.m_level);
    Vector3f dx = m_pmmvps.m_photoSet.project(index, coord + pxaxis, m_pmmvps.m_level) - center;
    Vector3f dy = m_pmmvps.m_photoSet.project(index, coord + pyaxis, m_pmmvps.m_level) - center;
    
    // ??? update the center, dx, dy
    // Reason: dx.norm() + dy.norm() == 2
    const float ratio = (dx.norm() + dy.norm()) / 2.0f;
    int levelDiff = (int)floorf(log(ratio) / log(2.0f) + 0.5f);
    levelDiff = std::max(-m_pmmvps.m_level, std::min(2, levelDiff));
    const float scale = myPow2(levelDiff);
    const int newLevel = m_pmmvps.m_level + levelDiff;
    center /= scale;
    dx /= scale;
    dy /= scale;
    
    if (getTexSafe(index, m_pmmvps.m_wsize, center, dx, dy, newLevel) == -1) {
        return -1;
    }
    
    texs.resize(m_pmmvps.m_nillums);
    for (int i = 0; i < m_pmmvps.m_nillums; ++i) {
        texs[i].resize(size * size);
    }

    Vector3f tl = center - dx * margin - dy * margin;
    
    for (int illum = 0; illum < m_pmmvps.m_nillums; ++illum) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                Vector3f samp = tl + dx * x + dy * y;
                int ind = y * size + x;
                texs[illum][ind] = m_pmmvps.m_photoSet.getColor(index, samp(0), samp(1), newLevel, illum);
            }
        }
    }
    
    return 0;
}

int Optim::getTexSafe(const int index, const int size, const Vector3f& center, const Vector3f& dx, const Vector3f& dy, const int level) const {
    const int margin = size / 2;
    
    const Vector3f tl = center - dx * margin - dy * margin;
    const Vector3f tr = center + dx * margin - dy * margin;
    const Vector3f bl = center - dx * margin + dy * margin;
    const Vector3f br = center + dx * margin + dy * margin;
    
    const float minx = min(tl(0), min(tr(0), min(bl(0), br(0))));
    const float maxx = max(tl(0), max(tr(0), max(bl(0), br(0))));
    const float miny = min(tl(1), min(tr(1), min(bl(1), br(1))));
    const float maxy = max(tl(1), max(tr(1), max(bl(1), br(1))));
    
    const int margin2 = 2; // 1 should be enough;
    if (minx < margin2 || m_pmmvps.m_photoSet.getWidth(index, level) - 1 - margin2 <= maxx ||
        miny < margin2 || m_pmmvps.m_photoSet.getHeight(index, level) - 1 -margin2 <= maxy) {
        return -1;
    }
    
    return 0;
}

void Optim::normalize(vector<Vector3f>& tex) {
    const int sz = static_cast<int>(tex.size());
    
    Vector3f ave = Vector3f::Zero();
    for (int i = 0; i < sz; ++i) {
        ave += tex[i];
    }
    ave /= sz;
    
    float ssd = 0.0f;
    Vector3f diff;
    for (int i =0; i < sz; ++i) {
        diff = tex[i] - ave;
        ssd += diff.squaredNorm();
    }
    float msd = sqrtf(ssd / (3 * sz));
    
    if (msd == 0.0f) {
        msd = 1.0f;
    }
    for (int i = 0; i < sz; ++i) {
        tex[i] = (tex[i] - ave).eval() / msd;
    }
}

void Optim::computeWeights(const Patch& patch) {
    computeUnits(patch, m_weights);
    for (int i = 1; i < static_cast<int>(m_weights.size()); ++i) {
        m_weights[i] = std::min(1.0f, m_weights[0] / m_weights[i]);
    }
    m_weights[0] = 1.0f;
}

//-------------------------------------------------
// trash code
//-------------------------------------------------
// normalize only scale for each image, not used
/*
void Optim::normalize(vector<vector<Vector3f> >& texs, const int sz) {
    Vector3f ave = Vector3f::Zero();
    int denom = 0;
    
    vector<Vector3f> rgbs;
    rgbs.resize(sz); // there might be some empty elements in rgbs
    for (int i = 0; i < sz; ++i) {
        if (texs[i].empty()) {
            continue;
        }
        
        int count = 0;
        while (count <= static_cast<int>(texs[i].size())) {
            rgbs[i] += texs[i][count++];
        }
        rgbs[i] /= static_cast<int>(texs[i].size()); // count - 1?
        ave += rgbs[i];
        denom++;
    }
    
    if (denom == 0) {
        return;
    }
    ave /= denom;
    
    // scale all the colors;
    for (int i = 0; i < sz; ++i) {
        if (texs[i].empty()) {
            continue;
        }
        Vector3f scale;
        for (int j = 0; j < 3; ++j) {
            if (rgbs[i][j] != 0.0f) { // what if it's 0.0f?
                scale[j] = ave[j] / rgbs[i][j];
            }
        }
        
        int count = 0;
        while (count < static_cast<int>(texs[i].size())) {
            texs[i][count++].array() *= scale; // need to test
        }
    }
}
*/