#pragma once
#include <cstdint>
#include "config.h"

namespace find {
// Hooks the in-game UI (the search dialog, on the hotkey) and the HUD's marker layer (chest markers).
bool Init(const Config& config);
// The chests and stations the search lights right now: world positions (x, y, z per target) and entity ids, up to
// max of each (either may be null); returns how many there are. generation changes with every new search.
int FoundTargets(float* xyz, uint32_t* entities, int max, uint32_t* generation);
}
