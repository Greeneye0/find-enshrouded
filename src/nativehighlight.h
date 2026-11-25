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
// find.ini debug_fps_test = 1: parts of the glow switched off one at a time while the frame rate is measured.
enum DebugOff : unsigned {
    kOffPulse = 1,     // a steady gold, the palette colour not changed every frame
    kOffOutline = 2,   // no outline drawn on the HUD
    kOffCompass = 4,   // no compass markers
    kOffCrafters = 8,  // the NPCs and stations of the search not lit (find.cpp leaves them out)
    kOffChests = 16,   // the chests of the search not lit
    kOffGold = 32,     // the game's highlight group 6 with its own colour instead of Find's group 7
    kOffGroup = 64,    // no highlight group set at all: nothing lit in the 3D scene
    kOffAll = 128,     // nothing published, nothing drawn
    kOffCopies = 256,  // no 3D copies (through_walls = glow only)
};
void SetDebugOff(unsigned mask);
unsigned DebugOff();
// What the render hooks did since the last call: seconds in Find's model update, how many updates ran and how many
// iterator ends were seen, what is lit and how many copies stand.
struct DebugTimes { double updateSeconds; long updates, iteratorEnds; unsigned lit; long copies; };
DebugTimes TakeDebugTimes();
// UI thread, each frame while something glows: the game client (its view matrix places the copies) and the player's feet.
void SetView(void* client, const float player[3]);
}
