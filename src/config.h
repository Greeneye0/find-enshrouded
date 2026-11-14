#pragma once
#include <string>
#include "../../common/input_binds.h"

// The keys and controller buttons of the search (common/input_binds.h).
struct Bindings {
    greeneye::input::Binding open, confirm, close, next, previous, up, down, exact;
};

// How long the chests of a search glow.
struct GlowTimes {
    float glowSeconds = 180.0f;  // at most this long after a search (until the next search)
    float nearMeters = 2.5f;     // coming this close to a chest counts as reaching it...
    float nearSeconds = 3.0f;    // ...and its glow ends this long after (or after opening it)
};

struct Config {
    Bindings keys;
    GlowTimes glow;
    bool exactMatch = false; // only names holding the whole text, not its words one by one
};

// Reads the keys from find.ini again when it changed (checked at most every 2 s); true if it did.
bool RefreshBindings(Bindings& keys);

// "180", "2.5"; false unless a number in [lo, hi].
bool ParseNumber(const char* text, float lo, float hi, float& out);

// Reads find.ini from the game folder, writing one with the defaults if it is missing and
// adding settings an older find.ini lacks.
Config LoadConfig();

// Remembers the dialog's Exact match setting in find.ini.
void SaveExactMatch(bool on);
