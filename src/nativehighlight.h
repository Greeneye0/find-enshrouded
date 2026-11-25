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
void SetDevSwitch(int which, bool on); // dev: 1 no gold pulse, 3 fps log, 4 move copies every frame (old way)
// find.ini through_walls: 0 glow (3D copies), 1 icon (no copies; Find draws an icon), 2 HUD silhouette
void SetThroughWalls(int mode);
int ThroughWalls();
// find.ini debug_deposit_glow = 1, before Init: everything as Quick Deposit lights its chests, in gold. No 3D copies and
// no staging hook; the HUD silhouette through walls; the colour through the two palette calls, not the converter.
void SetDepositStyle(bool on);
// UI thread, each frame while something glows: the game client (its view matrix places the copies) and the player's feet.
void SetView(void* client, const float player[3]);
}
