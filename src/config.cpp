// find.ini in the game folder. Written with the defaults when missing.
#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "config.h"
#include "log.h"
#include "../../common/greeneye.h"

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

constexpr char kHeader[] = "; Find - search your base's chests for an item or a chest name.\n";
constexpr char kHotkeyText[] =
    "; hotkey: the key that opens the search. A letter, digit or F1-F12, optionally with\n"
    ";         Shift+, Ctrl+ and/or Alt+ in front. Examples: Shift+F, Ctrl+F, Alt+G, F6\n"
    "hotkey = Shift+F\n";

// The glow settings: name, default text, range, comment, and where the value goes.
struct Number {
    const char* key;
    const char* text;
    float lo, hi;
    const char* comment;
    float GlowTimes::*field;
};
const Number kNumbers[] = {
    {"glow_seconds", "180", 10, 3600,
     "; glow_seconds: the found chests glow at most this long (10-3600), or until your next search\n",
     &GlowTimes::glowSeconds},
    {"near_meters", "2.5", 0.5f, 20,
     "; near_meters: coming this close to a found chest counts as reaching it (0.5-20)\n",
     &GlowTimes::nearMeters},
    {"near_seconds", "3", 0, 60,
     "; near_seconds: a chest stops glowing this long after you reach or open it (0-60)\n",
     &GlowTimes::nearSeconds},
};

void WriteNumber(FILE* f, const Number& n)
{
    fprintf(f, "%s%s = %s\n", n.comment, n.key, n.text);
}

void WriteDefaults()
{
    FILE* f = fopen(greeneye::DataPath(kFile), "w");
    if (!f)
        return;
    fputs(kHeader, f);
    fputs(kHotkeyText, f);
    for (const Number& n : kNumbers)
        WriteNumber(f, n);
    fclose(f);
}

} // namespace

bool ParseNumber(const char* text, float lo, float hi, float& out)
{
    std::string s = Trim(text);
    if (s.empty())
        return false;
    char* end = nullptr;
    double v = strtod(s.c_str(), &end);
    if (*end || !(v >= lo && v <= hi))
        return false;
    out = static_cast<float>(v);
    return true;
}

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
    FILE* f = fopen(greeneye::DataPath(kFile), "r");
    if (!f) {
        WriteDefaults();
        return c;
    }
    bool seen[sizeof(kNumbers) / sizeof(kNumbers[0])] = {};
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
        for (size_t i = 0; i < sizeof(kNumbers) / sizeof(kNumbers[0]); ++i) {
            const Number& n = kNumbers[i];
            if (key != n.key)
                continue;
            seen[i] = true;
            float v;
            if (ParseNumber(value.c_str(), n.lo, n.hi, v))
                c.glow.*n.field = v;
            else
                Log("find.ini: %s '%s' not a number from %g to %g, using %s", n.key, value.c_str(), n.lo, n.hi,
                    n.text);
        }
    }
    fclose(f);
    // An older find.ini: add the settings it lacks, with their defaults, so they can be found.
    FILE* add = nullptr;
    for (size_t i = 0; i < sizeof(kNumbers) / sizeof(kNumbers[0]); ++i) {
        if (seen[i])
            continue;
        if (!add && !(add = fopen(greeneye::DataPath(kFile), "a")))
            break;
        WriteNumber(add, kNumbers[i]);
    }
    if (add)
        fclose(add);
    return c;
}
