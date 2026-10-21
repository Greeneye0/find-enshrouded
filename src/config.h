#pragma once
#include <string>

struct Hotkey {
    int key = 0; // virtual-key code
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
};

struct Config {
    Hotkey hotkey;
    std::string hotkeyText; // as written in find.ini, for the log
};

// "Shift+F", "Ctrl+Alt+G", "F6"; false if not understood.
bool ParseHotkey(const char* text, Hotkey& out);

// Reads find.ini from the game folder, writing one with the defaults if it is missing.
Config LoadConfig();
