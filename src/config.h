#pragma once
#include <string>

struct Hotkey {
    int key = 0; // virtual-key code
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
};

// How long the chests of a search glow.
struct GlowTimes {
    float glowSeconds = 180.0f;  // at most this long after a search (until the next search)
    float nearMeters = 2.5f;     // coming this close to a chest counts as reaching it...
    float nearSeconds = 3.0f;    // ...and its glow ends this long after (or after opening it)
};

struct Config {
    Hotkey hotkey;
    std::string hotkeyText; // as written in find.ini, for the log
    GlowTimes glow;
};

// "Shift+F", "Ctrl+Alt+G", "F6"; false if not understood.
bool ParseHotkey(const char* text, Hotkey& out);

// "180", "2.5"; false unless a number in [lo, hi].
bool ParseNumber(const char* text, float lo, float hi, float& out);

// Reads find.ini from the game folder, writing one with the defaults if it is missing and
// adding settings an older find.ini lacks.
Config LoadConfig();
