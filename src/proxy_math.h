#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>
namespace nativehighlight::detail {
struct Spatial { int64_t position[3]; float rotation[4]; float scale[3]; uint32_t pad; };
static_assert(sizeof(Spatial)==56);
inline bool PerspectiveProxy(const Spatial& eye, Spatial& output, float modelRadius, float& factor) {
    double norm = 0;
    for (float q : eye.rotation) { if (!std::isfinite(q)) return false; norm += q*q; }
    if (std::fabs(norm - 1.0) > 0.01) return false;
    float x=eye.rotation[0], y=eye.rotation[1], z=eye.rotation[2], w=eye.rotation[3];
    double forward[3] = {2.0*(x*z+y*w), 2.0*(y*z-x*w), 1.0-2.0*(x*x+y*y)};
    double depth = 0;
    float maxScale = 0;
    for (unsigned j=0;j<3;++j) {
        double delta = (output.position[j] / 4294967296.0) - (eye.position[j] / 4294967296.0);
        if (!std::isfinite(delta) || std::fabs(delta)>5000 || !std::isfinite(output.scale[j])) return false;
        depth += delta*forward[j];
        maxScale = (std::max)(maxScale,std::fabs(output.scale[j]));
    }
    double radius = modelRadius*maxScale;
    if (!std::isfinite(radius) || radius<0.05 || radius>20 || depth<=radius+0.2) return false;
    factor=static_cast<float>((std::min)(0.25,0.15/(depth-radius)));
    if (!(factor>0 && factor<=0.25f)) return false;
    for (unsigned j=0;j<3;++j) {
        double origin=eye.position[j]/4294967296.0;
        double point=output.position[j]/4294967296.0;
        output.position[j]=static_cast<int64_t>((origin+(point-origin)*factor)*4294967296.0);
        output.scale[j]*=factor;
    }
    return true;
}
}
