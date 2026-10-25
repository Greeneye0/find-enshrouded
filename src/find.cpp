#include "hotapi.h"
// Hotkey (Shift+F by default, find.ini): search the chests of the base you stand in for an
// item or a chest name, with suggestions while typing, and make every matching chest glow
// (build 1269579).
//
// 140c4e0c0 doPlayStateUi(): the in-game UI, once per frame; the search dialog is drawn after
//           the game's own UI.
// 140c32210 the HUD's waypoint marker, inside the spatial elements window; chest glows are
//           drawn right after it, in the same window.
// 140845600 UiCommandLoop(app): every frame; the game client = *(*(app + 0x35280) + 0x1b8)
//           (a different object than the UI's client at *(**(G + 0xd0) + 0x38)). Quick
//           Deposit hooks it too: found by the code after its first 13 bytes, chained.
// Base and chests as in Quick Deposit: client + 0x47d4e0 world; world + 0x2c08 player
// position (int64, 32 fraction bits); world + 0x3eb8 base registry {+0x24 count, +0x30 bases
// of 0x130: +0x00 magic chest ids, +0x08 count, +0xf0 min, +0x100 max}.
// Item names: ItemInfo + 0x48 is the localized name's key.
// 140852760 UiDispatch(gameClient, cmd): command 0x01 <screen> opens a HUD screen; the screen
//           shown is *(uiClient + 0x1cd8): 1 playing, 9 inventory. The text dialog takes the
//           keyboard and only a screen change gives it back, so the search opens the
//           inventory screen behind it while playing (which also gives it the mouse) and
//           goes back to playing when it closes.
// 140b8c4c0 DualInventory(mode): the window of an opened chest (player + chest inventories),
//           every frame it is open. While it draws, the found items' icons light up; opening
//           it counts as reaching the nearest glowing chest.
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "config.h"
#include "ecsnames.h"
#include "find.h"
#include "inventory.h"
#include "log.h"
#include "nativehighlight.h"
#include "scan.h"
#include "strings.h"
#include "ui.h"

namespace find {
namespace {

using PlayStateFn = void (*)(void* self);
using SpatialMarkersFn = void (*)(void* state, void* arg);
using CommandLoopFn = void (*)(void* app);
using UiDispatchFn = void (*)(void* client, uint8_t* cmd);
using DualInventoryFn = void (*)(char mode);

PlayStateFn g_originalPlayState = nullptr;
SpatialMarkersFn g_originalSpatial = nullptr;
CommandLoopFn g_originalLoop = nullptr;
UiDispatchFn g_dispatch = nullptr;
DualInventoryFn g_originalDual = nullptr;
void* volatile g_app = nullptr;

constexpr size_t kClientWorld = 0x47d4e0;
constexpr size_t kClientPlayer = 0x466b44;
constexpr size_t kWorldPlayerPos = 0x2c08;
constexpr size_t kWorldPermissions = 0x2c58;
constexpr size_t kWorldBases = 0x3eb8;
constexpr size_t kRegPermissions = 0x10;
constexpr size_t kRegBaseCount = 0x24;
constexpr size_t kRegBases = 0x30;
constexpr size_t kBaseStride = 0x130;
constexpr size_t kBaseMin = 0xF0;
constexpr size_t kBaseMax = 0x100;
constexpr size_t kItemName = 0x48;
constexpr float kFixedToMeters = 1.0f / 4294967296.0f;
constexpr int kMaxChests = 256;

constexpr int kVisibleSuggestions = 7; // the dialog shows up to 8 lines
constexpr int kMaxSuggestions = 60;     // the list scrolls through this many
constexpr size_t kItemRarity = 0x17;    // ItemInfo: ItemRarity (1 uncommon .. 4 legendary)
constexpr size_t kItemCaption = 0x58;   // ItemInfo: CaptionLocaTag, the type line ("One-Handed Axe")
constexpr size_t kItemCategory = 0xcc;  // ItemInfo: ItemCategory
constexpr uint8_t kCategoryBlueprints = 10;
constexpr size_t kItemPerkIds = 0x658;  // ItemInfo: PerkId[5], the perks its upgrades unlock
constexpr size_t kUiClientPerks = 0x466800; // UI client: the perk registry (as the item tooltip uses it)
// A glow lasts until the next search, at most glow_seconds (find.ini), or until you come within
// near_meters (3D) and then near_seconds more.
constexpr float kLeftMeters = 120.0f;                // farther than this: you left the base, the glow ends
constexpr ULONGLONG kMinGlowMs = 2000;               // chests right next to you still show for a moment
constexpr ULONGLONG kFadeOutMs = 600;
constexpr float kChestCentre = 0.5f; // chest origin is at its bottom; chests stack 1 m apart
constexpr size_t kItemIcon = 0x56c;   // ItemInfo: iconImage, a 16-byte UiImageReference
constexpr uint8_t kCmdOpenScreen = 0x01;
constexpr size_t kUiScreen = 0x1cd8;  // UI client: the HUD screen shown
constexpr uint8_t kScreenPlaying = 1;
constexpr uint8_t kScreenInventory = 9;
// HudScreenType Collection: with nothing to collect it draws nothing and gives the UI the mouse,
// so the search sits over the world (as Auto Loot's panel does). Rename (47) closes itself,
// InfoPopup (49) shows the news.
constexpr uint8_t kScreenBehindSearch = 24;
constexpr uint8_t kScreenSystemMenu = 2;
constexpr size_t kWindowMouseScale = 0x68; // game window object: raw mouse -> camera factor
constexpr size_t kDispatchCmdSize = 0x410;
constexpr float kOpenedChestMeters = 5.0f;   // an opened chest window belongs to the nearest glow this close
constexpr ULONGLONG kChestWindowGapMs = 400; // a longer gap between chest window frames: opened anew
constexpr uint32_t kIconColour = 0xFF3CC8FF; // bright gold (0xAABBGGRR)
constexpr ULONGLONG kIconPulseMs = 1200;
constexpr ULONGLONG kIconShowMs = 5000; // found items light up for this long, the first time each found chest opens

template <class T> T& At(void* base, size_t offset)
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

// The game client (world, player, chests), learned from UiCommandLoop.
void* GameClient()
{
    void* app = g_app;
    if (!app)
        return nullptr;
    void* ui = At<void*>(app, 0x35280);
    return ui ? At<void*>(ui, 0x1b8) : nullptr;
}

// --- glows ---

struct Glow {
    float pos[3];
    uint32_t chest;
    ULONGLONG arrived; // 0 until you came within g_arrivedMeters
    bool shown;        // g_found: its items already lit up once in its window
};
std::vector<Glow> g_glows;
std::vector<Glow> g_found; // every chest of the search, kept after its glow ends
ULONGLONG g_glowsUntil = 0;
ULONGLONG g_glowsStart = 0;
ULONGLONG g_lastGlowDraw = 0;

bool g_nativeReady = false;

// --- the search ---

enum class Kind : uint8_t {
    Item,  // one item
    Chest, // a chest's own name
    Group, // every item of a category, item type or rarity ("Consumables", "Legendary")
};

struct Entry {
    std::string name; // as the game shows it
    std::string lower;
    Kind kind;
    uint32_t itemId;
    uint32_t total;   // items in the base
    std::vector<uint32_t> chests;
    uint32_t kinds = 0; // Group: how many different items
    std::vector<uint32_t> items; // Group: those items
    uint8_t group = 0;           // Group: what kind (GroupCategory...)
    uint8_t rarity = 0;          // Group of a rarity: which (1 uncommon .. 4 legendary)
};

struct Index {
    bool ok = false;
    bool inBase = false; // !ok only because the base has no storage chests
    std::string why;
    std::vector<Entry> entries;
    std::map<uint32_t, std::vector<float>> positions; // chest -> x, y, z
    int chestCount = 0;
};

Hotkey g_hotkey;
ULONGLONG g_glowLifetimeMs = 180000;
float g_arrivedMeters = 2.5f;
ULONGLONG g_afterArrivalMs = 3000;
ui::Dialog* g_dialog = nullptr;
const char* volatile g_step = "";
bool g_open = false;
bool g_hotPaused = false;
volatile LONG64 g_devHoldUntil = 0;
bool g_openedInventory = false; // the search opened the inventory screen and closes it again
ULONGLONG g_openedAt = 0;
bool g_screenLogged = false;
bool g_screenReached = false;
std::vector<uint32_t> g_foundItems; // the items of the last search, lit in an opened chest
ULONGLONG g_chestWindowAt = 0;      // the last frame the chest window drew
ULONGLONG g_lastIconLog = 0;
ULONGLONG g_windowOpenedAt = 0;
bool g_lightThisWindow = false; // this chest window lights the found items (its first opening)
bool g_traced = false, g_tracePending = false;
bool g_keyWasDown = false;
Index g_index;
std::string g_lastText;      // the field as last seen
std::string g_typed;         // what the player typed (suggestions are for this)
std::vector<int> g_suggested; // entry indices, best first
int g_selected = -1;
int g_top = 0;                // the first suggestion shown (the list scrolls)
std::string g_notice;         // "nothing found" and the like, shown until the text changes
bool g_navDown[5] = {};       // Tab, Up, Down, left mouse button, Enter edges
ULONGLONG g_clickAt = 0;       // the last click on a suggestion
int g_hover = -1;              // the suggestion under the mouse
constexpr ULONGLONG kDoubleClickMs = 500;
ULONGLONG g_tabAt = 0;        // the field turns Tab into a space; that space is dropped
bool g_openPending = false;   // the hotkey was pressed; open once it is released
constexpr ULONGLONG kTabSpaceMs = 400;

bool GameFocused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool PlayerPosition(float out[3])
{
    void* client = GameClient();
    auto* world = client ? At<uint8_t*>(client, kClientWorld) : nullptr;
    if (!world)
        return false;
    for (int i = 0; i < 3; ++i)
        out[i] = static_cast<float>(At<int64_t>(world, kWorldPlayerPos + 8 * i)) * kFixedToMeters;
    return true;
}

// Lower case for any language (names follow the game's language: Ä/ä, Д/д).
std::string Lower(const std::string& s)
{
    if (s.empty())
        return s;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return s;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
    int m = WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
    std::string r(static_cast<size_t>(m), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), n, r.data(), m, nullptr, nullptr);
    return r;
}

std::string Trim(std::string s)
{
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace(static_cast<unsigned char>(s[i])))
        ++i;
    return s.substr(i);
}

