#pragma once
#include <cstdint>

namespace nativehighlight {
struct Target {
    uint32_t entity;
    float position[3];
    float alpha;
};
struct Status {
    uint32_t size, ready, requested, applied, pulsing;
};
bool Init();
bool GetStatus(Status* out);
// UI thread publishes intent; only the engine's render-system thread mutates models.
void Publish(const Target* targets, unsigned count);
void Clear();
void DrawSilhouettes();
void SetStressTest(bool enabled);
}
