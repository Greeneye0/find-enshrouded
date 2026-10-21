// find.ini in the game folder. Written with the defaults when missing.
#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include "config.h"
#include "log.h"

namespace {

constexpr char kFile[] = "find.ini";
constexpr char kDefaultHotkey[] = "Shift+F";

std::string Trim(std::string s)
{
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace(static_cast<unsigned char>(s[i])))
        ++i;
    return s.substr(i);
}

std::string Upper(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

// "F" -> 'F', "7" -> '7', "F6" -> VK_F6, plus a few named keys.
int KeyCode(const std::string& k)
{
    if (k.size() == 1 && (isalpha(static_cast<unsigned char>(k[0])) || isdigit(static_cast<unsigned char>(k[0]))))
        return k[0];
    if (k.size() >= 2 && k[0] == 'F' && isdigit(static_cast<unsigned char>(k[1]))) {
        int n = atoi(k.c_str() + 1);
        if (n >= 1 && n <= 24)
            return VK_F1 + n - 1;
    }
    static const struct {
        const char* name;
        int vk;
    } named[] = {{"TAB", VK_TAB}, {"SPACE", VK_SPACE}, {"INSERT", VK_INSERT}, {"HOME", VK_HOME},
                 {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}, {"DELETE", VK_DELETE},
                 {"BACKSPACE", VK_BACK}};
    for (const auto& n : named)
        if (k == n.name)
            return n.vk;
    return 0;
}

void WriteDefaults()
{
    FILE* f = fopen(kFile, "w");
    if (!f)
        return;
    fputs("; Find - search your base's chests for an item or a chest name.\n"
          "; hotkey: the key that opens the search. A letter, digit or F1-F12, optionally with\n"
          ";         Shift+, Ctrl+ and/or Alt+ in front. Examples: Shift+F, Ctrl+F, Alt+G, F6\n"
          "hotkey = Shift+F\n",
          f);
    fclose(f);
}

} // namespace

bool ParseHotkey(const char* text, Hotkey& out)
{
    out = {};
    std::string rest = Upper(Trim(text));
    size_t plus;
    while ((plus = rest.find('+')) != std::string::npos && plus + 1 < rest.size()) {
        std::string mod = Trim(rest.substr(0, plus));
        rest = Trim(rest.substr(plus + 1));
        if (mod == "SHIFT")
            out.shift = true;
        else if (mod == "CTRL" || mod == "CONTROL")
            out.ctrl = true;
        else if (mod == "ALT")
            out.alt = true;
        else
            return false;
    }
    out.key = KeyCode(rest);
    return out.key != 0;
}

Config LoadConfig()
{
    Config c;
    ParseHotkey(kDefaultHotkey, c.hotkey);
    c.hotkeyText = kDefaultHotkey;
    FILE* f = fopen(kFile, "r");
    if (!f) {
        WriteDefaults();
        return c;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s = Trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#')
            continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = Trim(s.substr(0, eq)), value = Trim(s.substr(eq + 1));
        for (char& ch : key)
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        if (key == "hotkey") {
            Hotkey h;
            if (ParseHotkey(value.c_str(), h)) {
                c.hotkey = h;
                c.hotkeyText = value;
            } else {
                Log("find.ini: hotkey '%s' not understood, using %s", value.c_str(), kDefaultHotkey);
            }
        }
    }
    fclose(f);
    return c;
}