// The storage chests of the base you stand in (as Quick Deposit finds them).
#ifdef FIND_DEV
bool g_devPlain = false; // @dev-plain: act as if the base listed no chests (a small flame altar)
#endif

bool BaseChests(std::vector<uint32_t>& out, std::string& why)
{
    void* client = GameClient();
    auto* world = client ? At<uint8_t*>(client, kClientWorld) : nullptr;
    inv::ForgetOnWorldChange(world);
    if (!world) {
        why = "not in a world";
        return false;
    }
    uint32_t handle = At<uint32_t>(client, kClientPlayer);
    uint32_t playerId = handle ? (handle & 0x3f) + 1 : 0;
    auto* reg = At<uint8_t*>(world, kWorldBases);
    if (!playerId || !reg || At<uint8_t*>(reg, kRegPermissions) != world + kWorldPermissions) {
        why = "player or base data not where expected - game version changed?";
        return false;
    }
    float pos[3];
    PlayerPosition(pos);
    uint8_t* base = nullptr;
    uint32_t baseCount = At<uint32_t>(reg, kRegBaseCount);
    for (uint32_t b = 0; b < baseCount && b < 1024 && !base; ++b) {
        uint8_t* cand = reg + kRegBases + b * kBaseStride;
        const float* lo = &At<float>(cand, kBaseMin);
        const float* hi = &At<float>(cand, kBaseMax);
        if (pos[0] >= lo[0] && pos[0] < hi[0] && pos[1] >= lo[1] && pos[1] < hi[1] && pos[2] >= lo[2] && pos[2] < hi[2])
            base = cand;
    }
    if (!base) {
        why = "not inside a base";
        return false;
    }
    auto* ids = At<uint32_t*>(base, 0x00);
    uint64_t n = At<uint64_t>(base, 0x08);
    std::vector<uint32_t> magic;
    for (uint64_t i = 0; ids && i < n && magic.size() < kMaxChests; ++i)
        if (ids[i])
            magic.push_back(ids[i]);
#ifdef FIND_DEV
    if (g_devPlain)
        magic.clear();
    {
        std::string ids;
        for (uint32_t m : magic)
            ids += " " + std::to_string(m);
        DevLog("  altar lists:%s", ids.c_str());
    }
#endif
    out = magic;
    if (!magic.empty()) {
        if (!inv::Ready(playerId, magic.data(), static_cast<int>(magic.size())))
            return true;
        // The altar lists planted bushes too. Left in, they glowed as chests, and as "magic
        // chests" they let crafting stations past KeepStorageChests.
        magic.resize(inv::DropPlants(client, magic.data(), static_cast<int>(magic.size())));
        out = magic;
    }
    if (magic.empty()) {
        // A small flame altar lists no crafting-storage chests: find the plain chests themselves.
        std::vector<uint32_t> plain(kMaxChests);
        int np = !inv::Ready(playerId, nullptr, 0) ? 0 : inv::FindPlainChests(&At<float>(base, kBaseMin), &At<float>(base, kBaseMax), plain.data(), kMaxChests);
        np = inv::DropPlants(client, plain.data(), np);
        out.assign(plain.begin(), plain.begin() + np);
        if (out.empty()) {
            why = "no storage chests in this base";
            return false;
        }
        return true;
    }
    std::vector<uint32_t> found(kMaxChests);
    int nf = inv::FindContainers(&At<float>(base, kBaseMin), &At<float>(base, kBaseMax), found.data(), kMaxChests);
    nf = inv::KeepStorageChests(magic.data(), static_cast<int>(magic.size()), found.data(), nf);
    for (int i = 0; i < nf; ++i)
        if (std::find(out.begin(), out.end(), found[i]) == out.end())
            out.push_back(found[i]);
    return true;
}

std::string ItemName(uint32_t itemId)
{
    const uint8_t* info = inv::ItemInfo(itemId);
    std::string name = info ? ui::LocaKey(info + kItemName) : std::string();
    if (name.empty())
        name = inv::Describe(itemId).name;
    return name;
}

// An NPC's summoning staff is named after the NPC ("Fenrig Mills, Jr."), but the game shows it as
// a Summoning Staff: "Summoning Staff (Fenrig Mills, Jr.)" finds it by either.
constexpr uint32_t kSummoningStaffItem = 1182285312; // NpcSummoner_Intro: "Summoning Staff"

std::string SearchName(uint32_t itemId)
{
    std::string name = ItemName(itemId);
    if (_strnicmp(inv::Describe(itemId).raw, "NPC_", 4) != 0)
        return name;
    std::string staff = inv::ItemInfo(kSummoningStaffItem) ? ItemName(kSummoningStaffItem) : std::string();
    if (staff.empty() || staff.rfind("item ", 0) == 0)
        staff = "Summoning Staff";
    return staff + " (" + name + ")";
}

// Item categories as the search offers them (keen::ItemCategory order); empty: not offered.
enum Group : uint8_t { GroupNone, GroupCategory, GroupRarity, GroupPerk, GroupItemType };

// Category and rarity names as the game shows them, in its language. The UI texts
// (*(G + 200)): + 0x10 FbUiLocaLinks; its crafting texts (+ 0xb030) hold the item categories at
// + 0xe0 (FbUiLocaCraftingCategories: equipment + 0x20, weapons + 0x30, tools + 0x40,
// buildTools + 0x50, consumables + 0xa0, ammunition + 0xb0, materials + 0xd0, blueprints + 0xe0,
// currency + 0x110, collectible + 0x120, petFood + 0x130); its RPG terms (+ 0x40) hold the
// rarities at + 0x1d90 (common, uncommon, rare, epic, legendary; 0x10 apart).
constexpr size_t kLocaCategories = 0x10 + 0xb030 + 0xe0;
constexpr size_t kLocaRarities = 0x10 + 0x40 + 0x1d90;

std::string CategoryLabel(uint8_t category)
{
    // keen::ItemCategory -> offset in FbUiLocaCraftingCategories (0: Find's own text)
    static const uint16_t offsets[] = {0, 0x20, 0x30, 0x40, 0, 0, 0x50, 0xa0, 0xb0, 0xd0, 0xe0, 0x110, 0x120, 0, 0x130, 0, 0};
    static const strings::Id own[] = {strings::Count, strings::Count, strings::Count, strings::Count, strings::Instruments,
                                      strings::FishingRods, strings::Count, strings::Count, strings::Count, strings::Count,
                                      strings::Count, strings::Count, strings::Count, strings::AnimalFood, strings::Count,
                                      strings::WeaponGems, strings::Count};
    static const char* const english[] = {"", "Equipment", "Weapons", "Tools", "Instruments", "Fishing Rods",
                                          "Building Tools", "Consumables", "Ammunition", "Materials", "Blueprints",
                                          "Currency", "Collectibles", "Animal Food", "Pet Food", "Weapon Gems", ""};
    if (category >= sizeof(offsets) / sizeof(offsets[0]) || !*english[category])
        return std::string();
    if (offsets[category]) {
        ui::Text t = ui::Loca(kLocaCategories + offsets[category]);
        if (t.utf8 && t.len)
            return Trim(std::string(t.utf8, t.len));
    }
    if (own[category] != strings::Count)
        return strings::Get(own[category]);
    return english[category];
}

