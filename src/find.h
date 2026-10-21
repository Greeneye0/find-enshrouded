#pragma once
#include "config.h"

namespace find {
// Hooks the in-game UI (the search dialog, on the hotkey) and the HUD's marker layer (chest markers).
bool Init(const Hotkey& hotkey);
}
