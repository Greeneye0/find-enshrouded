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

// The copy's transform from an eye and its view direction (unit vector): every point moved toward the eye by one
// factor keeps its projection, the copy's nearest point `nearest` metres from the eye (never moved away). As the map
// mod's G G creature copies (UnlockMapMarkers chest_glow.inl), which the user checked in game on 2026-11-11.
inline bool PerspectiveCopy(const double eye[3], const double forward[3], double nearest, Spatial& out, float modelRadius) {
    double depth = 0;
    float maxScale = 0;
    for (int j = 0; j < 3; ++j) {
        const double delta = out.position[j] / 4294967296.0 - eye[j];
        if (!std::isfinite(delta) || std::fabs(delta) > 5000 || !std::isfinite(out.scale[j])) return false;
        depth += delta * forward[j];
        maxScale = (std::max)(maxScale, std::fabs(out.scale[j]));
    }
    const double radius = modelRadius * maxScale;
    if (!std::isfinite(radius) || radius < 0.05 || radius > 20 || depth <= radius + 0.2) return false;
    const double factor = (std::min)(1.0, nearest / (depth - radius));
    if (!(factor > 0 && factor <= 1.0)) return false;
    for (int j = 0; j < 3; ++j) {
        const double origin = eye[j], point = out.position[j] / 4294967296.0;
        out.position[j] = static_cast<int64_t>((origin + (point - origin) * factor) * 4294967296.0);
        out.scale[j] = static_cast<float>(out.scale[j] * factor);
    }
    return true;
}

// The eye and view direction from a view matrix (the client's, client + 0x1d20): x_view = sum_j v[j * 4 + i] * x_j +
// v[12 + i], so the eye is -R^T t and forward the depth row.
inline bool EyeFromView(const float* v, double eye[3], double forward[3]) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(v[i])) return false;
    for (int j = 0; j < 3; ++j) {
        eye[j] = -(double(v[j * 4 + 0]) * v[12] + double(v[j * 4 + 1]) * v[13] + double(v[j * 4 + 2]) * v[14]);
        forward[j] = v[j * 4 + 2];
    }
    const double len = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (!(len > 0.5 && len < 2.0)) return false;
    for (int j = 0; j < 3; ++j) forward[j] /= len;
    return std::fabs(eye[0]) < 1e6 && std::fabs(eye[1]) < 1e6 && std::fabs(eye[2]) < 1e6;
}

// The same from the renderer's camera (renderer + 0x40 -> +0x8d0); last frame's while the model systems run.
inline bool EyeFromCamera(const Spatial& cam, double eye[3], double forward[3]) {
    double norm = 0;
    for (float q : cam.rotation) {
        if (!std::isfinite(q)) return false;
        norm += double(q) * q;
    }
    if (std::fabs(norm - 1.0) > 0.01) return false;
    const float x = cam.rotation[0], y = cam.rotation[1], z = cam.rotation[2], w = cam.rotation[3];
    forward[0] = 2.0 * (x * z + y * w), forward[1] = 2.0 * (y * z - x * w), forward[2] = 1.0 - 2.0 * (x * x + y * y);
    for (int j = 0; j < 3; ++j) eye[j] = cam.position[j] / 4294967296.0;
    return true;
}
}