std::string RarityLabel(uint8_t rarity)
{
    static const char* const english[] = {"", "Uncommon", "Rare", "Epic", "Legendary"};
    if (rarity == 0 || rarity >= sizeof(english) / sizeof(english[0]))
        return std::string();
    ui::Text t = ui::Loca(kLocaRarities + 0x10 * rarity);
    if (t.utf8 && t.len)
        return Trim(std::string(t.utf8, t.len));
    return english[rarity];
}

bool ReadByte(const uint8_t* p, uint8_t& out)
{
    __try {
        out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A perk record by id: the registry is a hash map (+0x98 occupied bits, +0xa0 capacity, +0xb8
// u32 keys, +0xd0 record pointers, +0xe4 count, linear probing), read as 140e7ea90 does. The
// record starts with the perk's name (a text key, like an item's).
const uint8_t* PerkRecord(uint32_t id)
{
    auto* client = static_cast<uint8_t*>(ui::Client());
    if (!client || !id)
        return nullptr;
    __try {
        uint8_t* reg = *reinterpret_cast<uint8_t**>(client + kUiClientPerks);
        if (!reg || *reinterpret_cast<uint32_t*>(reg + 0xe4) == 0)
            return nullptr;
        uint64_t cap = *reinterpret_cast<uint64_t*>(reg + 0xa0);
        auto* used = *reinterpret_cast<uint64_t**>(reg + 0x98);
        auto* keys = *reinterpret_cast<uint32_t**>(reg + 0xb8);
        auto* values = *reinterpret_cast<uint8_t***>(reg + 0xd0);
        if (!cap || (cap & (cap - 1)) || cap > (1u << 20))
            return nullptr;
        uint64_t i = id & (cap - 1);
        for (uint64_t n = 0; n < cap && ((used[i >> 6] >> (i & 63)) & 1); ++n) {
            if (keys[i] == id)
                return values[i];
            i = (i + 1) & (cap - 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

bool ReadPerkIds(const uint8_t* info, uint32_t out[5])
{
    __try {
        memcpy(out, info + kItemPerkIds, 5 * sizeof(uint32_t));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::map<uint32_t, std::string> g_perkNames; // perk id -> name (empty: unknown)

std::string PerkName(uint32_t id)
{
    auto it = g_perkNames.find(id);
    if (it != g_perkNames.end())
        return it->second;
    const uint8_t* perk = PerkRecord(id);
    std::string name = perk ? Trim(ui::LocaKey(perk)) : std::string();
    if (name.size() >= 48)
        name.clear();
    if (!name.empty()) // unknown ones are asked again next time
        g_perkNames[id] = name;
    return name;
}

// The groups an item belongs to: its category, rarity, perks and type line.
std::vector<std::pair<std::string, uint8_t>> ItemGroups(uint32_t itemId, const std::string& itemName)
{
    std::vector<std::pair<std::string, uint8_t>> groups;
    const uint8_t* info = inv::ItemInfo(itemId);
    uint8_t category, rarity;
    if (!info || !ReadByte(info + kItemCategory, category) || !ReadByte(info + kItemRarity, rarity))
        return groups;
    if (std::string c = CategoryLabel(category); !c.empty())
        groups.push_back({c, GroupCategory});
    if (std::string r = RarityLabel(rarity); !r.empty())
        groups.push_back({r, static_cast<uint8_t>(GroupRarity + 0x10 * rarity)}); // rarity in the high bits
    uint32_t perks[5];
    if (ReadPerkIds(info, perks)) {
        for (uint32_t id : perks) {
            std::string name = id ? PerkName(id) : std::string();
            if (!name.empty() &&
                std::find(groups.begin(), groups.end(), std::make_pair(name, uint8_t{GroupPerk})) == groups.end())
                groups.push_back({name, GroupPerk});
        }
    }
    std::string caption = Trim(ui::LocaKey(info + kItemCaption));
    if (!caption.empty() && caption.size() < 48 && Lower(caption) != Lower(itemName))
        groups.push_back({caption, GroupItemType});
    return groups;
}

// Every item and named chest of the base, built when the search opens.
Index BuildIndex()
{
    Index ix;
    std::vector<uint32_t> chests;
    g_step = "base chests";
    if (!BaseChests(chests, ix.why)) {
        ix.inBase = ix.why == "no storage chests in this base";
        return ix;
    }
    ix.ok = true;
    ix.chestCount = static_cast<int>(chests.size());
    void* client = GameClient();
    std::map<uint32_t, size_t> byItem;
    std::map<std::string, size_t> byChestName;
    std::map<std::string, size_t> byGroup;
    std::map<uint32_t, std::vector<size_t>> itemGroups;
    for (uint32_t chest : chests) {
        float p[3];
        if (!inv::Position(chest, p))
            continue;
        ix.positions[chest] = {p[0], p[1], p[2]};
        g_step = "chest name";
        char name[96] = {};
        if (inv::CustomName(client, chest, name, sizeof(name)) && name[0]) {
            std::string n = Trim(name);
            auto it = byChestName.find(Lower(n));
            if (it == byChestName.end()) {
                byChestName[Lower(n)] = ix.entries.size();
                ix.entries.push_back({n, Lower(n), Kind::Chest, 0, 0, {chest}});
            } else {
                ix.entries[it->second].chests.push_back(chest);
            }
        }
        g_step = "chest contents";
        inv::Owner owner;
        if (!inv::ReadOwner(chest, owner))
            continue;
        for (int pg = 0; pg < owner.pageCount; ++pg) {
            for (const inv::Stack& s : owner.pages[pg].slots) {
                if (!s.itemId || !s.count)
                    continue;
                auto it = byItem.find(s.itemId);
                size_t e;
                if (it == byItem.end()) {
                    g_step = "item name";
                    std::string n = SearchName(s.itemId);
                    e = ix.entries.size();
                    byItem[s.itemId] = e;
                    // A blueprint's own name is what it builds ("Single Wall Block 0,5m"), but the
                    // game shows it as the Construction Hammer: it is found only by its kind
                    // (Blueprints), never by that name.
                    uint8_t category = 0;
                    const uint8_t* info = inv::ItemInfo(s.itemId);
                    bool blueprint = info && ReadByte(info + kItemCategory, category) && category == kCategoryBlueprints;
                    ix.entries.push_back({n, blueprint ? std::string() : Lower(n), Kind::Item, s.itemId, 0, {}});
                    g_step = "item groups";
                    for (const auto& [gname, gkindAndRarity] : ItemGroups(s.itemId, n)) {
                        uint8_t gkind = gkindAndRarity & 0x0f;
                        std::string key = std::to_string(gkind) + ":" + Lower(gname);
                        auto g = byGroup.find(key);
                        if (g == byGroup.end()) {
                            g = byGroup.emplace(key, ix.entries.size()).first;
                            ix.entries.push_back({gname, Lower(gname), Kind::Group, 0, 0, {}});
                            ix.entries.back().group = gkind;
                            ix.entries.back().rarity = static_cast<uint8_t>(gkindAndRarity >> 4);
                        }
                        ix.entries[g->second].kinds++;
                        ix.entries[g->second].items.push_back(s.itemId);
                        itemGroups[s.itemId].push_back(g->second);
                    }
                } else {
                    e = it->second;
                }
                auto addTo = [&](size_t x) {
                    ix.entries[x].total += s.count;
                    if (std::find(ix.entries[x].chests.begin(), ix.entries[x].chests.end(), chest) == ix.entries[x].chests.end())
                        ix.entries[x].chests.push_back(chest);
                };
                addTo(e);
                for (size_t g : itemGroups[s.itemId])
                    addTo(g);
            }
        }
    }
    return ix;
}

// 0 exact, 1 the name starts with it, 2 a word in the name starts with it, 3 anywhere in the
// name; -1 no match.
int Rank(const std::string& name, const std::string& q)
{
    if (q.empty())
        return -1;
    if (name == q)
        return 0;
    size_t at = name.find(q);
    if (at == std::string::npos)
        return -1;
    if (at == 0)
        return 1;
    for (; at != std::string::npos; at = name.find(q, at + 1)) {
        char before = name[at - 1];
        if (!isalnum(static_cast<unsigned char>(before)))
            return 2;
    }
    return 3;
}

void UpdateSuggestions()
{
    std::string q = Lower(Trim(g_typed));
    std::vector<std::pair<int, int>> ranked; // rank, entry
    for (int i = 0; i < static_cast<int>(g_index.entries.size()); ++i) {
        int r = Rank(g_index.entries[i].lower, q);
        if (r >= 0)
            ranked.push_back({r, i});
    }
    const auto& es = g_index.entries;
    std::sort(ranked.begin(), ranked.end(), [&](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        if (a.first != b.first)
            return a.first < b.first;
        const Entry& x = es[a.second];
        const Entry& y = es[b.second];
        if (x.chests.size() != y.chests.size())
            return x.chests.size() > y.chests.size();
        if (x.total != y.total)
            return x.total > y.total;
        return x.lower < y.lower;
    });
    g_suggested.clear();
    for (size_t i = 0; i < ranked.size() && static_cast<int>(i) < kMaxSuggestions; ++i)
        g_suggested.push_back(ranked[i].second);
    g_selected = -1;
    g_top = 0;
}

// At most max characters (not bytes), with an ellipsis: a line that wrapped would push the lines
// below it out of the rows the mouse and the highlight bars use.
std::string Shorten(const std::string& s, size_t max)
{
    size_t chars = 0, i = 0;
    for (; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && ++chars > max)
            break;
    }
    if (i >= s.size())
        return s;
    return s.substr(0, i) + "\xE2\x80\xA6"; // ...
}

// "Granite Block  -  996 in 1 chest", "Perk: Backstab Damage  -  12 items in 3 chests",
// "Chest: Arrows" (in the game's language).
std::string Describe(const Entry& e)
{
    static const char kDash[] = "  \xE2\x80\x94  "; // em dash (UTF-8 bytes)
    int n = static_cast<int>(e.chests.size());
    constexpr size_t kMaxName = 34;
    std::string name = Shorten(e.name, kMaxName);
    if (e.kind == Kind::Chest)
        return std::string(strings::Get(strings::KindChest)) + ": " + name;
    if (e.kind == Kind::Group) {
        strings::Id label = e.group == GroupRarity ? strings::KindRarity
                            : e.group == GroupPerk ? strings::KindPerk
                            : e.group == GroupItemType ? strings::KindItemType
                                                       : strings::KindCategory;
        return std::string(strings::Get(label)) + ": " + name + kDash + strings::KindCount(e.kinds, n);
    }
    return name + kDash + strings::ItemCount(e.total, n);
}

std::string Format(strings::Id id, const std::string& arg)
{
    char buf[256];
    _snprintf_s(buf, _TRUNCATE, strings::Get(id), arg.c_str());
    return buf;
}

void ShowMessage()
{
    std::vector<std::string> lines;
    if (!g_notice.empty()) {
        lines.push_back(g_notice);
    } else if (!g_index.ok) {
        lines.push_back(strings::Get(g_index.inBase ? strings::NoChests : strings::NotInBase));
    } else if (Trim(g_typed).empty()) {
        lines.push_back(strings::Get(strings::TypePrompt));
    } else if (g_suggested.empty()) {
        lines.push_back(Format(strings::NoMatch, Trim(g_typed)));
    } else {
        int n = static_cast<int>(g_suggested.size());
        int end = (std::min)(n, g_top + kVisibleSuggestions);
        for (int i = g_top; i < end; ++i)
            lines.push_back(Describe(g_index.entries[g_suggested[i]]));
        // Selection and hover are bars drawn behind the lines (the text stays as it is).
        ui::SetDialogScroll(g_dialog, g_top, end - g_top, n);
        ui::SetDialogHighlights(g_dialog, g_selected - g_top, g_hover - g_top, end - g_top);
    }
    if (g_suggested.empty() || !g_notice.empty()) {
        ui::SetDialogScroll(g_dialog, 0, 0, 0);
        ui::SetDialogHighlights(g_dialog, -1, -1, 0);
    }
    ui::SetDialogLines(g_dialog, lines);
    // Small key hints under the buttons
    ui::SetDialogHint(g_dialog, strings::Get(g_index.ok ? strings::Hint : strings::HintClose));
}

void Select(int index)
{
    if (g_suggested.empty())
        return;
    int n = static_cast<int>(g_suggested.size());
    g_selected = ((index % n) + n) % n;
    if (g_selected < g_top) // scroll to keep it in view
        g_top = g_selected;
    else if (g_selected >= g_top + kVisibleSuggestions)
        g_top = g_selected - kVisibleSuggestions + 1;
    g_lastText = g_index.entries[g_suggested[g_selected]].name;
    ui::SetDialogText(g_dialog, g_lastText);
}

int Screen()
{
    auto* uc = static_cast<uint8_t*>(ui::Client());
    return uc ? uc[kUiScreen] : -1;
}

void OpenScreen(uint8_t screen)
{
    void* client = GameClient(); // the UI client would be the wrong one: the command is dropped
    if (!client || !g_dispatch)
        return;
    alignas(8) uint8_t cmd[kDispatchCmdSize] = {};
    cmd[0] = kCmdOpenScreen;
    cmd[1] = screen;
    g_dispatch(client, cmd);
}

// Closes the search and gives the keyboard back to the game.
// While the search is open the mouse must not turn the camera: the game's window object
// (GetWindowLongPtrW(hwnd, 0), whose +0x08 is the hwnd) scales raw mouse movement by the float
// at +0x68 before it turns the camera; it is 0 while the search is open (as in Auto Loot).
uint8_t* g_lockedWindow = nullptr;
float g_savedMouseScale = 1.0f;

HWND GameWindow()
{
    struct Best {
        DWORD pid;
        HWND hwnd;
        LONG area;
    } best{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(
        [](HWND w, LPARAM p) -> BOOL {
            auto* b = reinterpret_cast<Best*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            RECT r;
            if (pid == b->pid && IsWindowVisible(w) && GetClientRect(w, &r) && (r.right - r.left) * (r.bottom - r.top) > b->area) {
                b->area = (r.right - r.left) * (r.bottom - r.top);
                b->hwnd = w;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&best));
    return best.hwnd;
}

// The mouse wheel scrolls the list: the game window's procedure is replaced while the search
// is open and the wheel is taken from it (the game would zoom the camera).
std::atomic<int> g_wheel{0};
HWND g_wheelWindow = nullptr;
WNDPROC g_previousProc = nullptr;

LRESULT CALLBACK SearchWindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_MOUSEWHEEL) {
        g_wheel += GET_WHEEL_DELTA_WPARAM(wp);
        return 0;
    }
    return CallWindowProcW(g_previousProc, hwnd, message, wp, lp);
}

HWND GameWindow();

void CatchWheel(bool on)
{
    if (on == (g_wheelWindow != nullptr))
        return;
    if (on) {
        HWND hwnd = GameWindow();
        if (!hwnd)
            return;
        g_wheel = 0;
        g_previousProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
        SetLastError(0);
        if (!SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SearchWindowProc)) && GetLastError())
            return;
        g_wheelWindow = hwnd;
        return;
    }
    // Only put the old procedure back if nobody replaced ours in the meantime.
    if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_wheelWindow, GWLP_WNDPROC)) == &SearchWindowProc)
        SetWindowLongPtrW(g_wheelWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_previousProc));
    g_wheelWindow = nullptr;
}

void LockCamera(bool lock)
{
    if (lock == (g_lockedWindow != nullptr))
        return;
    if (!lock) {
        *reinterpret_cast<float*>(g_lockedWindow + kWindowMouseScale) = g_savedMouseScale;
        g_lockedWindow = nullptr;
        return;
    }
    HWND hwnd = GameWindow();
    auto* object = hwnd ? reinterpret_cast<uint8_t*>(GetWindowLongPtrW(hwnd, 0)) : nullptr;
    if (!object || *reinterpret_cast<HWND*>(object + 8) != hwnd) {
        Log("camera lock unavailable (window object not found)");
        return;
    }
    float& scale = *reinterpret_cast<float*>(object + kWindowMouseScale);
    if (!(scale > 0.0f && scale < 100.0f)) {
        Log("camera lock unavailable (mouse scale %g)", scale);
        return;
    }
    g_savedMouseScale = scale;
    scale = 0.0f;
    g_lockedWindow = object;
}

ULONGLONG g_escapeAt = 0;
bool g_escapeWasDown = false;

void CloseSearch(const char* why)
{
    ui::ReleaseDialogInput();
    g_open = false;
    LockCamera(false);
    CatchWheel(false);
    int screen = Screen();
    // Escape on the screen behind the search also opens the system menu: close it again.
    if (g_openedInventory && (screen == kScreenBehindSearch ||
                              (screen == kScreenSystemMenu && GetTickCount64() - g_escapeAt < 1000)))
        OpenScreen(kScreenPlaying);
    g_openedInventory = false;
    Log("search closed%s", why);
}

bool ReadIconRef(uint32_t itemId, std::array<uint8_t, 16>& out)
{
    const uint8_t* info = inv::ItemInfo(itemId);
    if (!info)
        return false;
    __try {
        memcpy(out.data(), info + kItemIcon, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SetFoundItems(const std::vector<uint32_t>& items)
{
    g_foundItems = items;
    std::vector<std::array<uint8_t, 16>> refs;
    for (uint32_t id : items) {
        std::array<uint8_t, 16> ref;
        if (ReadIconRef(id, ref) && std::find(refs.begin(), refs.end(), ref) == refs.end())
            refs.push_back(ref);
    }
    ui::SetIconTargets(refs);
    ui::SetSlotTargets(items);
}

void Unlight() { nativehighlight::Clear(); }

// The search is over: chests stop glowing and their items no longer light up.
void EndSearch()
{
    g_glows.clear();
    g_found.clear();
    g_glowsUntil = 0;
    Unlight();
}

void StartGlows(const std::vector<uint32_t>& chests)
{
    g_glows.clear();
    for (uint32_t c : chests) {
        auto it = g_index.positions.find(c);
        if (it == g_index.positions.end())
            continue;
        g_glows.push_back({{it->second[0], it->second[1], it->second[2]}, c, 0, false});
        if (g_glows.size() <= 8)
            DevLog("  glow %u at %.1f %.1f %.1f", c, it->second[0], it->second[1], it->second[2]);
    }
    g_found = g_glows;
    g_glowsStart = GetTickCount64();
    g_glowsUntil = g_glowsStart + g_glowLifetimeMs;
    g_lastGlowDraw = g_glowsStart;
    g_step = "highlight";
    nativehighlight::Clear();
}

// Returns true when chests were found (the dialog closes), false to keep it open.
bool Search(const std::string& text, bool picked = false)
{
    std::string q = Lower(Trim(text));
    if (q.empty())
        return false;
    if (!g_index.ok && q.rfind("@dev-", 0) != 0) {
        g_notice = strings::Get(g_index.inBase ? strings::NoChests : strings::NotInBase);
        Log("find '%s': %s", q.c_str(), g_index.why.c_str());
        return false;
    }
#ifdef FIND_DEV
    if (q.rfind("@dev-types ", 0) == 0) {
        // Component type names of entities: "@dev-types 1227 1177"
        void* reg = inv::InspectRegistry(0);
        for (const char* p = q.c_str() + 11; *p;) {
            char* end;
            unsigned long id = strtoul(p, &end, 10);
            if (end == p)
                break;
            p = end;
            uint16_t types[640];
            int n = reg ? inv::Types(reg, static_cast<uint32_t>(id), types, 640) : -1;
            std::string line;
            for (int i = 0; i < n; ++i) {
                char name[96];
                line += " " + std::to_string(types[i]) + "=" + (ecs::Name(reg, types[i], name, sizeof(name)) ? name : "?");
            }
            Log("dev types %lu:%s", id, line.c_str());
        }
        return false;
    }
    if (q.rfind("@dev-owner ", 0) == 0) {
        // Inventory pages of entities: "@dev-owner 1693 1228"
        for (const char* p = q.c_str() + 11; *p;) {
            char* end;
            unsigned long id = strtoul(p, &end, 10);
            if (end == p)
                break;
            p = end;
            inv::Owner o;
            if (!inv::ReadOwner(static_cast<uint32_t>(id), o)) {
                Log("dev owner %lu: unknown", id);
                continue;
            }
            std::string line;
            for (int i = 0; i < o.pageCount; ++i) {
                line += " [page " + std::to_string(o.pages[i].entity) + " kind " + std::to_string(static_cast<int>(o.pages[i].kind)) + ":";
                for (const auto& s : o.pages[i].slots)
                    if (s.itemId)
                        line += " " + ItemName(s.itemId) + "x" + std::to_string(s.count);
                line += "]";
            }
            Log("dev owner %lu: %d pages%s", id, o.pageCount, line.c_str());
        }
        return false;
    }
    if (q == "@dev-pages") {
        std::map<int, std::string> byPages;
        for (const auto& entry : g_index.positions) {
            inv::Owner o;
            int pages = inv::ReadOwner(entry.first, o) ? o.pageCount : -1;
            byPages[pages] += " " + std::to_string(entry.first);
        }
        for (const auto& [pages, ids] : byPages)
            Log("dev pages %d:%s", pages, ids.c_str());
        void* reg = inv::InspectRegistry(0);
        for (const auto& entry : g_index.positions) {
            uint16_t types[640];
            int n = reg ? inv::Types(reg, entry.first, types, 640) : -1;
            std::string line;
            for (int i = 0; i < n; ++i)
                line += " " + std::to_string(types[i]);
            Log("dev chest types %u:%s", entry.first, line.c_str());
        }
        return false;
    }
    if (q == "@dev-plain") {
        g_devPlain = !g_devPlain;
        Log("dev: small-altar mode %s", g_devPlain ? "on" : "off");
        return false;
    }
    if(q=="@dev-all-chests") {
        std::vector<uint32_t> all;for(const auto& entry:g_index.positions)all.push_back(entry.first);
        nativehighlight::SetStressTest(true);
        InterlockedExchange64(&g_devHoldUntil,GetTickCount64()+300000);
        SetFoundItems({});StartGlows(all);
        Log("performance stress: all %zu storage chests requested; detail limit 256",all.size());
        return true;
    }
#endif
    nativehighlight::SetStressTest(false);
    std::vector<uint32_t> chests;
    std::vector<uint32_t> items;
    std::string matched; // for the log
    auto add = [&](const Entry& e) {
        if (matched.size() < 600)
            matched += (matched.empty() ? "" : ", ") + e.name + (e.kind == Kind::Group ? " (kind)" : e.kind == Kind::Chest ? " (chest)" : "");
        for (uint32_t c : e.chests)
            if (std::find(chests.begin(), chests.end(), c) == chests.end())
                chests.push_back(c);
        if (e.kind == Kind::Item)
            items.push_back(e.itemId);
        for (uint32_t id : e.items)
            items.push_back(id);
    };
    // A suggestion picked with Tab: just that item, chest or kind. Typed text: every name
    // containing it ("stone" finds Stone and Rough Stone Block).
    bool exact = false;
    for (const Entry& e : g_index.entries) {
        if (picked && e.lower == q) {
            add(e);
            exact = true;
        }
    }
    // Names with a word starting with the text ("arrow": Iron Arrow, not Narrow Wall); only if
    // there are none, names containing it anywhere.
    if (!exact) {
        for (const Entry& e : g_index.entries) {
            int r = Rank(e.lower, q);
            if (r >= 0 && r <= 2)
                add(e);
        }
        if (chests.empty())
            for (const Entry& e : g_index.entries)
                if (e.lower.find(q) != std::string::npos)
                    add(e);
    }
    if (chests.empty()) {
        g_notice = Format(strings::NoMatch, Trim(text));
        Log("find '%s': nothing in %d chests", q.c_str(), g_index.chestCount);
#ifdef FIND_DEV
        for (const Entry& e : g_index.entries)
            DevLog("  index: %s [%d] %zu chests", e.name.c_str(), static_cast<int>(e.kind), e.chests.size());
#endif
        return false;
    }
    StartGlows(chests);
    SetFoundItems(items);
    DevLog("  matched: %s", matched.c_str());
#ifdef FIND_DEV
    {
        std::string ids;
        for (uint32_t c : chests)
            ids += " " + std::to_string(c);
        DevLog("  glowing:%s", ids.c_str());
    }
#endif
    Log("find '%s': %d of %d chests glow, %d items light up in them%s", q.c_str(), static_cast<int>(chests.size()),
        g_index.chestCount, static_cast<int>(items.size()), g_nativeReady ? "" : " (no chest outline: see above)");
    return true;
}

bool Edge(int slot, bool down)
{
    bool pressed = down && !g_navDown[slot];
    g_navDown[slot] = down;
    return pressed;
}

SRWLOCK g_requestLock = SRWLOCK_INIT;
char g_request[128]{};
bool g_requestPending = false;

void Tick()
{
    if (g_tracePending) {
        g_tracePending = false;
        ui::TraceDraws(false);
    }
    if (g_chestWindowAt && GetTickCount64() - g_chestWindowAt > 300) {
        ui::LightIcons(0, 0.0f);
        g_chestWindowAt = 0;
    }
    if (g_hotPaused) return;
    char request[128]{};
    bool pending;
    AcquireSRWLockExclusive(&g_requestLock);
    pending = g_requestPending && (!g_open || !strcmp(g_request, "@dev-recover-ui") || !strcmp(g_request, "@dev-input-state"));
    if (pending) { memcpy(request, g_request, sizeof(request)); g_requestPending = false; }
    ReleaseSRWLockExclusive(&g_requestLock);
    if (pending) {
        if (!strcmp(request, "@dev-input-state") || !strcmp(request, "@dev-recover-ui")) {
            Log("input diagnostic: screen=%d search=%d openedScreen=%d camera=%p wheel=%p escape=%d",
                Screen(), g_open, g_openedInventory, g_lockedWindow, g_wheelWindow,
                (GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0);
            if (!strcmp(request, "@dev-recover-ui")) {
                // Explicit bridge recovery only; never an automatic Escape override.
                if (Screen() == kScreenPlaying || Screen() == kScreenInventory || Screen() == 10) ui::RecoverDialogInput();
                CloseSearch(" (bridge recovery)");
                g_openPending=false;
                g_escapeWasDown=false;
                OpenScreen(kScreenPlaying);
            }
            return;
        }
        if (!request[0]) EndSearch();
        else { g_index = BuildIndex(); Search(request); }
        return;
    }
    auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool focused = GameFocused();
    // Modifiers must match exactly, so Shift+F does not also fire on Ctrl+Shift+F.
    bool down = focused && held(g_hotkey.key) && held(VK_SHIFT) == g_hotkey.shift &&
                held(VK_CONTROL) == g_hotkey.ctrl && held(VK_MENU) == g_hotkey.alt;
    bool pressed = down && !g_keyWasDown;
    g_keyWasDown = down;
    // Only while playing or in the inventory: not while typing in chat (43), renaming (47), a
    // signpost (19) or any other screen, where Shift+F is just a capital F.
    if (pressed && !g_open) {
        int screen = Screen();
        if (screen == kScreenPlaying || screen == kScreenInventory)
            g_openPending = true;
    }
    // Open when the key comes up, so its letter has already gone to the game, not the field.
    if (g_openPending && !held(g_hotkey.key)) {
        g_openPending = false;
        strings::Detect(); // the game's language (it can be changed in the game)
        if (static bool logged = false; !logged) {
            logged = true;
            Log("language %s: %s, %s, %s, %s", strings::Language(), CategoryLabel(7).c_str(), CategoryLabel(9).c_str(),
                RarityLabel(1).c_str(), RarityLabel(4).c_str());
        }
        if (!g_dialog)
            g_dialog = ui::NewDialog(strings::Get(strings::Title), strings::Get(strings::Title));
        ui::ClearDialog(g_dialog);
        g_index = BuildIndex();
        g_lastText.clear();
        g_typed.clear();
        g_notice.clear();
        g_suggested.clear();
        g_selected = -1;
        g_hover = -1;
        for (bool& b : g_navDown)
            b = true; // ignore keys already held
        g_open = true;
        g_openedAt = GetTickCount64();
        g_screenLogged = false;
        g_screenReached = false;
        // While playing the mouse turns the camera and nothing would give the keyboard back:
        // put a screen behind the search that gives the UI the mouse (the inventory: Rename closes itself without a chest).
        g_openedInventory = Screen() == kScreenPlaying;
        if (g_openedInventory) {
            OpenScreen(kScreenBehindSearch);
            LockCamera(true);
            CatchWheel(true);
        }
        ShowMessage();
        Log("search opened: %s", g_index.ok ? "index ready" : g_index.why.c_str());
        if (g_index.ok)
        {
            int groups = 0;
            for (const Entry& e : g_index.entries)
                groups += e.kind == Kind::Group;
            Log("  %d chests, %d names (%d kinds: categories, rarities, perks, item types; %d perk names known)",
                g_index.chestCount, static_cast<int>(g_index.entries.size()), groups, static_cast<int>(g_perkNames.size()));
        }
        return;
    }
    if (!g_open)
        return;

    if (g_openedInventory && Screen() == kScreenBehindSearch)
        g_screenReached = true;
    if (g_openedInventory && !g_screenLogged && GetTickCount64() - g_openedAt > 600) {
        g_screenLogged = true;
        DevLog("screen behind the search: %d", Screen());
    }
    // The screen the search opened was closed (Escape): close with it.
    // (Only once it was there: a screen that closes itself right away must not close the search.)
    bool escape = GameFocused() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    bool escapePressed = escape && !g_escapeWasDown;
    if (escapePressed) g_escapeAt = GetTickCount64();
    g_escapeWasDown = escape;
    g_step = "dialog";
    // Let the native field process cancel/deactivate before retiring the dialog.
    int r = ui::DrawDialog(g_dialog);
    if (escapePressed || r == 1) {
        CloseSearch(escapePressed ? " (Escape)" : " (cancelled)");
        return;
    }
    if (g_openedInventory && g_screenReached && Screen() != kScreenBehindSearch && GetTickCount64() - g_openedAt > 500) {
        CloseSearch(" (screen closed)");
        return;
    }
    size_t len;
    const char* raw = ui::DialogText(g_dialog, &len);
    std::string text(raw ? raw : "", raw ? len : 0);
    // Outside a base there is nothing to search: the field is switched off (no caret, nothing
    // typed) and Enter closes.
    if (!g_index.ok) {
        ui::DisableDialogField();
        if (r == 2 || Edge(4, focused && held(VK_RETURN))) {
            CloseSearch(" (not in a base)");
            return;
        }
        if (!text.empty())
            ui::SetDialogText(g_dialog, "");
        return;
    }
    // Enter finds even when the field lost the keyboard to a click on the list (the dialog
    // itself would only take the field back with that press).
    if (r != 2 && Edge(4, focused && held(VK_RETURN)) && !Trim(text).empty())
        r = 2;
    if (r == 2) {
        g_step = "search";
        bool picked = g_selected >= 0 && g_selected < static_cast<int>(g_suggested.size()) &&
                      g_index.entries[g_suggested[g_selected]].name == text;
        if (Search(text, picked)) {
            CloseSearch("");
            return;
        }
        ui::ClearDialog(g_dialog); // the confirm press is handled; stay open with the notice
        ui::SetDialogText(g_dialog, text);
        ShowMessage();
        return;
    }
    bool shift = held(VK_SHIFT);
    bool tab = Edge(0, focused && held(VK_TAB));
    bool up = Edge(1, focused && held(VK_UP));
    bool downKey = Edge(2, focused && held(VK_DOWN));
    ULONGLONG now = GetTickCount64();
    if (tab)
        g_tabAt = now;
    if (text != g_lastText && now - g_tabAt < kTabSpaceMs && text.size() == g_lastText.size() + 1 &&
        text.compare(0, g_lastText.size(), g_lastText) == 0 && isspace(static_cast<unsigned char>(text.back()))) {
        ui::SetDialogText(g_dialog, g_lastText); // the space Tab typed
        text = g_lastText;
    }
    if (text != g_lastText) { // typed
        g_lastText = text;
        g_typed = text;
        g_notice.clear();
        UpdateSuggestions();
        ShowMessage();
    }
    if ((tab || up || downKey) && !g_suggested.empty()) {
        int step = (up || (tab && shift)) ? -1 : 1;
        Select(g_selected < 0 ? (step > 0 ? 0 : -1) : g_selected + step);
        ShowMessage();
    }
    // The wheel scrolls the list; the selection stays on its top line.
    int n = static_cast<int>(g_suggested.size());
    if (int wheel = g_wheel.exchange(0); wheel && n > kVisibleSuggestions && g_notice.empty()) {
        int steps = -wheel / WHEEL_DELTA;
        if (steps == 0)
            steps = wheel > 0 ? -1 : 1;
        g_top = std::clamp(g_top + steps, 0, n - kVisibleSuggestions);
        Select(g_top);
        ShowMessage();
    }
    // The mouse: the line under it is marked; a click picks it, a second click on it finds.
    bool click = Edge(3, focused && held(VK_LBUTTON));
    float mx, my;
    int shown = (std::min)(n - g_top, kVisibleSuggestions);
    int under = -1;
    if (shown > 0 && g_notice.empty() && ui::CursorInUi(mx, my)) {
        int line = ui::DialogLineAt(g_dialog, shown, mx, my);
        under = line >= 0 ? g_top + line : -1;
    }
    if (under != g_hover) {
        g_hover = under;
        ShowMessage();
    }
    if (click && under >= 0) {
        int index = under;
        {
            if (index == g_selected && now - g_clickAt < kDoubleClickMs) {
                g_clickAt = 0;
                std::string name = g_index.entries[g_suggested[index]].name;
                if (Search(name, true)) {
                    CloseSearch(" (picked with the mouse)");
                    return;
                }
                ShowMessage();
            } else {
                Select(index);
                ShowMessage();
                g_clickAt = now;
            }
        }
    }
}

void DrawGlows()
{
    if (g_hotPaused) { nativehighlight::Clear(); return; }
    if (g_glows.empty())
        return;
    ULONGLONG now = GetTickCount64();
    // No HUD frame for a while (a loading screen, another world) ends the search; an open chest
    // window hides the HUD too, but the search goes on behind it.
    if (now >= g_glowsUntil || (now - g_lastGlowDraw > 2000 && now - g_chestWindowAt > 2000)) {
        EndSearch();
        return;
    }
    g_lastGlowDraw = now;
    float player[3];
    bool havePlayer = PlayerPosition(player);
    if (!havePlayer) { EndSearch(); return; }
    std::vector<nativehighlight::Target> targets;
    targets.reserve(g_glows.size());
    for (size_t i = 0; i < g_glows.size();) {
        Glow& g = g_glows[i];
        float alpha = 1.0f;
        if (havePlayer) { // left the base (or another world): this glow is done
            float dx = g.pos[0] - player[0], dy = g.pos[1] - player[1], dz = g.pos[2] - player[2];
            if (dx * dx + dy * dy + dz * dz > kLeftMeters * kLeftMeters) {
                g_glows.erase(g_glows.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }
        if (now >= static_cast<ULONGLONG>(InterlockedCompareExchange64(&g_devHoldUntil,0,0)) && havePlayer && !g.arrived) {
            float dx = g.pos[0] - player[0], dy = g.pos[1] + kChestCentre - player[1], dz = g.pos[2] - player[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) < g_arrivedMeters)
                g.arrived = (std::max)(now, g_glowsStart + kMinGlowMs);
        }
        if (now >= static_cast<ULONGLONG>(InterlockedCompareExchange64(&g_devHoldUntil,0,0)) && g.arrived && now >= g.arrived) {
            ULONGLONG since = now - g.arrived;
            if (since >= g_afterArrivalMs) {
                g_glows.erase(g_glows.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ULONGLONG fade = (std::min)(kFadeOutMs, g_afterArrivalMs);
            if (since > g_afterArrivalMs - fade)
                alpha = static_cast<float>(g_afterArrivalMs - since) / fade;
        }
        ULONGLONG left = g_glowsUntil - now;
        if (left < kFadeOutMs)
            alpha *= static_cast<float>(left) / kFadeOutMs;
        targets.push_back({g.chest, {g.pos[0], g.pos[1], g.pos[2]}, alpha});
        ++i;
    }
    nativehighlight::Publish(targets.data(), static_cast<unsigned>(targets.size()));
    nativehighlight::DrawSilhouettes();
}

// Logs where a fault happened (module + offset) before the handler runs.
int FaultFilter(EXCEPTION_POINTERS* e, const char* what)
{
    void* at = e->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(at), &mod);
    wchar_t path[MAX_PATH] = L"?";
    if (mod)
        GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* file = wcsrchr(path, L'\\');
    char name[64];
    WideCharToMultiByte(CP_UTF8, 0, file ? file + 1 : path, -1, name, sizeof(name), nullptr, nullptr);
    Log("%s: fault %08lx at %s+0x%llx (step: %s)", what, e->ExceptionRecord->ExceptionCode, name,
        static_cast<unsigned long long>(static_cast<uint8_t*>(at) - reinterpret_cast<uint8_t*>(mod)), g_step);
    return EXCEPTION_EXECUTE_HANDLER;
}

void SafeTick()
{
    __try {
        Tick();
    } __except (FaultFilter(GetExceptionInformation(), "search")) {
        g_open = false;
        g_openedInventory = false;
        __try {
            LockCamera(false);
            CatchWheel(false);
            if (Screen() == kScreenBehindSearch)
                OpenScreen(kScreenPlaying); // the empty screen would hide the whole HUD
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_lockedWindow = nullptr;
            g_wheelWindow = nullptr;
        }
        Log("search: fault, closed");
    }
}

void SafeDrawGlows()
{
    __try {
        DrawGlows();
    } __except (FaultFilter(GetExceptionInformation(), "glows")) {
        EndSearch();
        Log("glows: fault, cleared");
    }
}

void PlayStateHookBody(void* self);
void PlayStateHook(void* self)
{
    hot::Lease lease;
    if (!lease) return g_originalPlayState(self);
    PlayStateHookBody(self);
}
void PlayStateHookBody(void* self)
{
    // The native dialog participates in UI input routing. Let the game's normal
    // menu pass run afterward rather than leaving Find as the final UI consumer.
    // Live verification must cover subsequent menus as well as closing Find.
    SafeTick();
    g_originalPlayState(self);
}

void CommandLoopHookBody(void* app);
void CommandLoopHook(void* app)
{
    hot::Lease lease;
    if (!lease) return g_originalLoop(app);
    CommandLoopHookBody(app);
}
void CommandLoopHookBody(void* app)
{
    g_app = app;
    g_originalLoop(app);
}

// The chest window: opening a found chest ends its glow. Returns true when its found items
// should light up: the first time that chest opens in this search.
bool ChestWindowOpened()
{
    float player[3];
    if (g_found.empty() || !PlayerPosition(player))
        return false;
    Glow* best = nullptr;
    float bestD = kOpenedChestMeters * kOpenedChestMeters;
    for (Glow& g : g_found) {
        float dx = g.pos[0] - player[0], dy = g.pos[1] + kChestCentre - player[1], dz = g.pos[2] - player[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) {
            bestD = d;
            best = &g;
        }
    }
    if (!best)
        return false;
    for (Glow& g : g_glows) {
        if (g.chest == best->chest && !g.arrived) {
            g.arrived = GetTickCount64();
            Log("chest %u opened: its glow ends", g.chest);
        }
    }
    bool first = !best->shown;
    best->shown = true;
    return first;
}

#ifdef FIND_DEV
// Diagnostics: the items the slots showed, against the ones searched for.
void LogSlotItems()
{
    std::string ids;
    for (uint32_t id : ui::TakeSlotIds())
        ids += " " + std::to_string(id) + "=" + ItemName(id);
    DevLog("  slot items:%s", ids.c_str());
    std::string wanted;
    for (uint32_t id : g_foundItems)
        wanted += " " + std::to_string(id) + "=" + ItemName(id);
    DevLog("  wanted:%s", wanted.c_str());
}
#endif

void DualInventoryHookBody(char mode);
void DualInventoryHook(char mode)
{
    hot::Lease lease;
    if (!lease) return g_originalDual(mode);
    DualInventoryHookBody(mode);
}
void DualInventoryHookBody(char mode)
{
    ULONGLONG now = GetTickCount64();
    bool active = !g_foundItems.empty() && now < g_glowsUntil;
    if (now - g_chestWindowAt > kChestWindowGapMs) {
        g_traced = false;
        g_windowOpenedAt = now;
        DevLog("chest window opened (mode %d): %s, %d found items, %d of their icons known", mode,
            active ? "search active" : "no active search", static_cast<int>(g_foundItems.size()), ui::IconIdsLearned());
    }
    if (active) {
        __try {
            if (now - g_chestWindowAt > kChestWindowGapMs)
                g_lightThisWindow = ChestWindowOpened();
        } __except (FaultFilter(GetExceptionInformation(), "chest window")) {
            g_lightThisWindow = false;
        }
        float pulse = 0.55f + 0.35f * std::sin(static_cast<float>(now % kIconPulseMs) / kIconPulseMs * 6.2831853f);
        ULONGLONG shown = now - g_windowOpenedAt;
        if (!g_lightThisWindow || shown >= kIconShowMs)
            pulse = 0.0f;
        else if (shown > kIconShowMs - kFadeOutMs)
            pulse *= static_cast<float>(kIconShowMs - shown) / kFadeOutMs;
        ui::LightIcons(kIconColour, pulse);
    }
    g_chestWindowAt = now;
    g_originalDual(mode);
    // The grid's slots may be drawn after this call: lighting stays on while the window is
    // open and is switched off in Tick once it has not drawn for a moment.
    if (active) {
        int hits = ui::TakeIconHits();
        if (now - g_lastIconLog > 5000 && now - g_windowOpenedAt > 1000) {
            g_lastIconLog = now;
            DevLog("chest window: %d lit icons drawn this frame, %d icons known", hits, ui::IconIdsLearned());
#ifdef FIND_DEV
            ui::LogSlotDiagnostics();
            LogSlotItems();
#endif
        }
    }
}

// Diagnostic capture: one interval per existing spatial HUD callback. This is
// CPU-side frame cadence, not Present/display/GPU completion timing.
volatile LONG g_profileCommand=0;
LARGE_INTEGER g_profileLast{},g_profileFrequency{};
struct ProfileFrame { double interval,overlay; };
ProfileFrame g_profileFrames[20000];unsigned g_profileCount=0;bool g_profileActive=false;
void ProfileCommand() {
    LONG command=InterlockedExchange(&g_profileCommand,0);
    if(command==1){g_profileCount=0;g_profileLast.QuadPart=0;QueryPerformanceFrequency(&g_profileFrequency);g_profileActive=true;}
    if(command==2 && g_profileActive){
        g_profileActive=false;wchar_t path[MAX_PATH];GetModuleFileNameW(nullptr,path,MAX_PATH);
        wchar_t* slash=wcsrchr(path,L'\\');if(slash)wcscpy_s(slash+1,MAX_PATH-(slash+1-path),L"Find.profile.csv");
        FILE* f=nullptr;_wfopen_s(&f,path,L"w");if(f){fputs("interval_ms,overlay_ms\n",f);for(unsigned i=0;i<g_profileCount;++i)fprintf(f,"%.6f,%.6f\n",g_profileFrames[i].interval,g_profileFrames[i].overlay);fclose(f);}
        Log("performance profile: saved %u HUD frame intervals",g_profileCount);
    }
}
void SpatialMarkersHookBody(void* state, void* arg);
void SpatialMarkersHook(void* state, void* arg)
{
    hot::Lease lease;
    if (!lease) return g_originalSpatial(state, arg);
    SpatialMarkersHookBody(state, arg);
}
void SpatialMarkersHookBody(void* state, void* arg)
{
    ProfileCommand();
    LARGE_INTEGER begin{},overlayBegin{},end{};
    if(g_profileActive)QueryPerformanceCounter(&begin);
    g_originalSpatial(state, arg);
    if(g_profileActive)QueryPerformanceCounter(&overlayBegin);
    SafeDrawGlows();
    if(g_profileActive){
        QueryPerformanceCounter(&end);
        if(g_profileLast.QuadPart && g_profileCount<20000)g_profileFrames[g_profileCount++]={1000.0*(begin.QuadPart-g_profileLast.QuadPart)/g_profileFrequency.QuadPart,1000.0*(end.QuadPart-overlayBegin.QuadPart)/g_profileFrequency.QuadPart};
        g_profileLast=begin;
    }
}

} // namespace

bool HotPrepare() {
    if (g_open) return false;
    g_hotPaused = true;
    nativehighlight::Clear();
    ui::LightIcons(0, 0);
    return true;
}
bool HotReady() {
    nativehighlight::Status s{sizeof(s)};
    return nativehighlight::GetStatus(&s) && s.applied == 0;
}
void HotResume() { g_hotPaused = false; }

bool QueueSearch(const char* text)
{
    if (!text || strlen(text) >= sizeof(g_request)) return false;
#ifdef FIND_DEV
    if (!strcmp(text,"@dev-profile-start")) { InterlockedExchange(&g_profileCommand,1); return true; }
    if (!strcmp(text,"@dev-profile-stop")) { InterlockedExchange(&g_profileCommand,2); return true; }
    if (!strcmp(text,"@dev-hold-60s")) { InterlockedExchange64(&g_devHoldUntil,GetTickCount64()+60000); return true; }
    if (!strcmp(text,"@dev-hold-off")) { nativehighlight::SetStressTest(false); InterlockedExchange64(&g_devHoldUntil,0); return true; }
#endif
    AcquireSRWLockExclusive(&g_requestLock);
    strcpy_s(g_request, text);
    g_requestPending = true;
    ReleaseSRWLockExclusive(&g_requestLock);
    return true;
}

bool Init(const Config& config)
{
    g_hotkey = config.hotkey;
    g_glowLifetimeMs = static_cast<ULONGLONG>(config.glow.glowSeconds * 1000.0f);
    g_arrivedMeters = config.glow.nearMeters;
    g_afterArrivalMs = static_cast<ULONGLONG>(config.glow.nearSeconds * 1000.0f);
    g_nativeReady = nativehighlight::Init();
    uint8_t* playState = FindUnique("4C 8B DC 55 49 8D AB E8 FD FF FF 48 81 EC 10 03 00 00");
    if (!playState) {
        // Auto Loot (18-byte jump) or Creative Mode (safetyhook) may have hooked it already:
        // find the code after the first 18 bytes, which neither overwrites.
        uint8_t* after = FindUnique("48 8B 05 ?? ?? ?? ?? 48 8B 88 D0 00 00 00 48 8B 01 48 85 C0 0F 84 ?? ?? ?? ?? 4D 89 73 C8 4C 8B 70 38");
        if (after && IsHookPatch(after - 18))
            playState = after - 18;
    }
    uint8_t* spatial = FindUnique("48 89 54 24 10 55 57 41 56 48 8D AC 24 00 FE FF FF");
    uint8_t* loop = FindUnique("48 81 EC 98 00 00 00 48 8D 81 58 4F 03 00");
    if (!playState || !spatial || !loop) {
        Log("signatures: play state %p, markers %p, command loop %p", playState, spatial, loop);
        return false;
    }
    loop -= 13; // push rbx/rbp/rsi/rdi/r12/r13/r14/r15
    // UiCommandLoop: Quick Deposit's 12-byte jump + nop may be there already.
    g_originalLoop = reinterpret_cast<CommandLoopFn>(InstallHookChained(loop, reinterpret_cast<void*>(&CommandLoopHook), 13));
    // doPlayStateUi: mov r11, rsp; push rbp; lea rbp, [r11-218h]; sub rsp, 310h (18 bytes).
    g_originalPlayState = reinterpret_cast<PlayStateFn>(InstallHookChained(playState, reinterpret_cast<void*>(&PlayStateHook), 18));
    // markers: mov [rsp+10h], rdx; push rbp; push rdi; push r14; lea rbp, [rsp-200h] (17 bytes).
    g_originalSpatial = reinterpret_cast<SpatialMarkersFn>(InstallHookChained(spatial, reinterpret_cast<void*>(&SpatialMarkersHook), 17));
    if (!g_originalPlayState || !g_originalSpatial || !g_originalLoop) {
        Log("hooks: could not patch the game");
        return false;
    }
    // UiDispatch, called as it is (Quick Deposit may have hooked it; its jump leads on).
    g_dispatch = reinterpret_cast<UiDispatchFn>(FindUnique("4C 8B DC 55 56 57 49 8D AB 18 FF FF FF 48 81 EC D0 01 00 00"));
    if (!g_dispatch) {
        uint8_t* after = FindUnique("48 81 EC D0 01 00 00 0F B6 02 48 8B F2 FF C8 48 8B F9");
        if (after && IsHookPatch(after - 13))
            g_dispatch = reinterpret_cast<UiDispatchFn>(after - 13);
    }
    if (!g_dispatch)
        Log("signature not found: UiDispatch - the search cannot give back the keyboard; avoid Enter/Escape issues by clicking Cancel");
    // The chest window: mov [rsp+8], cl; push rbp/rbx/rsi/rdi/r12/r13/r15 (14 bytes).
    if (uint8_t* dual = FindUnique("88 4C 24 08 55 53 56 57 41 54 41 55 41 57"))
        g_originalDual = reinterpret_cast<DualInventoryFn>(InstallHookChained(dual, reinterpret_cast<void*>(&DualInventoryHook), 14));
    if (!g_originalDual)
        Log("signature not found: chest window - found items do not light up, opening a chest does not end its glow");
    return true;
}

} // namespace find

#ifdef FIND_DEV
// Dev bridge submits searches without opening a dialog, sending keys, or changing focus.
extern "C" __declspec(dllexport) bool FindQueueSearch(const char* text)
{
    return find::QueueSearch(text);
}

extern "C" __declspec(dllexport) bool FindNativeStatus(nativehighlight::Status* out)
{
    return nativehighlight::GetStatus(out);
}
#endif
