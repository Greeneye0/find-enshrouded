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

std::string Trim(std::string s)
{
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace(static_cast<unsigned char>(s[i])))
        ++i;
    return s.substr(i);
}

constexpr char kHeader[] = "; Find - search your base's chests for an item or a chest name.\n";
constexpr char kHotkeyText[] =
    "; hotkey: the key that opens the search. A letter, digit, F1-F24, Enter, Tab, Mouse1-Mouse5 and\n"
    ";         so on, optionally with Shift+, Ctrl+ and/or Alt+ in front. Examples: Shift+F, Ctrl+F, F6\n"
    ";         A controller button goes after a comma: Pad: and any of A B X Y LB RB LT RT LS RS View\n"
    ";         Menu Up Down Left Right, joined with +. Default Pad:LB+X: hold LB, press X.\n"
    ";         NoPad turns the controller off, NoKeys the keyboard, None both.\n"
    ";         All keys here apply within two seconds; no restart needed.\n"
    "hotkey = Shift+F, Pad:LB+X\n";
// Added after find.ini first shipped, appended to a file that lacks them.
constexpr char kDialogKeysText[] =
    "; In the open search: find, close, move through the list. Same format as hotkey.\n"
    "; With a controller and nothing typed, Up and Down list everything in the base.\n"
    "confirm_key = Enter, Pad:A\n"
    "close_key = Esc, Pad:B\n"
    "next_key = Tab\n"
    "previous_key = Shift+Tab\n"
    "up_key = Up, Pad:Up\n"
    "down_key = Down, Pad:Down\n";
constexpr char kExactText[] =
    "; exact_key switches Exact match in the open search (also a button in it). exact_match = 1: only names\n"
    ";   holding the whole text; 0: also names with all its words (\"ring mana\": Ring of Mana), then some of them.\n"
    "exact_key = Ctrl+E, Pad:Y\n";
constexpr char kExactKey[] = "exact_match";

struct BindingSetting {
    const char* key;
    const char* defaultText;
    greeneye::input::Binding Bindings::* field;
};
constexpr BindingSetting kBindings[] = {
    {"hotkey", "Shift+F, Pad:LB+X", &Bindings::open},
    {"confirm_key", "Enter, Pad:A", &Bindings::confirm},
    {"close_key", "Esc, Pad:B", &Bindings::close},
    {"next_key", "Tab", &Bindings::next},
    {"previous_key", "Shift+Tab", &Bindings::previous},
    {"up_key", "Up, Pad:Up", &Bindings::up},
    {"down_key", "Down, Pad:Down", &Bindings::down},
    {"exact_key", "Ctrl+E, Pad:Y", &Bindings::exact},
};

void DefaultBindings(Bindings& keys)
{
    for (const BindingSetting& b : kBindings)
        keys.*b.field = greeneye::input::MakeBinding(b.defaultText);
}

// One line of find.ini, if it sets a key. The name given back tells the caller it was one.
bool ReadBindingLine(const std::string& key, const std::string& value, Bindings& keys)
{
    for (const BindingSetting& b : kBindings) {
        if (key != b.key)
            continue;
        std::string why;
        if (!greeneye::input::ParseBinding(value, greeneye::input::MakeBinding(b.defaultText), keys.*b.field, &why))
            Log("find.ini: %s: '%s' is not a key or button Find knows, using %s", b.key, why.c_str(), b.defaultText);
        return true;
    }
    return false;
}

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
    fputs(kDialogKeysText, f);
    for (const Number& n : kNumbers)
        WriteNumber(f, n);
    fprintf(f, "%s%s = 0\n", kExactText, kExactKey);
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

bool RefreshBindings(Bindings& keys)
{
    static greeneye::input::FileWatch watch;
    if (!watch.Changed(greeneye::DataPath(kFile)))
        return false;
    FILE* f = fopen(greeneye::DataPath(kFile), "r");
    if (!f)
        return false;
    Bindings fresh;
    DefaultBindings(fresh);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s = Trim(line);
        size_t eq = s.find('=');
        if (s.empty() || s[0] == ';' || s[0] == '#' || eq == std::string::npos)
            continue;
        std::string key = Trim(s.substr(0, eq));
        for (char& ch : key)
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        ReadBindingLine(key, Trim(s.substr(eq + 1)), fresh);
    }
    fclose(f);
    keys = fresh;
    return true;
}

Config LoadConfig()
{
    Config c;
    DefaultBindings(c.keys);
    FILE* f = fopen(greeneye::DataPath(kFile), "r");
    if (!f) {
        WriteDefaults();
        return c;
    }
    bool seen[sizeof(kNumbers) / sizeof(kNumbers[0])] = {};
    bool dialogKeysSeen = false, exactSeen = false;
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
        ReadBindingLine(key, value, c.keys);
        dialogKeysSeen = dialogKeysSeen || key == "confirm_key";
        exactSeen = exactSeen || key == "exact_key";
        if (key == kExactKey)
            c.exactMatch = value == "1" || _stricmp(value.c_str(), "on") == 0 || _stricmp(value.c_str(), "true") == 0;
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
    if (!dialogKeysSeen && (add || (add = fopen(greeneye::DataPath(kFile), "a"))))
        fputs(kDialogKeysText, add);
    if (!exactSeen && (add || (add = fopen(greeneye::DataPath(kFile), "a"))))
        fprintf(add, "%sexact_match = %d\n", kExactText, c.exactMatch ? 1 : 0);
    if (add)
        fclose(add);
    return c;
}

void SaveExactMatch(bool on)
{
    // Rewrites only the exact_match line (added at the end if missing); the rest stays as written.
    std::string out;
    bool done = false;
    if (FILE* f = fopen(greeneye::DataPath(kFile), "r")) {
        char buf[512];
        while (fgets(buf, sizeof(buf), f)) {
            std::string s = Trim(buf);
            size_t eq = s.find('=');
            std::string key = eq == std::string::npos ? "" : Trim(s.substr(0, eq));
            for (char& ch : key)
                ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            if (!done && key == kExactKey) {
                out += std::string(kExactKey) + " = " + (on ? "1" : "0") + "\n";
                done = true;
            } else {
                out += buf;
            }
        }
        fclose(f);
    }
    if (!done) {
        if (!out.empty() && out.back() != '\n')
            out += "\n";
        out += std::string(kExactKey) + " = " + (on ? "1" : "0") + "\n";
    }
    if (FILE* f = fopen(greeneye::DataPath(kFile), "w")) {
        fputs(out.c_str(), f);
        fclose(f);
    } else {
        Log("find.ini: could not save exact_match");
    }
}
