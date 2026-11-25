#include "hotapi.h"
#include "../../common/game_hints.h"
// Hotkey (Shift+F by default, find.ini): search the chests of the base you stand in for an
// item or a chest name, with suggestions while typing, and make every matching chest glow
// (build 1269579).
//
// 140c4e0c0 doPlayStateUi(): the in-game UI, once per frame; the search dialog is drawn after
//           the game's own UI.
// 140c32210 the HUD's waypoint marker, inside the spatial elements window; chest glows are
//           drawn right after it, in the same window.
// 140845600 UiCommandLoop(app): every frame; the game client = *(*(app + 0x352a0) + 0x1b8)
//           (a different object than the UI's client at *(**(G + 0xd0) + 0x38)). Quick
//           Deposit hooks it too: found by the code after its first 13 bytes, chained.
// Base and chests as in Quick Deposit: client + 0x47d4e0 world; world + 0x2c18 player
// position (int64, 32 fraction bits); world + 0x3ec8 base registry {+0x24 count, +0x30 bases
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
#include <intrin.h>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <set>
#include <sstream>
#include "projection_frame.h"
#include <string>
#include <vector>
#include "config.h"
#include "ecsnames.h"
#include "knowledge.h"
#include "recipes.h"
#include "find.h"
#include "inventory.h"
#include "log.h"
#include "reflect.h"
#include "match.h"
#include "nativehighlight.h"
#include "scan.h"
#include "silhouette.h"
#include "skills.h"
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

size_t kClientWorld = 0x47d4e0;
size_t kClientPlayer = 0x466b44;
size_t kWorldPlayerPos = 0x2c18;
size_t kWorldPermissions = 0x2c68;
size_t kWorldBases = 0x3ec8;
constexpr size_t kRegPermissions = 0x10;
constexpr size_t kRegBaseCount = 0x24;
constexpr size_t kRegBases = 0x30;
constexpr size_t kBaseStride = 0x130;
constexpr size_t kBaseMin = 0xF0;
constexpr size_t kBaseMax = 0x100;
size_t kItemName = 0x48;
constexpr float kFixedToMeters = 1.0f / 4294967296.0f;
constexpr int kMaxChests = 4096; // 256 stopped short in big bases: a depot server had more containers than that

constexpr int kVisibleSuggestions = 7; // the dialog shows up to 8 lines
constexpr int kMaxSuggestions = 60;     // the list scrolls through this many
constexpr int kMaxSkillSuggestions = 512; // the Skills page: all of them, to pick with a controller
size_t kItemRarity = 0x17;    // ItemInfo: ItemRarity (1 uncommon .. 4 legendary)
size_t kItemCaption = 0x58;   // ItemInfo: CaptionLocaTag, the type line ("One-Handed Axe")
size_t kItemCategory = 0xcc;  // ItemInfo: ItemCategory
constexpr uint8_t kCategoryBlueprints = 10;
size_t kItemPerkIds = 0x658;  // ItemInfo: PerkId[5], the perks its upgrades unlock
size_t kItemUiValues = 0x5e8;     // ItemInfo: BlobArray<ImpactUiValue> uiValues (0x28 each, +0 LocaTagId)
size_t kItemUiBuffValues = 0x5f0; // ItemInfo: ... uiBuffValues (what a food or potion gives: "Maximum Oxygen")
size_t kItemLevelRange = 0x674; // ItemInfo: itemLevelRange {minLevel, maxLevel} (uint32)
size_t kUiClientPerks = 0x466800; // UI client: the perk registry (as the item tooltip uses it)
// A glow lasts until the next search, at most glow_seconds (find.ini), or until you come within
// near_meters (3D) and then near_seconds more.
constexpr float kLeftMeters = 120.0f;                // farther than this: you left the base, the glow ends
constexpr ULONGLONG kMinGlowMs = 2000;               // chests right next to you still show for a moment
constexpr ULONGLONG kFadeOutMs = 600;
constexpr float kChestCentre = 0.5f; // chest origin is at its bottom; chests stack 1 m apart
size_t kItemPlacedEntity = 0x104 + 0x2bc; // ItemInfo: equipment.placedEntity, a TemplateReference (GUID first)
size_t kItemIcon = 0x56c;   // ItemInfo: iconImage, a 16-byte UiImageReference
constexpr uint8_t kCmdOpenScreen = 0x01;
size_t kUiScreen = 0x1cd8;  // UI client: the HUD screen shown
constexpr uint8_t kScreenPlaying = 1;
constexpr uint8_t kScreenInventory = 9;
// HudScreenType Collection: with nothing to collect it draws nothing and gives the UI the mouse,
// so the search sits over the world (as Auto Loot's panel does). Rename (47) closes itself,
// InfoPopup (49) shows the news.
constexpr uint8_t kScreenBehindSearch = 24;
constexpr uint8_t kScreenSystemMenu = 2;
constexpr uint8_t kScreenCrafting = 25; // a station's or an NPC's crafting window (the Kitchen)
constexpr size_t kWindowMouseScale = 0x68; // game window object: raw mouse -> camera factor
constexpr size_t kDispatchCmdSize = 0x410;
// Field offsets from the game's own type reflection and code (reflect.h); the literals are the build of 2026-11-04.
// The code signatures come from tools/patchcheck/dispsig.py, checked to read the right value on the builds of
// 2026-10-21 (World fields 0x10 lower) and 2026-11-02 too.
size_t kUiValueSize = 0x28, kUiValueLoca = 0, kUiValueValue = 8; // ImpactUiValue: its size, locaId, value

void ReadItemLayout()
{
    kClientWorld = reflect::FromCode("ClientWorld", "8B 8F ?? ?? ?? ?? 33 DB 0F 10 72 04", 2, "83 B9 ?? ?? ?? ?? 00 8B FA 48 8B D9", 2, 0x47d4e0);
    kClientPlayer = reflect::FromCode("ClientPlayer", "39 BF ?? ?? ?? ?? 75 ?? 4C 39 7C 24 58", 2, "8D 8F ?? ?? ?? ?? E8 ?? ?? ?? ?? 89 43 10", 2, 0x466b44);
    kUiClientPerks = reflect::FromCode("UiClientPerks", "8B 92 ?? ?? ?? ?? 41 8B 4D 00 E8 ?? ?? ?? ??", 2, "8B 92 ?? ?? ?? ?? 41 8B 0F E8 ?? ?? ?? ?? 48 85 C0", 2, 0x466800);
    kUiScreen = reflect::FromCode("UiScreen", "80 BB ?? ?? ?? ?? 30 74 ?? 45 32 F6", 2, "80 BF ?? ?? ?? ?? 01 75 ?? 4D 85 C0", 2, 0x1cd8);
    kWorldPlayerPos = reflect::FromCode("WorldPlayerPos", "11 8F ?? ?? ?? ?? 4D 89 AF ?? ?? ?? ??", 2, "89 BB ?? ?? ?? ?? 48 89 BB ?? ?? ?? ?? 48 89 BB ?? ?? ?? ?? C7 83 ?? ?? ?? ?? 00 00 80 3F", 2, 0x2c18);
    kWorldPermissions = reflect::FromCode("WorldPermissions", "89 84 33 ?? ?? ?? ?? 49 8B 86 ?? ?? ?? ??", 3, "8D 97 ?? ?? ?? ?? 49 8B 0C 24 E8 ?? ?? ?? ??", 2, 0x2c68);
    kWorldBases = reflect::FromCode("WorldBases", "89 87 ?? ?? ?? ?? 49 8B 04 24 48 89 44 24 60", 2, "8B 8E ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4E 08", 2, 0x3ec8);
    ui::ClientViewOffset(); // resolved here, not on first use in a render job
    kUiValueSize = reflect::Size("keen::ImpactUiValue", 0x28);
    kUiValueLoca = reflect::Offset("keen::ImpactUiValue", "locaId", 0);
    kUiValueValue = reflect::Offset("keen::ImpactUiValue", "value", 8);
    kItemName = reflect::Offset("keen::ItemInfo", "name", 0x48);
    kItemRarity = reflect::Offset("keen::ItemInfo", "rarity", 0x17);
    kItemCaption = reflect::Offset("keen::ItemInfo", "caption", 0x58);
    kItemCategory = reflect::Offset("keen::ItemInfo", "category", 0xcc);
    kItemPerkIds = reflect::Offset("keen::ItemInfo", "perkIds", 0x658);
    kItemUiValues = reflect::Offset("keen::ItemInfo", "uiValues", 0x5e8);
    kItemUiBuffValues = reflect::Offset("keen::ItemInfo", "uiBuffValues", 0x5f0);
    kItemLevelRange = reflect::Offset("keen::ItemInfo", "itemLevelRange", 0x674);
    kItemPlacedEntity = reflect::Offset("keen::ItemInfo", "equipment.placedEntity", 0x104 + 0x2bc);
    kItemIcon = reflect::Offset("keen::ItemInfo", "iconImage", 0x56c);
}

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
    void* ui = At<void*>(app, 0x352a0);
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
// What other mods can read (the map mod draws it): the chests and stations lit right now, copied whenever they change.
SRWLOCK g_shareLock = SRWLOCK_INIT;
std::vector<Glow> g_shared;
uint32_t g_shareGeneration = 0; // a new search: a new number
void ShareFound(bool newSearch)
{
    AcquireSRWLockExclusive(&g_shareLock);
    g_shared = g_glows;
    if (newSearch)
        ++g_shareGeneration;
    ReleaseSRWLockExclusive(&g_shareLock);
}
ULONGLONG g_glowsStart = 0;
std::vector<uint32_t> g_crafterGlows; // the NPCs and stations among g_glows (the frame rate test leaves them or the chests out)
bool g_depositGlow = false;           // find.ini debug_deposit_glow = 1
bool g_fpsTest = false;               // find.ini debug_fps_test = 1
volatile LONG64 g_tickTicks, g_hudTicks; // time in Find's per-frame work and in its HUD drawing
volatile LONG g_hudCalls;
ULONGLONG g_lastGlowDraw = 0;

bool g_nativeReady = false;

// --- the search ---

enum class Kind : uint8_t {
    Item,  // one item
    Chest, // a chest's own name
    Group, // every item of a category, item type or rarity ("Consumables", "Legendary")
    Skill, // a skill of the skill tree (Skills page): every node of that name
};

struct Entry {
    std::string name; // as the game shows it
    std::string lower;
    Kind kind;
    uint32_t itemId;
    uint32_t total;   // items in the base
    std::vector<uint32_t> chests;
    std::vector<uint32_t> crafters; // Item: NPCs and stations of the base that make it
    bool locked = false;            // Item: none of them can make it yet (recipe not unlocked)
    std::vector<uint32_t> placed;   // Item: where it stands in the base (a station, bed, torch...)
    uint32_t kinds = 0; // Group: how many different items
    uint32_t made = 0, madeLocked = 0; // Group (effects): items only the base's crafters have, and of those not unlocked
    std::vector<uint32_t> items; // Group: those items
    uint8_t group = 0;           // Group: what kind (GroupCategory...)
    uint8_t rarity = 0;          // Group of a rarity: which (1 uncommon .. 4 legendary)
    std::string desc, descLower; // Skill: its description (game text, markup removed)
    std::vector<uint32_t> nodes; // Skill: its node ids
    bool category = false;       // Skill: a class label of the tree (Warrior, Guardian...), all its nodes
    float centre[2] = {};        // Skill category: the middle of its nodes
};

struct Index {
    bool ok = false;
    bool inBase = false; // !ok only because the base has no storage chests
    bool skills = false; // the Skills page: the entries are its skills
    std::string why;
    std::vector<Entry> entries;
    std::map<uint32_t, std::vector<float>> positions; // chest -> x, y, z
    int chestCount = 0;
};

Bindings g_keys;
greeneye::input::SharedBinding g_openPad;
std::atomic<uint16_t> g_padHiddenNow{0};     // the search's own buttons while it is open
std::atomic<ULONGLONG> g_padLiveUntil{0};    // the chord may open the search (play or inventory)
greeneye::input::PadPresses g_padNav;
bool g_browse = false;                       // a controller with nothing typed: list everything
bool g_tipsShown = false;                    // this search has its tips
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
bool g_navDown[6] = {};       // next, up, down, left mouse button, confirm, previous edges
#ifdef FIND_DEV
// "@dev-icons": the textures that could mark a chest on the compass, numbered, side by side for 30 s.
ULONGLONG g_iconPreviewUntil = 0;
struct PreviewIcon { size_t asset; const char* name; };
const PreviewIcon kPreviewIcons[] = {
    {0xbfb8, "accessStorage"}, {0xe0f0, "compass.priorityMarker"}, {0xe0d0, "compass.mark"},
    {0xbae8, "hudMarkerFill"}, {0xbaf8, "hudMarkerOutline"}, {0xba78, "pin"}, {0xbbb8, "star4"}, {0xbbc8, "star5"},
    {0xbf38, "location"}, {0xd410, "diamond"}, {0xeec0, "map.waypoint"}, {0xef40, "map.playerPing"},
    {0xef50, "map.playerMarkerPing"}, {0xef10, "map.investigation"}, {0xf030, "customMarkerWaypoint"},
    {0xba88, "menuAttentionMarker"}, {0xbe78, "questImportant"}, {0xf078, "pinnedRecipeIcon"},
};
void DrawIconPreview()
{
    if (GetTickCount64() >= g_iconPreviewUntil)
        return;
    constexpr int kPerRow = 9;
    for (int i = 0; i < static_cast<int>(sizeof(kPreviewIcons) / sizeof(kPreviewIcons[0])); ++i) {
        float x = 360.0f + (i % kPerRow) * 150.0f, y = 260.0f + (i / kPerRow) * 150.0f;
        ui::DrawAssetIcon(kPreviewIcons[i].asset, x, y, 40.0f, kIconColour, 1.0f);
        ui::DrawAssetIcon(kPreviewIcons[i].asset, x, y + 50.0f, 26.0f, 0xFFFFFFFF, 1.0f);
        char label[8];
        sprintf_s(label, "%d", i + 1);
        ui::DrawSmallLabel(x - 8.0f, y + 70.0f, label);
    }
}
#endif

int g_compassSign = 1; // which way the compass strip runs (dev: "@dev-compass" flips it)
bool g_resumeField = false;   // Enter found nothing: give the field the keyboard back once Enter is up
ULONGLONG g_clickAt = 0;       // the last click on a suggestion
int g_hover = -1;              // the suggestion under the mouse
constexpr ULONGLONG kDoubleClickMs = 500;
ULONGLONG g_tabAt = 0;        // the field turns Tab into a space; that space is dropped
bool g_exact = false;         // Exact match (find.ini exact_match): no matching by words
bool g_exactKeyDown = false;
int g_hiddenByExact = 0;      // results Exact match leaves out of the list
ULONGLONG g_exactAt = 0;      // Ctrl+E may type an e into the field; dropped like Tab's space
bool g_openPending = false;   // the hotkey was pressed; open once it is released
constexpr ULONGLONG kTabSpaceMs = 400;

int Screen();
#ifdef FIND_DEV
int g_devNoTip = 0;                        // @dev-tip: 1 no tip
#endif

bool GameFocused()
{
    return greeneye::input::GameHasFocus();
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

// Crafting stations hold what they made (a seedbed's seedlings, a kiln's charcoal): they are
// searched like chests.
void AddStations(uint8_t* base, std::vector<uint32_t>& out)
{
    std::vector<uint32_t> stations(kMaxChests);
    int ns = inv::FindStations(&At<float>(base, kBaseMin), &At<float>(base, kBaseMax), out.data(),
                               static_cast<int>(out.size()), stations.data(), kMaxChests);
    for (int i = 0; i < ns && out.size() < static_cast<size_t>(kMaxChests); ++i)
        out.push_back(stations[i]);
}

float g_baseLo[3], g_baseHi[3]; // the bounds of the base BaseChests found

double DevMs();
double g_devReadMs, g_devNameMs, g_devPosMs;
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
    memcpy(g_baseLo, &At<float>(base, kBaseMin), sizeof(g_baseLo));
    memcpy(g_baseHi, &At<float>(base, kBaseMax), sizeof(g_baseHi));
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
        inv::DetectPosition(playerId, pos); // once the registry is known; before anything goes by position
        // The altar lists planted bushes too. Left in, they glowed as chests, and as "magic
        // chests" they let crafting stations past KeepStorageChests.
        magic.resize(inv::DropPlants(client, magic.data(), static_cast<int>(magic.size())));
        out = magic;
    }
    if (magic.empty()) {
        // A small flame altar lists no crafting-storage chests: find the plain chests themselves.
        std::vector<uint32_t> plain(kMaxChests);
        bool ready = inv::Ready(playerId, nullptr, 0);
        if (ready)
            inv::DetectPosition(playerId, pos);
        int np = !ready ? 0 : inv::FindPlainChests(&At<float>(base, kBaseMin), &At<float>(base, kBaseMax), plain.data(), kMaxChests);
        np = inv::DropPlants(client, plain.data(), np);
        out.assign(plain.begin(), plain.begin() + np);
        if (out.empty()) {
            why = "no storage chests in this base";
            return false;
        }
        AddStations(base, out);
        return true;
    }
    double t0 = DevMs();
    std::vector<uint32_t> found(kMaxChests);
    int nf = inv::FindContainers(&At<float>(base, kBaseMin), &At<float>(base, kBaseMax), found.data(), kMaxChests);
    double t1 = DevMs();
    nf = inv::KeepStorageChests(magic.data(), static_cast<int>(magic.size()), found.data(), nf);
    double t2 = DevMs();
    for (int i = 0; i < nf; ++i)
        if (std::find(out.begin(), out.end(), found[i]) == out.end())
            out.push_back(found[i]);
    double t3 = DevMs();
    AddStations(base, out);
    DevLog("  chest steps: walk %.0f ms, keep %.0f ms, merge %.0f ms, stations %.0f ms", t1 - t0, t2 - t1, t3 - t2, DevMs() - t3);
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

// A blueprint's own name is what it builds ("Single Wall Block 0,5m"), but the game shows it as
// the tool that holds it, usually the Construction Hammer: it is listed as "Construction Hammer
// (Single Wall Block 0,5m)" and found by the tool's name only, so "wall" doesn't light every chest
// of blueprints. The tools themselves (Tool_*: hammers, Decay and Overgrowth Tools) are Blueprints
// too and keep their own names.
constexpr uint32_t kConstructionHammerItem = 693164685; // Tool_Quickbuilder_Hammer
constexpr uint32_t kDecayToolItem = 3286071567;         // Tool_Building_Decay: its Building_Decay_Shape_*
constexpr uint32_t kOvergrowthToolItem = 3196782416;    // Tool_Building_Overgrowth: Building_Overgrowth_Shape_*

bool ReadByte(const uint8_t* p, uint8_t& out);

uint32_t BlueprintToolOf(uint32_t itemId);

// The item a blueprint or an NPC's summoning staff is listed as, or 0 for anything else. Remembered per item: a
// big base asks for every stack (an item's kind doesn't change during a game session).
uint32_t BlueprintTool(uint32_t itemId)
{
    static std::unordered_map<uint32_t, uint32_t> known;
    auto it = known.find(itemId);
    if (it != known.end())
        return it->second;
    uint32_t tool = BlueprintToolOf(itemId);
    if (inv::ItemInfo(itemId)) // not before the item database is there
        known.emplace(itemId, tool);
    return tool;
}

uint32_t BlueprintToolOf(uint32_t itemId)
{
    // An NPC's summoning staff ("NPC_..."): one Summoning Staff entry for all of them.
    if (_strnicmp(inv::Describe(itemId).raw, "NPC_", 4) == 0)
        return kSummoningStaffItem;
    uint8_t category = 0;
    const uint8_t* info = inv::ItemInfo(itemId);
    if (!info || !ReadByte(info + kItemCategory, category) || category != kCategoryBlueprints)
        return 0;
    const char* raw = inv::Describe(itemId).raw;
    if (_strnicmp(raw, "Tool_", 5) == 0)
        return 0;
    if (_strnicmp(raw, "Building_Decay_", 15) == 0)
        return kDecayToolItem;
    if (_strnicmp(raw, "Building_Overgrowth_", 20) == 0)
        return kOvergrowthToolItem;
    return kConstructionHammerItem;
}

std::string ToolName(uint32_t tool)
{
    std::string name = inv::ItemInfo(tool) ? ItemName(tool) : std::string();
    if (!name.empty() && name.rfind("item ", 0) != 0)
        return name;
    return tool == kDecayToolItem        ? "Decay Tool"
           : tool == kOvergrowthToolItem ? "Overgrowth Tool"
           : tool == kSummoningStaffItem ? "Summoning Staff"
                                         : "Construction Hammer";
}

// What a search matches an item by (lower case).
std::string MatchText(uint32_t itemId, const std::string& name)
{
    uint32_t tool = BlueprintTool(itemId);
    return Lower(tool ? ToolName(tool) : name);
}

std::string SearchName(uint32_t itemId)
{
    std::string name = ItemName(itemId);
    if (uint32_t tool = BlueprintTool(itemId))
        return ToolName(tool) + " (" + name + ")";
    if (_strnicmp(inv::Describe(itemId).raw, "NPC_", 4) != 0)
        return name;
    std::string staff = inv::ItemInfo(kSummoningStaffItem) ? ItemName(kSummoningStaffItem) : std::string();
    if (staff.empty() || staff.rfind("item ", 0) == 0)
        staff = "Summoning Staff";
    return staff + " (" + name + ")";
}

// Item categories as the search offers them (keen::ItemCategory order); empty: not offered.
enum Group : uint8_t { GroupNone, GroupCategory, GroupRarity, GroupPerk, GroupItemType, GroupLevel, GroupEffect };

// Category and rarity names as the game shows them, in its language. The UI texts
// (*(G + 200)): + 0x10 FbUiLocaLinks; its crafting texts (+ 0xb1b0) hold the item categories at
// + 0xe0 (FbUiLocaCraftingCategories: equipment + 0x20, weapons + 0x30, tools + 0x40,
// buildTools + 0x50, consumables + 0xa0, ammunition + 0xb0, materials + 0xd0, blueprints + 0xe0,
// currency + 0x110, collectible + 0x120, petFood + 0x130); its RPG terms (+ 0x40) hold the
// rarities at + 0x1d90 (common, uncommon, rare, epic, legendary; 0x10 apart).
constexpr size_t kLocaCategories = 0x10 + 0xb1b0 + 0xe0;
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

// What a food or potion gives, as its tooltip lists it ("Maximum Oxygen", "Health Regeneration"):
// ItemInfo uiValues, BlobArray<ImpactUiValue> {int32 offset from the field, uint32 count}, 0x28
// each: +0 LocaTagId, +4 format, +8 value. Only consumables: other items list mechanics there
// (Attack Speed, Radius, Item Level). Left out: losses (raw meat's Health -50) and how long it
// lasts (Duration, Minutes Duration: every label holding the game's own word for it).
constexpr uint8_t kCategoryConsumables = 7;
constexpr uint32_t kLocaDuration = 0x86a75e07; // the uiValues label "Duration"
std::string g_durationWord; // lower case, set per index build

std::vector<std::string> ItemEffects(const uint8_t* info, uint8_t category)
{
    std::vector<std::string> effects;
    if (category != kCategoryConsumables)
        return effects;
    int32_t blob[2] = {};
    hot::Read(info + kItemUiValues, blob, sizeof(blob));
    if (!blob[0] || blob[1] <= 0 || blob[1] > 32)
        return effects;
    for (int32_t k = 0; k < blob[1]; ++k) {
        const uint8_t* entry = info + kItemUiValues + blob[0] + kUiValueSize * k;
        uint32_t loca = 0;
        int32_t value = 0;
        hot::Read(entry + kUiValueLoca, &loca, sizeof(loca));
        hot::Read(entry + kUiValueValue, &value, sizeof(value));
        if (!loca || value <= 0)
            continue;
        std::string label = Trim(ui::LocaId(loca));
        if (label.empty() || label.size() >= 48 || (!g_durationWord.empty() && Lower(label).find(g_durationWord) != std::string::npos))
            continue;
        if (std::find(effects.begin(), effects.end(), label) == effects.end())
            effects.push_back(label);
    }
    return effects;
}

// The groups an item belongs to: its category, rarity, perks, type line and effects.
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
    for (const std::string& effect : ItemEffects(info, category))
        if (std::find(groups.begin(), groups.end(), std::make_pair(effect, uint8_t{GroupEffect})) == groups.end())
            groups.push_back({effect, GroupEffect});
    return groups;
}

// The NPCs and crafting stations of the base light up for the items their recipes make, also
// items no chest holds ("can be made here").
void AddCrafters(Index& ix, std::map<uint32_t, size_t>& byItem)
{
    if (!recipes::Loaded()) {
        // The recipes are in the same game data as the item records.
        const uint8_t* info = byItem.empty() ? nullptr : inv::ItemInfo(byItem.begin()->first);
        if (!recipes::Load(info))
            return;
    }
    double c0 = DevMs();
    bool knows = knowledge::Refresh(recipes::IsRecipe);
    double c1 = DevMs();
    std::map<size_t, bool> unlocked; // entries with crafters: whether one of them can make it
    std::map<size_t, std::vector<uint32_t>> recipeIds; // ...and the recipes that would
    std::vector<inv::Crafter> crafters(2048);
    int n = inv::FindCrafters(g_baseLo, g_baseHi, recipes::IsWorkshop, crafters.data(), static_cast<int>(crafters.size()));
    double c2 = DevMs();
    size_t pairs = 0;
    for (int c = 0; c < n; ++c) {
        float p[3];
        if (!inv::Position(crafters[c].entity, p))
            continue;
        ix.positions[crafters[c].entity] = {p[0], p[1], p[2]};
        const std::vector<uint32_t>* makes = recipes::Makes(crafters[c].workshop);
        pairs += makes ? makes->size() : 0;
        for (size_t i = 0; makes && i < makes->size(); ++i) {
            uint32_t tool = BlueprintTool((*makes)[i]);
            uint32_t item = tool ? tool : (*makes)[i];
            auto it = byItem.find(item);
            if (it == byItem.end()) {
                if (!inv::ItemInfo(item))
                    continue;
                std::string name = SearchName(item);
                it = byItem.emplace(item, ix.entries.size()).first;
                ix.entries.push_back({name, MatchText(item, name), Kind::Item, item, 0, {}});
            }
            auto& list = ix.entries[it->second].crafters;
            if (std::find(list.begin(), list.end(), crafters[c].entity) == list.end())
                list.push_back(crafters[c].entity);
            bool& can = unlocked[it->second];
            const std::vector<uint32_t>* ids = recipes::RecipeIds(crafters[c].workshop, (*makes)[i]);
            for (size_t r = 0; ids && r < ids->size(); ++r) {
                can = can || knowledge::Known((*ids)[r]);
                recipeIds[it->second].push_back((*ids)[r]);
            }
        }
    }
    double c3 = DevMs();
    // Without the player's knowledge nothing is called locked. Before something is, the knowledge is read again
    // in full if it may be stale (a recipe learned since the arrays were found).
    for (auto& [entry, can] : unlocked) {
        if (knows && !can)
            for (uint32_t id : recipeIds[entry])
                if ((can = knowledge::KnownAfterRescan(id, recipes::IsRecipe)))
                    break;
        ix.entries[entry].locked = knows && !can;
    }
    DevLog("  crafter steps: knowledge %.0f ms, find %.0f ms (%d crafters), items %.0f ms (%zu pairs), locked %.0f ms",
           c1 - c0, c2 - c1, n, c3 - c2, pairs, DevMs() - c3);
}

// What stands in the base lights up for its own name: searching "oven" finds the ovens placed
// there, not only the Farmer who makes them. Each placed entity is matched to the item that
// places its template.
constexpr int kMaxPlaced = 4096;
void AddPlaced(Index& ix, std::map<uint32_t, size_t>& byItem)
{
    if (!recipes::Loaded())
        return;
    static std::map<std::string, uint32_t> byTemplate; // template GUID -> item; the same all session
    if (byTemplate.empty()) {
        for (uint32_t item : recipes::AllOutputs()) {
            const uint8_t* info = inv::ItemInfo(item);
            if (!info)
                continue;
            uint8_t guid[16];
            hot::Read(info + kItemPlacedEntity, guid, sizeof(guid));
            static const uint8_t kNone[16] = {};
            if (memcmp(guid, kNone, sizeof(guid)) != 0)
                byTemplate.emplace(std::string(reinterpret_cast<char*>(guid), 16), item);
        }
        Log("placed: %zu items place something", byTemplate.size());
    }
    std::vector<inv::Placed> placed(kMaxPlaced);
    int n = inv::FindPlaced(g_baseLo, g_baseHi, placed.data(), kMaxPlaced);
    int matched = 0;
    for (int i = 0; i < n; ++i) {
        auto t = byTemplate.find(std::string(reinterpret_cast<char*>(placed[i].guid), 16));
        if (t == byTemplate.end())
            continue;
        uint32_t item = t->second;
        auto it = byItem.find(item);
        if (it == byItem.end()) {
            std::string name = SearchName(item);
            it = byItem.emplace(item, ix.entries.size()).first;
            ix.entries.push_back({name, MatchText(item, name), Kind::Item, item, 0, {}});
        }
        float p[3];
        if (!inv::Position(placed[i].entity, p))
            continue;
        ix.positions[placed[i].entity] = {p[0], p[1], p[2]};
        ix.entries[it->second].placed.push_back(placed[i].entity);
        ++matched;
    }
    Log("placed: %d of %d things in the base are known items", matched, n);
}

// Every item and named chest of the base, built when the search opens.
// The level an item reaches once fully upgraded. Each upgrade adds a level and unlocks the next of its perks, as many
// upgrades as its rarity allows (common 1 .. legendary 5) and it has perks for; items without perks don't upgrade. The
// first level is the kind's fixed level when it has one (Crestflame Shield: 45, 50 when upgraded five times), else the
// level it has now. Measured on a depot server (2026-11-17): fully upgraded items sat at exactly that level.
int MaxLevel(uint32_t itemId, uint8_t level, uint8_t rarity)
{
    const uint8_t* info = inv::ItemInfo(itemId);
    uint32_t perks[5] = {}, range[2] = {};
    if (!info || !ReadPerkIds(info, perks))
        return level;
    hot::Read(info + kItemLevelRange, range, sizeof(range));
    int count = 0;
    for (uint32_t p : perks)
        count += p != 0;
    if (!count)
        return level;
    int upgrades = (std::min)(count, rarity + 1);
    int base = range[0] && range[0] == range[1] && range[0] <= level ? static_cast<int>(range[0]) : level;
    return (std::max)(static_cast<int>(level), base + upgrades);
}

double DevMs()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * c.QuadPart / f.QuadPart;
}

Index BuildIndex()
{
    Index ix;
    std::vector<uint32_t> chests;
    g_step = "base chests";
    double t0 = DevMs();
    bool haveChests = BaseChests(chests, ix.why);
    double t1 = DevMs();
    if (!haveChests) {
        ix.inBase = ix.why == "no storage chests in this base";
        return ix;
    }
    ix.ok = true;
    g_durationWord = Lower(Trim(ui::LocaId(kLocaDuration)));
    ix.chestCount = static_cast<int>(chests.size());
    void* client = GameClient();
    std::map<uint32_t, size_t> byItem;
    std::map<std::string, size_t> byChestName;
    std::map<std::string, size_t> byGroup;
    std::map<uint32_t, std::vector<size_t>> itemGroups;
    std::set<uint32_t> grouped; // items whose kinds are counted
    std::map<int, size_t> byLevel; // item level -> its entry ("Level 55")
    g_devReadMs = g_devNameMs = g_devPosMs = 0;
    inv::CacheNames(client);
    for (uint32_t chest : chests) {
        float p[3];
        double q0 = DevMs();
        bool havePos = inv::Position(chest, p);
        g_devPosMs += DevMs() - q0;
        if (!havePos)
            continue;
        ix.positions[chest] = {p[0], p[1], p[2]};
        g_step = "chest name";
        char name[96] = {};
        double n0 = DevMs();
        bool named = inv::CustomName(client, chest, name, sizeof(name)) && name[0];
        g_devNameMs += DevMs() - n0;
        if (named) {
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
        double r0 = DevMs();
        bool readOk = inv::ReadOwner(chest, owner);
        g_devReadMs += DevMs() - r0;
        if (!readOk)
            continue;
        for (int pg = 0; pg < owner.pageCount; ++pg) {
            for (const inv::Stack& s : owner.pages[pg].slots) {
                if (!s.itemId || !s.count)
                    continue;
                // Blueprints are one entry, their tool's: "Construction Hammer - 3 in 1 chest".
                uint32_t tool = BlueprintTool(s.itemId);
                uint32_t key = tool ? tool : s.itemId;
                auto it = byItem.find(key);
                size_t e;
                if (it == byItem.end()) {
                    g_step = "item name";
                    std::string n = tool ? ToolName(tool) : SearchName(s.itemId);
                    e = ix.entries.size();
                    byItem[key] = e;
                    ix.entries.push_back({n, Lower(n), Kind::Item, key, 0, {}});
                } else {
                    e = it->second;
                }
                if (tool && std::find(ix.entries[e].items.begin(), ix.entries[e].items.end(), s.itemId) == ix.entries[e].items.end())
                    ix.entries[e].items.push_back(s.itemId); // lit in an opened chest
                if (grouped.insert(s.itemId).second) {
                    g_step = "item groups";
                    for (const auto& [gname, gkindAndRarity] : ItemGroups(s.itemId, ix.entries[e].name)) {
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
                }
                auto addTo = [&](size_t x) {
                    ix.entries[x].total += s.count;
                    if (std::find(ix.entries[x].chests.begin(), ix.entries[x].chests.end(), chest) == ix.entries[x].chests.end())
                        ix.entries[x].chests.push_back(chest);
                };
                addTo(e);
                for (size_t g : itemGroups[s.itemId])
                    addTo(g);
                // Its level, now and once fully upgraded: "Level 55" finds either.
                uint8_t level = 0, rarity = 0;
                if (inv::ItemLevel(s, level, rarity)) {
                    int maxLevel = MaxLevel(s.itemId, level, rarity);
                    for (int v : {static_cast<int>(level), maxLevel}) {
                        auto l = byLevel.find(v);
                        if (l == byLevel.end()) {
                            char text[64];
                            _snprintf_s(text, _TRUNCATE, strings::Get(strings::LevelN), v);
                            l = byLevel.emplace(v, ix.entries.size()).first;
                            ix.entries.push_back({text, Lower(text), Kind::Group, 0, 0, {}});
                            ix.entries.back().group = GroupLevel;
                        }
                        Entry& le = ix.entries[l->second];
                        if (std::find(le.items.begin(), le.items.end(), s.itemId) == le.items.end()) {
                            le.items.push_back(s.itemId);
                            le.kinds++;
                        }
                        addTo(l->second);
                        if (maxLevel == level)
                            break; // one level: counted once
                    }
                }
            }
        }
    }
    inv::CacheNames(nullptr);
    double t2 = DevMs();
    DevLog("  contents: positions %.0f ms, names %.0f ms, pages %.0f ms", g_devPosMs, g_devNameMs, g_devReadMs);
    g_step = "crafters";
    AddCrafters(ix, byItem);
    // Effects of what the base can make but no chest holds ("oxygen": the Kitchen, for Sushi).
    for (const auto& [itemId, e] : byItem) {
        if (grouped.count(itemId) || ix.entries[e].crafters.empty())
            continue;
        const uint8_t* info = inv::ItemInfo(itemId);
        uint8_t category;
        if (!info || !ReadByte(info + kItemCategory, category))
            continue;
        for (const std::string& effect : ItemEffects(info, category)) {
            std::string key = std::to_string(GroupEffect) + ":" + Lower(effect);
            auto g = byGroup.find(key);
            if (g == byGroup.end()) {
                g = byGroup.emplace(key, ix.entries.size()).first;
                ix.entries.push_back({effect, Lower(effect), Kind::Group, 0, 0, {}});
                ix.entries.back().group = GroupEffect;
            }
            Entry& ge = ix.entries[g->second];
            const Entry& item = ix.entries[e];
            ge.kinds++;
            ge.items.push_back(itemId);
            ge.made++;
            ge.madeLocked += item.locked ? 1 : 0;
            for (uint32_t c : item.crafters)
                if (std::find(ge.crafters.begin(), ge.crafters.end(), c) == ge.crafters.end())
                    ge.crafters.push_back(c);
        }
    }
    double t3 = DevMs();
    g_step = "placed";
    AddPlaced(ix, byItem);
    DevLog("  index steps: chests found %.0f ms, contents %.0f ms, crafters %.0f ms, placed %.0f ms", t1 - t0, t2 - t1, t3 - t2, DevMs() - t3);
    return ix;
}

// The Skills page: its skills by name, with their descriptions (the game's language).
Index BuildSkillIndex()
{
    Index ix;
    ix.skills = true;
    g_step = "skills";
    for (skills::Skill& s : skills::List(ix.why)) {
        Entry e{s.name, Lower(s.name), Kind::Skill, 0, 0, {}};
        e.desc = s.desc;
        e.descLower = Lower(s.desc);
        e.nodes = std::move(s.nodes);
        ix.entries.push_back(std::move(e));
    }
    // The tree's categories (class labels), listed first among equal matches.
    std::vector<Entry> categories;
    for (skills::Category& c : skills::Categories()) {
        Entry e{c.name, Lower(c.name), Kind::Skill, 0, 0, {}};
        e.nodes = std::move(c.nodes);
        e.category = true;
        e.centre[0] = c.centre[0];
        e.centre[1] = c.centre[1];
        categories.push_back(std::move(e));
    }
    ix.entries.insert(ix.entries.begin(), std::make_move_iterator(categories.begin()), std::make_move_iterator(categories.end()));
    ix.ok = !ix.entries.empty();
    return ix;
}

// The skills on the Skills page, the base's chests anywhere else.
Index BuildAny()
{
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    Index ix = skills::IsScreen(Screen()) ? BuildSkillIndex() : BuildIndex();
    QueryPerformanceCounter(&b);
    DevLog("  index built in %.0f ms", 1000.0 * (b.QuadPart - a.QuadPart) / f.QuadPart);
    return ix;
}

using match::Rank;

void UpdateSuggestions()
{
    std::string q = Lower(Trim(g_typed));
    const std::vector<std::string> words = match::Words(q);
    int hidden = 0;
    std::vector<std::pair<int, int>> ranked; // rank, entry
    for (int i = 0; i < static_cast<int>(g_index.entries.size()); ++i) {
        const Entry& e = g_index.entries[i];
        int r = q.empty() && g_browse ? 3 : Rank(e.lower, q);
        if (r < 0 && !q.empty() && e.kind == Kind::Skill && e.descLower.find(q) != std::string::npos)
            r = 4; // a skill whose description has it
        if (r < 0 && !(e.kind == Kind::Group && e.group == GroupLevel)) { // a level matches as a whole: "level 99" isn't every level
            int w = match::WordsRank(e.lower, e.descLower, words);
            if (!g_exact)
                r = w;
            else if (w >= 0)
                ++hidden;
        } // "ring mana": Ring of Mana, then names with some words
        if (r >= 0)
            ranked.push_back({r, i});
    }
    const auto& es = g_index.entries;
    std::sort(ranked.begin(), ranked.end(), [&](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        if (a.first != b.first)
            return a.first < b.first;
        const Entry& x = es[a.second];
        const Entry& y = es[b.second];
        if (x.category != y.category)
            return x.category;
        if (x.chests.size() != y.chests.size())
            return x.chests.size() > y.chests.size();
        if (x.total != y.total)
            return x.total > y.total;
        return x.lower < y.lower;
    });
    g_hiddenByExact = hidden;
    g_suggested.clear();
    for (size_t i = 0; i < ranked.size() && static_cast<int>(i) < (g_index.skills ? kMaxSkillSuggestions : kMaxSuggestions); ++i)
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
    if (e.kind == Kind::Skill && e.category)
        return std::string(strings::Get(strings::KindCategory)) + ": " + name + " (" + std::to_string(e.nodes.size()) + ")";
    if (e.kind == Kind::Skill) {
        std::string line = e.nodes.size() > 1 ? name + " (" + std::to_string(e.nodes.size()) + ")" : name;
        // Found by its description: the words around the match, the whole line short enough not
        // to wrap (a wrapped line would push the lines below out of their rows).
        std::string q = Lower(Trim(g_typed));
        size_t at = q.empty() || e.lower.find(q) != std::string::npos ? std::string::npos : e.descLower.find(q);
        if (at == std::string::npos && !q.empty() && e.lower.find(q) == std::string::npos)
            for (const std::string& w : match::Words(q)) // found by its words: the first one the name lacks
                if (e.lower.find(w) == std::string::npos && (at = e.descLower.find(w)) != std::string::npos)
                    break;
        if (at != std::string::npos) {
            constexpr size_t kMaxLine = 46;
            auto chars = [](const std::string& t) {
                size_t n = 0;
                for (unsigned char c : t)
                    n += (c & 0xC0) != 0x80;
                return n;
            };
            size_t used = chars(line) + 3; // the dash
            size_t room = used + 12 < kMaxLine ? kMaxLine - used : 12;
            size_t from = 0;
            if (e.descLower.size() == e.desc.size() && at > 16) {
                from = at - 16;
                // Start at a word (within a few characters; languages without spaces start anywhere).
                size_t space = e.desc.find(' ', from);
                if (space != std::string::npos && space < at && space - from <= 12)
                    from = space + 1;
                while (from > 0 && (static_cast<unsigned char>(e.desc[from]) & 0xC0) == 0x80)
                    --from; // not inside a character
            }
            static const char kEllipsis[] = "\xE2\x80\xA6";
            line += kDash + std::string(from ? kEllipsis : "") + Shorten(e.desc.substr(from), from ? room - 1 : room);
        }
        return line;
    }
    if (e.kind == Kind::Group && e.group == GroupLevel)
        return name + kDash + strings::ItemCount(e.total, n);
    if (e.kind == Kind::Group) {
        strings::Id label = e.group == GroupRarity ? strings::KindRarity
                            : e.group == GroupPerk ? strings::KindPerk
                            : e.group == GroupItemType ? strings::KindItemType
                            : e.group == GroupEffect ? strings::KindEffect
                                                       : strings::KindCategory;
        std::string count = !n && e.made ? strings::Get(e.made == e.madeLocked ? strings::MadeHereLocked : strings::MadeHere)
                                         : strings::KindCount(e.kinds - e.made, n);
        return std::string(strings::Get(label)) + ": " + name + kDash + count;
    }
    if (!n && !e.placed.empty())
        return name + kDash + strings::PlacedCount(static_cast<int>(e.placed.size()));
    if (!n && !e.crafters.empty())
        return name + kDash + strings::Get(e.locked ? strings::MadeHereLocked : strings::MadeHere);
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
        lines.push_back(strings::Get(g_index.skills ? strings::SkillsUnreadable : g_index.inBase ? strings::NoChests : strings::NotInBase));
    } else if (Trim(g_typed).empty() && !g_browse) {
        lines.push_back(strings::Get(g_index.skills ? strings::SkillPrompt : strings::TypePrompt));
    } else if (g_suggested.empty()) {
        std::string line = Format(g_index.skills ? strings::SkillNoMatch : strings::NoMatch, Trim(g_typed));
        if (g_exact && g_hiddenByExact > 0) // what unticking Exact match would show
            line += " " + strings::HiddenCount(g_hiddenByExact);
        lines.push_back(line);
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

// The label of each part of a hint line ("Tab  choose     Click  choose     Enter  find": choose, choose, find).
std::vector<std::string> HintLabels(const std::string& line)
{
    std::vector<std::string> labels;
    size_t from = 0;
    while (from < line.size()) {
        size_t gap = line.find("     ", from);
        std::string part = line.substr(from, gap == std::string::npos ? std::string::npos : gap - from);
        from = gap == std::string::npos ? line.size() : gap + 5;
        size_t cut = part.rfind("  ");
        if (cut == std::string::npos)
            return {};
        labels.push_back(part.substr(cut + 2));
    }
    return labels;
}

// The game's own tips under the buttons, in this language's words, for the device the game is showing.
void ShowTips()
{
    using namespace greeneye::input;
    const bool pad = greeneye::hints::PadInUse();
    static int shownFor = -1;
    static uint64_t shownKeys = 0;
    const uint64_t keys = static_cast<uint64_t>(g_keys.confirm.keys[0].vk) | static_cast<uint64_t>(g_keys.next.keys[0].vk) << 8 |
                          static_cast<uint64_t>(g_keys.close.keys[0].vk) << 16 | static_cast<uint64_t>(g_keys.confirm.pads[0].buttons) << 24 |
                          static_cast<uint64_t>(g_keys.down.pads[0].buttons) << 40 | static_cast<uint64_t>(g_index.ok) << 56;
    if (shownFor == static_cast<int>(pad) && shownKeys == keys && g_tipsShown)
        return;
    shownFor = pad;
    shownKeys = keys;
    g_tipsShown = true;
    const std::vector<std::string> open = HintLabels(strings::Get(strings::Hint));
    const std::vector<std::string> closing = HintLabels(strings::Get(strings::HintClose));
    std::vector<ui::DialogTip> tips;
    auto add = [&](const std::string& label, const Binding& binding) {
        if (greeneye::hints::ShowsOn(binding, pad))
            tips.push_back({label, binding});
    };
    if (open.size() == 3 && closing.size() == 1) {
        if (!g_index.ok) {
            add(closing[0], g_keys.close);
        } else if (pad) {
            add(open[0], g_keys.down);
            add(open[2], g_keys.confirm);
            add(closing[0], g_keys.close);
        } else {
            add(open[0], g_keys.next);
            add(open[2], MakeBinding("Mouse1")); // a click finds
            add(open[2], g_keys.confirm);
        }
    }
    ui::SetDialogTips(g_dialog, tips);
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
    g_resumeField = false;
    ui::ReleaseDialogInput();
    g_open = false;
    LockCamera(false);
    CatchWheel(false);
    int screen = Screen();
    // Escape on the screen behind the search also opens the system menu: close it again.
    // A search closed before its screen arrived: the command is on its way, and the empty screen
    // would stay, hiding the HUD.
    if (g_openedInventory && (!g_screenReached || screen == kScreenBehindSearch ||
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
    ShareFound(false);
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
    ShareFound(true);
}

#ifdef FIND_DEV
// Copies n bytes from memory that may not be readable.
bool DevCopy(void* to, const void* from, size_t n)
{
    __try {
        memcpy(to, from, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif

// Switches Exact match (the dialog's checkbox, its key or button) and remembers it in find.ini.
void ToggleExact(bool byKey)
{
    g_exact = !g_exact;
    if (byKey)
        g_exactAt = GetTickCount64();
    SaveExactMatch(g_exact);
    Log("search: exact match %s", g_exact ? "on" : "off");
    g_notice.clear();
    if (!Trim(g_typed).empty())
        UpdateSuggestions();
    ShowMessage();
}

// Text of several words that matches nothing as a whole: every entry with all the words, else
// the entries with the most of them (as the list ranks them).
template <class Add>
void AddByWords(const std::string& q, bool withDesc, Add&& add)
{
    const std::vector<std::string> words = match::Words(q);
    if (words.size() < 2)
        return;
    std::vector<int> ranks;
    int best = -1;
    for (const Entry& e : g_index.entries) {
        int r = e.kind == Kind::Group && e.group == GroupLevel ? -1 : match::WordsRank(e.lower, withDesc ? e.descLower : std::string(), words);
        ranks.push_back(r);
        if (r >= 0 && (best < 0 || r < best))
            best = r;
    }
    if (best < 0)
        return;
    for (size_t i = 0; i < ranks.size(); ++i)
        if (ranks[i] >= 0 && (match::IsAll(best) ? match::IsAll(ranks[i]) : ranks[i] == best))
            add(g_index.entries[i]);
}

// The Skills page: lights every node whose skill's name or description has the text for 5 s
// (a picked suggestion: that skill), and with one node the view glides to it.
constexpr unsigned kSkillPulseMs = 5000;
bool SearchSkills(const std::string& text, const std::string& q, bool picked)
{
    std::vector<uint32_t> nodes;
    std::string matched;
    const Entry* category = nullptr; // a category named exactly so: the view glides to its middle
    auto add = [&](const Entry& e) {
        if (matched.size() < 600)
            matched += (matched.empty() ? "" : ", ") + e.name + (e.category ? " (category)" : "");
        if (e.category && e.lower == q)
            category = &e;
        for (uint32_t n : e.nodes)
            if (std::find(nodes.begin(), nodes.end(), n) == nodes.end())
                nodes.push_back(n);
    };
    if (picked)
        for (const Entry& e : g_index.entries)
            if (e.lower == q)
                add(e);
    if (nodes.empty())
        for (const Entry& e : g_index.entries)
            if (e.lower.find(q) != std::string::npos || e.descLower.find(q) != std::string::npos)
                add(e);
    if (nodes.empty() && !g_exact)
        AddByWords(q, true, add);
    if (nodes.empty()) {
        g_notice = Format(strings::SkillNoMatch, Trim(text));
        Log("find skill '%s': nothing among %zu skills", q.c_str(), g_index.entries.size());
        return false;
    }
    skills::Highlight(nodes, kSkillPulseMs, category ? category->centre : nullptr);
    Log("find skill '%s': %zu nodes light up%s: %s", q.c_str(), nodes.size(), nodes.size() == 1 ? " (centred)" : "", matched.c_str());
    return true;
}

// Returns true when chests were found (the dialog closes), false to keep it open.
bool Search(const std::string& text, bool picked = false)
{
    std::string q = Lower(Trim(text));
    if (q.empty())
        return false;
    if (!g_index.ok && q.rfind("@dev-", 0) != 0) {
        g_notice = strings::Get(g_index.skills ? strings::SkillsUnreadable : g_index.inBase ? strings::NoChests : strings::NotInBase);
        Log("find '%s': %s", q.c_str(), g_index.why.c_str());
        return false;
    }
#ifdef FIND_DEV
    if (q == "@dev-detect") {
        // Runs the Inventory/Position type detection anywhere (no base needed) and names the types.
        void* client = GameClient();
        uint32_t handle = client ? At<uint32_t>(client, kClientPlayer) : 0;
        uint32_t playerId = handle ? (handle & 0x3f) + 1 : 0;
        float pos[3] = {};
        bool ready = playerId && PlayerPosition(pos) && inv::Ready(playerId, nullptr, 0);
        if (ready)
            inv::DetectPosition(playerId, pos);
        void* reg = inv::InspectRegistry(0);
        char posName[96] = "?", invName[96] = "?";
        if (reg) {
            ecs::Name(reg, inv::TypePosition(), posName, sizeof(posName));
            ecs::Name(reg, inv::TypeInventory(), invName, sizeof(invName));
        }
        Log("dev detect: ready %d, Position type %u = %s, Inventory type %u = %s", ready, inv::TypePosition(), posName,
            inv::TypeInventory(), invName);
        return false;
    }
    if (q == "@dev-tip") {
        g_devNoTip = !g_devNoTip;
        Log("dev: Find's tip mode %d", g_devNoTip);
        return false;
    }
    if (q == "@dev-skills") {
        skills::DevDump();
        return false;
    }
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
    if (q == "@dev-icons") {
        g_iconPreviewUntil = GetTickCount64() + 30000;
        for (size_t i = 0; i < sizeof(kPreviewIcons) / sizeof(kPreviewIcons[0]); ++i)
            Log("dev icons %zu: %s (S+0x%zx)", i + 1, kPreviewIcons[i].name, kPreviewIcons[i].asset);
        return false;
    }
    if (q == "@dev-compass") {
        g_compassSign = -g_compassSign;
        auto* client = static_cast<uint8_t*>(ui::Client());
        float yaw = 0, px = 0, pz = 0;
        if (client) {
            hot::Read(client + 0x1da0, &yaw, sizeof(yaw));
            hot::Read(client + 0x38, &px, sizeof(px));
            hot::Read(client + 0x40, &pz, sizeof(pz));
        }
        Log("dev compass: sign %d, yaw %.3f, player %.1f %.1f", g_compassSign, yaw, px, pz);
        return false;
    }
    if (q == "@dev-effects") {
        // Each indexed item's uiValues / uiBuffValues labels.
        int shown = 0;
        for (const auto& e : g_index.entries) {
            if (e.kind != Kind::Item || shown >= 400)
                continue;
            const uint8_t* info = inv::ItemInfo(e.itemId);
            if (!info)
                continue;
            std::string line;
            for (size_t field : {kItemUiValues, kItemUiBuffValues}) {
                int32_t blob[2] = {};
                hot::Read(info + field, blob, sizeof(blob));
                line += field == kItemUiValues ? " | ui:" : " | buff:";
                for (int32_t k = 0; blob[0] && k < blob[1] && k < 12; ++k) {
                    uint32_t v[3] = {};
                    hot::Read(info + field + blob[0] + kUiValueSize * k, v, sizeof(v));
                    char extra[32]; snprintf(extra, sizeof(extra), " id %08x fmt %u", v[0], v[1] & 0xff);
                    line += " [" + ui::LocaId(v[0]) + " " + std::to_string(static_cast<int32_t>(v[2])) + extra + "]";
                }
            }
            if (line.find('[') != std::string::npos) {
                ++shown;
                Log("dev effects %s:%s", e.name.c_str(), line.c_str());
            }
        }
        return false;
    }
    if (q == "@dev-levels") {
        // Item levels: per-instance ItemState (pide entity, +16 rarity, +17 level) against the kind's level range and perks.
        void* reg = inv::InspectRegistry(0);
        uint16_t stateType = 0;
        std::map<std::string, int> stats; // "rarity r perks p" -> count
        int upgraded = 0, total = 0;
        for (const auto& entry : g_index.positions) {
            inv::Owner o;
            if (!reg || !inv::ReadOwner(entry.first, o))
                continue;
            for (int p = 0; p < o.pageCount; ++p)
                for (const auto& s : o.pages[p].slots) {
                    const uint8_t* info = s.itemId && s.pide ? inv::ItemInfo(s.itemId) : nullptr;
                    if (!info)
                        continue;
                    if (!stateType) { // the pide component that starts with a slot id and the item id at +8
                        uint16_t t[640];
                        int n = inv::Types(reg, s.pide, t, 640);
                        for (int k = 0; k < n && !stateType; ++k)
                            if (auto* d = static_cast<const uint8_t*>(inv::Data(reg, t[k], s.pide))) {
                                uint32_t id = 0; hot::Read(d + 8, &id, 4);
                                if (id == s.itemId) stateType = t[k];
                            }
                        Log("dev levels: state type %u", stateType);
                    }
                    auto* d = stateType ? static_cast<const uint8_t*>(inv::Data(reg, stateType, s.pide)) : nullptr;
                    if (!d) continue;
                    uint8_t st[18] = {}; hot::Read(d, st, sizeof(st));
                    uint32_t range[2] = {}; hot::Read(info + 0x674, range, sizeof(range));
                    uint32_t perks[5] = {}; hot::Read(info + kItemPerkIds, perks, sizeof(perks));
                    int np = 0; for (uint32_t pk : perks) np += pk != 0;
                    uint16_t flags = st[14] | st[15] << 8;
                    ++total;
                    stats["rarity " + std::to_string(st[16]) + " perks " + std::to_string(np) + ((flags & 0x10) ? " hasLevel" : " noLevel")]++;
                    if (range[0] == range[1] && st[17] != range[0] && upgraded < 15) {
                        ++upgraded;
                        Log("dev level upgraded? %s level %u range %u-%u rarity %u perks %d flags %04x", ItemName(s.itemId).c_str(), st[17], range[0], range[1], st[16], np, flags);
                    }
                }
        }
        for (const auto& [k, v] : stats)
            Log("dev levels: %s: %d", k.c_str(), v);
        Log("dev levels: %d items with state", total);
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
    if (q.rfind("@dev-near ", 0) == 0) {
        // Entities near the player with their component type names: "@dev-near 30"
        float at[3], dist[256];
        uint32_t ids[256];
        void* reg = inv::InspectRegistry(0);
        int n = PlayerPosition(at) ? inv::EntitiesNear(reg, at, static_cast<float>(atof(q.c_str() + 10)), ids, dist, 256) : 0;
        for (int i = 0; i < n; ++i) {
            uint16_t types[640];
            int t = inv::Types(reg, ids[i], types, 640);
            std::string line;
            for (int k = 0; k < t; ++k) {
                char name[96];
                line += " " + std::to_string(types[k]) + "=" + (ecs::Name(reg, types[k], name, sizeof(name)) ? name : "?");
            }
            Log("dev near %u %.1fm:%s", ids[i], dist[i], line.c_str());
        }
        return false;
    }
    if (q.rfind("@dev-data ", 0) == 0) {
        // Component bytes: "@dev-data 1263 412 64"
        unsigned id = 0, type = 0, size = 64;
        sscanf_s(q.c_str() + 10, "%u %u %u", &id, &type, &size);
        auto* data = static_cast<uint8_t*>(inv::Data(inv::InspectRegistry(0), static_cast<uint16_t>(type), id));
        for (unsigned off = 0; data && off < size && off < 0x400; off += 16) {
            uint8_t raw[16];
            hot::Read(data + off, raw, 16);
            char hex[64] = {};
            for (int b = 0; b < 16; ++b) sprintf_s(hex + b * 3, 4, "%02x ", raw[b]);
            Log("dev data %u/%u +%03x: %s", id, type, off, hex);
        }
        return false;
    }
    if (q.rfind("@dev-recipe ", 0) == 0) {
        // Every recipe of the items named so: "@dev-recipe coconut milk"
        std::string name = q.substr(12);
        knowledge::Refresh(recipes::IsRecipe);
        for (const Entry& e : g_index.entries)
            if (e.kind == Kind::Item && e.lower == name)
                recipes::DevDescribe(e.itemId, knowledge::Known);
        return false;
    }
    if (q.rfind("@dev-placed ", 0) == 0) {
        // Where a placed entity names the item it was built from: "@dev-placed 25" looks in the
        // record (and 0x100 bytes behind each pointer in it) of every entity within 25 m for the
        // placed-entity template GUID (ItemInfo + 0x3c0) of an item a recipe makes.
        std::map<std::string, uint32_t> byGuid;
        for (uint32_t item : recipes::AllOutputs()) {
            const uint8_t* info = inv::ItemInfo(item);
            uint8_t g[16];
            if (info && DevCopy(g, info + 0x3c0, 16) && (g[0] | g[4] | g[8] | g[12] | g[15]))
                byGuid[std::string(reinterpret_cast<char*>(g), 16)] = item;
        }
        float at[3], dist[512];
        uint32_t ids[512];
        void* reg = inv::InspectRegistry(0);
        int n = PlayerPosition(at) ? inv::EntitiesNear(reg, at, static_cast<float>(atof(q.c_str() + 12)), ids, dist, 512) : 0;
        int logged = 0;
        for (int i = 0; i < n && logged < 60; ++i) {
            uint8_t* rec = inv::Record(reg, ids[i]);
            uint8_t head[0x80];
            if (!rec || !DevCopy(head, rec, sizeof(head)))
                continue;
            auto check = [&](const uint8_t* bytes, size_t len, size_t recOff, int behind) {
                for (size_t o = 0; o + 16 <= len; o += 4) {
                    auto it = byGuid.find(std::string(reinterpret_cast<const char*>(bytes + o), 16));
                    if (it != byGuid.end() && logged < 60) {
                        ++logged;
                        DevLog("dev placed %u (%.1f m): record +0x%zx%s +0x%zx = %s", ids[i], dist[i], recOff,
                               behind ? " ->" : "", o, SearchName(it->second).c_str());
                    }
                }
            };
            check(head, sizeof(head), 0, 0);
            for (size_t k = 0; k < sizeof(head); k += 8) {
                uint64_t ptr;
                memcpy(&ptr, head + k, 8);
                uint8_t behind[0x100];
                if (ptr > 0x10000 && ptr < 0x7fffffffffff && !(ptr & 7) && DevCopy(behind, reinterpret_cast<void*>(ptr), sizeof(behind)))
                    check(behind, sizeof(behind), k, 1);
            }
        }
        Log("dev placed: %d entities, %zu placeable items, %d matches", n, byGuid.size(), logged);
        return false;
    }
    if (q.rfind("@dev-item ", 0) == 0) {
        // Where an item's ItemInfo lives: "@dev-item 3244772462"
        uint32_t id = static_cast<uint32_t>(strtoul(q.c_str() + 10, nullptr, 10));
        const uint8_t* info = inv::ItemInfo(id);
        MEMORY_BASIC_INFORMATION mbi{};
        if (info)
            VirtualQuery(info, &mbi, sizeof(mbi));
        Log("dev item %u: info %p allocation %p", id, info, mbi.AllocationBase);
        return false;
    }
    if (q.rfind("@dev-u32 ", 0) == 0) {
        // Entities whose component data holds a value, in every registry: "@dev-u32 841ad755"
        std::istringstream in(q.substr(9));
        void* regs[16];
        int rn = inv::Registries(regs, 16);
        for (std::string hex; in >> hex;) {
            uint32_t value = static_cast<uint32_t>(strtoul(hex.c_str(), nullptr, 16));
            for (int r = 0; r < rn; ++r) {
                inv::Hit hits[64];
                int n = inv::ScanValue(regs[r], value, hits, 64);
                for (int i = 0; i < n; ++i) {
                    char name[96];
                    Log("dev u32 %08x reg %d: entity %u type %u=%s +0x%x", value, r, hits[i].entity, hits[i].type,
                        ecs::Name(regs[r], hits[i].type, name, sizeof(name)) ? name : "?", hits[i].offset);
                }
                if (n)
                    Log("dev u32 %08x reg %d: %d hits", value, r, n);
            }
        }
        return false;
    }
    if (q == "@dev-plain") {
        g_devPlain = !g_devPlain;
        Log("dev: small-altar mode %s", g_devPlain ? "on" : "off");
        return false;
    }
    if (q.rfind("@dev-glow ", 0) == 0) {
        // Lights any entities, chests or not: "@dev-glow 1263 1031"
        std::vector<uint32_t> ids;
        std::istringstream in(q.substr(10));
        for (uint32_t id; in >> id;) {
            float at[3];
            if (!inv::Position(id, at)) { Log("dev glow %u: no position", id); continue; }
            g_index.positions[id] = {at[0], at[1], at[2]};
            ids.push_back(id);
        }
        InterlockedExchange64(&g_devHoldUntil, GetTickCount64() + 120000);
        SetFoundItems({});
        StartGlows(ids);
        return true;
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
    if (g_index.skills)
        return SearchSkills(text, q, picked);
    nativehighlight::SetStressTest(false);
    std::vector<uint32_t> chests;
    std::vector<uint32_t> crafters; // NPCs and stations that make what matched
    std::vector<uint32_t> placed;   // what matched, standing in the base
    std::vector<uint32_t> items;
    std::string matched; // for the log
    int locked = 0;      // matched items whose crafters can't make them yet
    auto add = [&](const Entry& e) {
        if (matched.size() < 600)
            matched += (matched.empty() ? "" : ", ") + e.name + (e.kind == Kind::Group ? " (kind)" : e.kind == Kind::Chest ? " (chest)" : "");
        for (uint32_t c : e.chests)
            if (std::find(chests.begin(), chests.end(), c) == chests.end())
                chests.push_back(c);
        for (uint32_t c : e.crafters)
            if (std::find(crafters.begin(), crafters.end(), c) == crafters.end())
                crafters.push_back(c);
        for (uint32_t c : e.placed)
            if (std::find(placed.begin(), placed.end(), c) == placed.end())
                placed.push_back(c);
        if (e.kind == Kind::Item)
            items.push_back(e.itemId);
        for (uint32_t id : e.items)
            items.push_back(id);
        locked += e.locked ? 1 : 0;
    };
    // A suggestion picked with Tab: just that item, chest or kind. Typed text: every name
    // containing it ("stone" finds Stone and Rough Stone Block).
    bool exact = false;
    for (const Entry& e : g_index.entries) {
        if (picked && Lower(e.name) == q) { // a blueprint matches by the hammer, picked by its own name
            add(e);
            exact = true;
        }
    }
    // Names with a word starting with the text ("arrow": Iron Arrow, not Narrow Wall); only if
    // there are none, names containing it anywhere.
    if (!exact) {
        for (const Entry& e : g_index.entries) {
            int r = Rank(e.lower, q);
            bool level = e.kind == Kind::Group && e.group == GroupLevel;
            if (r >= 0 && r <= (level ? 0 : 2)) // "level 5" is Level 5, not 50 to 59 as well
                add(e);
        }
        if (chests.empty() && crafters.empty() && placed.empty())
            for (const Entry& e : g_index.entries)
                if (e.lower.find(q) != std::string::npos && !(e.kind == Kind::Group && e.group == GroupLevel))
                    add(e);
        if (chests.empty() && crafters.empty() && placed.empty() && !g_exact)
            AddByWords(q, false, add);
    }
    if (chests.empty() && crafters.empty() && placed.empty()) {
        // A search that finds nothing ends the one before: its chests and items kept glowing, and looked like this one's.
        EndSearch();
        SetFoundItems({});
        g_notice = Format(strings::NoMatch, Trim(text));
        Log("find '%s': nothing in %d chests", q.c_str(), g_index.chestCount);
#ifdef FIND_DEV
        for (const Entry& e : g_index.entries)
            DevLog("  index: %s [%d] %zu chests", e.name.c_str(), static_cast<int>(e.kind), e.chests.size());
#endif
        return false;
    }
    int chestsLit = static_cast<int>(chests.size());
    for (uint32_t c : placed)
        if (std::find(chests.begin(), chests.end(), c) == chests.end())
            chests.push_back(c);
    g_crafterGlows.clear();
    for (uint32_t c : crafters)
        if (std::find(chests.begin(), chests.end(), c) == chests.end()) {
            chests.push_back(c);
            g_crafterGlows.push_back(c);
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
    Log("find '%s': %d of %d chests glow, %d items light up in them, %d placed in the base, %d NPCs and stations make them (%d not unlocked)%s",
        q.c_str(), chestsLit, g_index.chestCount, static_cast<int>(items.size()), static_cast<int>(placed.size()),
        static_cast<int>(crafters.size()), locked,
        g_nativeReady ? "" : " (no chest outline: see above)");
    return true;
}

#ifdef FIND_DEV
// The search dialog without keys, mouse or focus (FindDialogTest). Tick takes these on the game
// thread; a click goes through the real hit test at the middle of the wanted line.
struct DialogTestState {
    bool open = false, setText = false, enter = false, cancel = false;
    int screen = -1, typing = -1; // read in Tick: G is only set while the UI is being built
    std::string text;
    int clickLine = -1;
} g_dialogTest;
#endif

bool Edge(int slot, bool down)
{
    bool pressed = down && !g_navDown[slot];
    g_navDown[slot] = down;
    return pressed;
}

// Hotkey presses as they arrive. The check in Tick sees what is held at that moment, and a key
// from a programmable keypad or mouse macro goes down and up within one frame, so it would be
// missed. The game reads its keys as raw input (WM_INPUT); a message hook on the game window's
// thread reads the same input as it leaves the queue and leaves it for the game.
HHOOK g_keyHook = nullptr;
std::atomic<bool> g_hotkeyArrived{false};
bool g_rawShift = false, g_rawCtrl = false, g_rawAlt = false; // modifiers held, from the same input

LRESULT CALLBACK KeyHookProc(int code, WPARAM wp, LPARAM lp)
{
    hot::Lease lease;
    auto* m = reinterpret_cast<const MSG*>(lp);
    if (lease && code == HC_ACTION && wp == PM_REMOVE && m->message == WM_INPUT) {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(m->lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            raw.header.dwType == RIM_TYPEKEYBOARD) {
            bool down = !(raw.data.keyboard.Flags & RI_KEY_BREAK);
            USHORT vk = raw.data.keyboard.VKey;
            if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT)
                g_rawShift = down;
            else if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)
                g_rawCtrl = down;
            else if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU)
                g_rawAlt = down;
            else if (down)
                for (int i = 0; i < g_keys.open.keyCount; ++i) {
                    const greeneye::input::KeyChord& key = g_keys.open.keys[i];
                    if (vk == key.vk && g_rawShift == key.shift && g_rawCtrl == key.ctrl && g_rawAlt == key.alt)
                        g_hotkeyArrived = true;
                }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void HookKeys(bool on)
{
    if (!on) {
        if (g_keyHook)
            UnhookWindowsHookEx(g_keyHook);
        g_keyHook = nullptr;
        return;
    }
    if (g_keyHook)
        return;
    static ULONGLONG lastTry = 0;
    if (GetTickCount64() - lastTry < 5000) // until the game window is there
        return;
    lastTry = GetTickCount64();
    HWND hwnd = GameWindow();
    DWORD thread = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : 0;
    if (!thread)
        return;
    g_keyHook = SetWindowsHookExW(WH_GETMESSAGE, &KeyHookProc, nullptr, thread);
    Log(g_keyHook ? "hotkey: watching the game's keyboard input" : "hotkey: no message hook (error %lu), short key presses may be missed",
        GetLastError());
}

// What the game reads from a pad: the opening chord's button once its modifier is held, and the open
// search's buttons (all hidden until they come up, so closing with B does not also open a menu).
void FindPadFilter(XINPUT_GAMEPAD& pad, DWORD user)
{
    using namespace greeneye::input;
    static PadHide hide;
    uint16_t mask = 0;
    if (GetTickCount64() < g_padLiveUntil.load()) {
        mask = g_padHiddenNow.load();
        mask |= PadHideMask(g_openPad.Get(), PadState{pad.wButtons, pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD, pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD});
    }
    pad.wButtons = hide.Apply(pad.wButtons, user, mask);
}

// A crafting window (a station's or an NPC's): opened from playing near a found station or NPC,
// the found items light up in its recipe list for a few seconds, as in a found chest's window.
bool ChestWindowOpened();
int FaultFilter(EXCEPTION_POINTERS* e, const char* what);
int g_lastScreen = -1;
bool g_craftLight = false;
ULONGLONG g_craftOpenedAt = 0;
void CraftingWindowTick()
{
    ULONGLONG now = GetTickCount64();
    int screen = Screen();
    if (screen != g_lastScreen) {
        bool fromPlaying = g_lastScreen == kScreenPlaying;
        g_lastScreen = screen;
        if (g_craftLight)
            ui::LightIcons(0, 0.0f);
        g_craftLight = false;
        // Only the crafting screen: a chest's screen (22) is the chest window's, which lights its own items once.
        bool window = fromPlaying && !g_open && screen == kScreenCrafting;
        if (window && !g_foundItems.empty() && now < g_glowsUntil) {
            __try {
                g_craftLight = ChestWindowOpened();
            } __except (FaultFilter(GetExceptionInformation(), "crafting window")) {
                g_craftLight = false;
            }
            g_craftOpenedAt = now;
            DevLog("screen %d opened from playing: %s", screen, g_craftLight ? "found items light up" : "not at a found station");
        }
    }
    if (!g_craftLight)
        return;
    ULONGLONG shown = now - g_craftOpenedAt;
    if (shown >= kIconShowMs) {
        ui::LightIcons(0, 0.0f);
        g_craftLight = false;
        return;
    }
    float pulse = 0.55f + 0.35f * std::sin(static_cast<float>(now % kIconPulseMs) / kIconPulseMs * 6.2831853f);
    if (shown > kIconShowMs - kFadeOutMs)
        pulse *= static_cast<float>(kIconShowMs - shown) / kFadeOutMs;
    ui::LightIcons(kIconColour, pulse);
}

SRWLOCK g_requestLock = SRWLOCK_INIT;
char g_request[128]{};
bool g_requestPending = false;

void DrawReskillHints(); // the Reskill page's bottom bar, with Find's tip

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
    CraftingWindowTick();
    skills::Draw(Screen());
    DrawReskillHints();
    char request[128]{};
    bool pending;
    AcquireSRWLockExclusive(&g_requestLock);
    pending = g_requestPending && (!g_open || !strcmp(g_request, "@dev-recover-ui") || !strcmp(g_request, "@dev-input-state"));
    if (pending) { memcpy(request, g_request, sizeof(request)); g_requestPending = false; }
    ReleaseSRWLockExclusive(&g_requestLock);
    if (pending) {
        if (!strcmp(request, "@dev-input-state") || !strcmp(request, "@dev-recover-ui")) {
            Log("input diagnostic: screen=%d search=%d openedScreen=%d camera=%p wheel=%p escape=%d typing=%d",
                Screen(), g_open, g_openedInventory, g_lockedWindow, g_wheelWindow,
                (GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0, ui::TextInputFlag());
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
        if (!request[0]) { EndSearch(); skills::Clear(); }
        else { g_index = BuildAny(); Search(request); }
        return;
    }
    using namespace greeneye::input;
    auto held = [](int vk) { return KeyDown(vk); };
    bool focused = GameFocused();
    if (RefreshBindings(g_keys))
        Log("keys: open %s", BindingToText(g_keys.open).c_str());
    const PadState pad = ReadPad();
    const uint16_t padPressed = g_padNav.Update(pad.buttons, GetTickCount64());
    g_openPad.Set(g_keys.open);
    {
        const int playing = Screen();
        if (g_open || playing == kScreenPlaying || playing == kScreenInventory || skills::IsScreen(playing))
            g_padLiveUntil = GetTickCount64() + 250;
        g_padHiddenNow = g_open ? uint16_t(PadButtons(g_keys.up) | PadButtons(g_keys.down) | PadButtons(g_keys.close) | PadButtons(g_keys.exact) |
                                           (g_selected >= 0 ? PadButtons(g_keys.confirm) : 0))
                                : uint16_t(0);
    }
    // Modifiers must match exactly, so Shift+F does not also fire on Ctrl+Shift+F.
    bool down = focused && (KeysHeld(g_keys.open, held) || PadHeld(g_keys.open, pad));
    HookKeys(true);
    bool arrived = g_hotkeyArrived.exchange(false); // a press too short for the check above
    bool pressed = (down && !g_keyWasDown) || (arrived && focused);
    g_keyWasDown = down;
#ifdef FIND_DEV
    g_dialogTest.screen = Screen();
    g_dialogTest.typing = ui::TextInputFlag();
    if (g_dialogTest.open) {
        g_dialogTest.open = false;
        if (!g_open && (g_dialogTest.screen == kScreenPlaying || g_dialogTest.screen == kScreenInventory ||
                        skills::IsScreen(g_dialogTest.screen)))
            g_openPending = true;
    }
#endif
    // Only while playing, in the inventory or on the Skills page: not while typing in chat (43),
    // renaming (47), a signpost (19) or any other screen, where Shift+F is just a capital F.
    if (pressed && !g_open) {
        int screen = Screen();
        if (screen == kScreenPlaying || screen == kScreenInventory || skills::IsScreen(screen))
            g_openPending = true;
        else if (static bool seen[256]; screen >= 0 && screen < 256 && !seen[screen]) {
            seen[screen] = true;
            Log("hotkey: pressed on screen %d, where Find doesn't open", screen);
        }
    }
    // Open when the key comes up, so its letter has already gone to the game, not the field.
    if (g_openPending && !PartsHeld(g_keys.open, held, pad)) {
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
        g_index = BuildAny();
        g_lastText.clear();
        g_typed.clear();
        g_notice.clear();
        g_suggested.clear();
        g_browse = false;
        g_tipsShown = false;
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
        if (g_index.skills)
            CatchWheel(true); // the wheel scrolls the list, not the skill tree's zoom
        ShowMessage();
        Log("search opened%s: %s", g_index.skills ? " on the Skills page" : "", g_index.ok ? "index ready" : g_index.why.c_str());
        if (g_index.ok && !g_index.skills)
        {
            int groups = 0;
            for (const Entry& e : g_index.entries)
                groups += e.kind == Kind::Group;
            Log("  %d chests, %d names (%d kinds: categories, rarities, perks, item types; %d perk names known)",
                g_index.chestCount, static_cast<int>(g_index.entries.size()), groups, static_cast<int>(g_perkNames.size()));
        }
        return;
    }
#ifdef FIND_DEV
    if (!g_open) {
        g_dialogTest.setText = g_dialogTest.enter = g_dialogTest.cancel = false;
        g_dialogTest.clickLine = -1;
    }
#endif
    if (!g_open)
        return;
    ShowTips();

    if (g_openedInventory && Screen() == kScreenBehindSearch)
        g_screenReached = true;
    if (g_openedInventory && !g_screenLogged && GetTickCount64() - g_openedAt > 600) {
        g_screenLogged = true;
        DevLog("screen behind the search: %d", Screen());
    }
    // The screen the search opened was closed (Escape): close with it.
    // (Only once it was there: a screen that closes itself right away must not close the search.)
    bool escape = focused && (KeysHeld(g_keys.close, held) || PadHeld(g_keys.close, pad));
    bool escapePressed = escape && !g_escapeWasDown;
    if (escapePressed) g_escapeAt = GetTickCount64();
    g_escapeWasDown = escape;
    g_step = "dialog";
    // Let the native field process cancel/deactivate before retiring the dialog.
    int r = ui::DrawDialog(g_dialog);
#ifdef FIND_DEV
    if (g_dialogTest.cancel) r = 1;
    if (g_dialogTest.enter) r = 2;
    g_dialogTest.cancel = g_dialogTest.enter = false;
    if (g_dialogTest.setText) {
        g_dialogTest.setText = false;
        ui::SetDialogText(g_dialog, g_dialogTest.text);
    }
#endif
    if (escapePressed || r == 1) {
        CloseSearch(escapePressed ? " (Escape)" : " (cancelled)");
        return;
    }
    if (g_openedInventory && g_screenReached && Screen() != kScreenBehindSearch && GetTickCount64() - g_openedAt > 500) {
        CloseSearch(" (screen closed)");
        return;
    }
    if (g_index.skills && !skills::IsScreen(Screen())) {
        CloseSearch(" (Skills page closed)");
        return;
    }
    size_t len;
    const char* raw = ui::DialogText(g_dialog, &len);
    std::string text(raw ? raw : "", raw ? len : 0);
    // Outside a base there is nothing to search: the field is switched off (no caret, nothing
    // typed) and Enter closes.
    if (!g_index.ok) {
        ui::DisableDialogField();
        ui::SetDialogCheckbox(g_dialog, false, g_exact, "", g_keys.exact);
        if (r == 2 || Edge(4, focused && (KeysHeld(g_keys.confirm, held) || PadHeld(g_keys.confirm, pad)))) {
            CloseSearch(" (not in a base)");
            return;
        }
        if (!text.empty())
            ui::SetDialogText(g_dialog, "");
        return;
    }
    // Enter finds even when the field lost the keyboard to a click on the list (the dialog
    // itself would only take the field back with that press).
    // (The edge is read every frame: skipped while the dialog reported Enter itself, the key still held next frame
    // counted as a second press, searched again and took the field's keyboard away again.)
    bool confirmHeld = focused && (KeysHeld(g_keys.confirm, held) || PadHeld(g_keys.confirm, pad));
    bool confirmPressed = Edge(4, confirmHeld);
    if (r != 2 && confirmPressed && !Trim(text).empty())
        r = 2;
    // After Enter found nothing the field gets the keyboard back once Enter is up (while it is held the field
    // would take the same press again).
    if (g_resumeField && !confirmHeld && r != 2) {
        g_resumeField = false;
        if (!ui::ResumeDialogField())
            DevLog("dialog: the field could not take the keyboard back");
    }
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
        g_resumeField = true; // Enter ended the field's edit: it takes the keyboard back once Enter is up
        ShowMessage();
        return;
    }
    // Exact match: the checkbox after the tips (a click, below), Ctrl+E or Y.
    bool exactKey = focused && KeysHeld(g_keys.exact, held);
    bool exactPressed = (exactKey && !g_exactKeyDown) || (focused && PadPressed(g_keys.exact, padPressed, pad));
    g_exactKeyDown = exactKey;
    if (exactPressed)
        ToggleExact(exactKey);
    ui::SetDialogCheckbox(g_dialog, true, g_exact, strings::Get(strings::ExactMatch), g_keys.exact);
    bool next = Edge(0, focused && KeysHeld(g_keys.next, held)) || (focused && PadPressed(g_keys.next, padPressed, pad));
    bool previous = Edge(5, focused && KeysHeld(g_keys.previous, held)) || (focused && PadPressed(g_keys.previous, padPressed, pad));
    bool padStep = focused && (PadPressed(g_keys.up, padPressed, pad) || PadPressed(g_keys.down, padPressed, pad));
    bool up = Edge(1, focused && KeysHeld(g_keys.up, held)) || (focused && PadPressed(g_keys.up, padPressed, pad));
    bool downKey = Edge(2, focused && KeysHeld(g_keys.down, held)) || (focused && PadPressed(g_keys.down, padPressed, pad));
    ULONGLONG now = GetTickCount64();
    if (next || previous)
        g_tabAt = now;
    // A controller with nothing typed: the list shows everything in the base, for picking from.
    if (padStep && !g_browse && Trim(text).empty() && g_index.ok) {
        g_browse = true;
        UpdateSuggestions();
    }
    if (text != g_lastText && now - g_tabAt < kTabSpaceMs && text.size() == g_lastText.size() + 1 &&
        text.compare(0, g_lastText.size(), g_lastText) == 0 && isspace(static_cast<unsigned char>(text.back()))) {
        ui::SetDialogText(g_dialog, g_lastText); // the space Tab typed
        text = g_lastText;
    }
    if (text != g_lastText && now - g_exactAt < kTabSpaceMs && text.size() == g_lastText.size() + 1 &&
        text.compare(0, g_lastText.size(), g_lastText) == 0 && (text.back() == 'e' || text.back() == 'E')) {
        ui::SetDialogText(g_dialog, g_lastText); // the e of Ctrl+E
        text = g_lastText;
    }
    if (text != g_lastText) { // typed
        g_lastText = text;
        g_typed = text;
        g_browse = false;
        g_notice.clear();
        UpdateSuggestions();
        ShowMessage();
    }
    if ((next || previous || up || downKey) && !g_suggested.empty()) {
        int step = (up || previous) ? -1 : 1;
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
    // The mouse: the line under it is marked; a click on it finds it. It acts when the button comes back up: acting on the press
    // closed the dialog with the button still held, and the game took the held button for an attack.
    bool wasDown = g_navDown[3];
    bool click = Edge(3, focused && held(VK_LBUTTON));
    bool release = wasDown && !g_navDown[3] && !held(VK_LBUTTON);
    float mx, my;
    if (click && ui::CursorInUi(mx, my) && ui::DialogCheckboxAt(g_dialog, mx, my)) {
        ToggleExact(false);
        click = false;
    }
    int shown = (std::min)(n - g_top, kVisibleSuggestions);
    int under = -1;
    if (shown > 0 && g_notice.empty() && ui::CursorInUi(mx, my)) {
        int line = ui::DialogLineAt(g_dialog, shown, mx, my);
        under = line >= 0 ? g_top + line : -1;
    }
#ifdef FIND_DEV
    int g_dialogTestClicked = -1;
    if (g_dialogTest.clickLine >= 0) {
        float area[4];
        if (shown > 0 && g_notice.empty() && ui::DialogListArea(g_dialog, area))
            for (float y = area[1]; y < area[1] + area[3]; y += 1.0f)
                if (ui::DialogLineAt(g_dialog, shown, area[0] + area[2] / 2, y) == g_dialogTest.clickLine - g_top) {
                    under = g_dialogTest.clickLine;
                    click = true;
                    g_dialogTestClicked = under; // a test click has no button to let go of
                    break;
                }
        g_dialogTest.clickLine = -1;
    }
#endif
    if (under != g_hover) {
        g_hover = under;
        ShowMessage();
    }
    static int pressedLine = -1; // the line the button went down on
    bool act = false;
    if (click)
        pressedLine = under;
    else if (release) {
        act = under >= 0 && under == pressedLine;
        pressedLine = -1;
    }
#ifdef FIND_DEV
    if (click && under >= 0 && under == g_dialogTestClicked) { act = true; pressedLine = -1; }
#endif
    if (act) {
        // One click finds it (choosing first and then clicking again was one click too many).
        int index = under;
        Select(index);
        g_clickAt = now;
        std::string name = g_index.entries[g_suggested[index]].name;
        if (Search(name, true)) {
            CloseSearch(" (picked with the mouse)");
            return;
        }
        ShowMessage();
    }
}

void DrawGlows()
{
    if (g_hotPaused) { nativehighlight::Clear(); return; }
    if (g_glows.size() != g_shared.size()) // a chest reached: it stops glowing (last frame's change, shared now)
        ShareFound(false);
    if (g_glows.empty())
        return;
    ULONGLONG now = GetTickCount64();
    // Its time running out ends the search. The HUD being away doesn't: the World Map, a chest window and other menus hide it, and
    // the search goes on behind them. Leaving the base or the world ends it below.
    if (now >= g_glowsUntil) {
        EndSearch();
        return;
    }
    g_lastGlowDraw = now;
    const unsigned off = nativehighlight::DebugOff();
    if (off & nativehighlight::kOffAll) { nativehighlight::Clear(); return; } // the frame rate test: Find does nothing this while
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
        if (off & (nativehighlight::kOffCrafters | nativehighlight::kOffChests)) {
            const bool crafter = std::find(g_crafterGlows.begin(), g_crafterGlows.end(), g.chest) != g_crafterGlows.end();
            if (off & (crafter ? nativehighlight::kOffCrafters : nativehighlight::kOffChests)) { ++i; continue; }
        }
        targets.push_back({g.chest, {g.pos[0], g.pos[1], g.pos[2]}, alpha});
        ++i;
    }
    nativehighlight::SetView(ui::Client(), player); // the client whose view matrix (+0x1d20) the screen projection uses
    nativehighlight::Publish(targets.data(), static_cast<unsigned>(targets.size())); // the gold glow where it's in view
    if (nativehighlight::ThroughWalls() == 1) { // find.ini through_walls = icon: a marker over each, no 3D copies
        ui::ProjectionFrame camera;
        if (!ui::CaptureProjection(camera)) return;
        for (const auto& target : targets) {
            float pos[]{target.position[0], target.position[1] + 1.f, target.position[2]};
            float x, y, depth;
            if (!camera.Project(pos, x, y, depth)) continue;
            if (x < camera.viewport[0] || y < camera.viewport[1] ||
                x >= camera.viewport[0] + camera.viewport[2] || y >= camera.viewport[1] + camera.viewport[3]) continue;
            ui::DrawMarker(x, y, target.alpha);
        }
        return;
    }
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

// The game's compass (be34c0 Hud_CompassMarkers) shows only the waypoint, pings, players and your corpse, so the chests
// the search lights get markers of Find's own on its strip. World x is east and z north (the map mod's convention); a
// chest's bearing is atan2(dx, dz) from the player (UI client + 0x38 x, + 0x40 z), and the strip's centre is the bearing
// -yaw (UI client + 0x1da0), bearings growing to the right (W left of N). Measured 2026-11-17: yaw 1.071 with the strip
// centred at 298.4 degrees, and two chests at bearing 298.6 straight ahead. 90 degrees are 314 UI units of the strip
// (W to N at 2560 x 1600); its ends are about 78 degrees out.
constexpr float kCompassUiPerRad = 314.0f / 1.5707964f;
constexpr float kCompassHalfSpan = 1.36f;
constexpr float kCompassY = 44.0f;   // the icons' centre, UI units from the top
constexpr float kCompassIcon = 26.0f;
constexpr int kCompassMax = 24;      // the nearest; more only crowd the strip

void DrawCompass()
{
    if (g_glows.empty() || Screen() != kScreenPlaying)
        return;
    if (nativehighlight::DebugOff() & (nativehighlight::kOffCompass | nativehighlight::kOffAll))
        return;
    auto* client = static_cast<uint8_t*>(ui::Client());
    HWND window = GameWindow();
    RECT r{};
    if (!client || !window || !GetClientRect(window, &r) || r.right <= 0 || r.bottom <= 0)
        return;
    float yaw = 0, px = 0, pz = 0;
    hot::Read(client + 0x1da0, &yaw, sizeof(yaw));
    hot::Read(client + 0x38, &px, sizeof(px));
    hot::Read(client + 0x40, &pz, sizeof(pz));
    float scale = (std::max)(1920.0f / static_cast<float>(r.right), 1080.0f / static_cast<float>(r.bottom));
    float centre = static_cast<float>(r.right) * scale / 2;
    std::vector<std::pair<float, size_t>> nearest;
    for (size_t i = 0; i < g_glows.size(); ++i) {
        float dx = g_glows[i].pos[0] - px, dz = g_glows[i].pos[2] - pz;
        nearest.push_back({dx * dx + dz * dz, i});
    }
    std::sort(nearest.begin(), nearest.end());
    if (nearest.size() > static_cast<size_t>(kCompassMax))
        nearest.resize(kCompassMax);
    float pulse = 0.8f + 0.2f * std::sin(static_cast<float>(GetTickCount64() % kIconPulseMs) / kIconPulseMs * 6.2831853f);
    for (auto it = nearest.rbegin(); it != nearest.rend(); ++it) { // the nearest drawn last, on top
        const Glow& g = g_glows[it->second];
        float dx = g.pos[0] - px, dz = g.pos[2] - pz;
        if (dx * dx + dz * dz < 4.0f)
            continue; // standing at it: no direction
        float rel = std::remainder(std::atan2(dx, dz) + yaw, 6.2831853f);
        if (std::fabs(rel) > kCompassHalfSpan)
            continue;
        float edge = std::fabs(rel) / kCompassHalfSpan;
        float alpha = pulse * (edge > 0.8f ? (1.0f - edge) / 0.2f : 1.0f); // fades out at the strip's ends
        ui::DrawStorageIcon(centre + g_compassSign * rel * kCompassUiPerRad, kCompassY, kCompassIcon, kIconColour, alpha);
    }
}

void SafeDrawGlows()
{
    __try {
        DrawGlows();
        DrawCompass();
#ifdef FIND_DEV
        DrawIconPreview();
#endif
    } __except (FaultFilter(GetExceptionInformation(), "glows")) {
        EndSearch();
        Log("glows: fault, cleared");
    }
}

// find.ini debug_fps_test = 1 (HadToRegister, 2026-11-25: 57 fps fell to 12-23 with anything lit, with and without the 3D
// copies, and nothing here reproduces it). After a search the glow is held on and the test goes through the states below,
// 4 s each, three rounds; the first second of a state isn't counted. Each state's frame rate and the time Find's own code
// took per frame go to find.log, so one search and two minutes of standing still tell which part costs.
struct FpsState { const char* name; unsigned off; };
const FpsState kFpsStates[] = {
    {"everything on", 0},
    {"steady colour (no pulse)", nativehighlight::kOffPulse},
    {"no HUD outline", nativehighlight::kOffOutline},
    {"no compass markers", nativehighlight::kOffCompass},
    {"chests only (NPCs and stations not lit)", nativehighlight::kOffCrafters},
    {"NPCs and stations only (chests not lit)", nativehighlight::kOffChests},
    {"the game's own highlight colour (group 6, no gold)", nativehighlight::kOffGold},
    {"nothing lit in the 3D scene (HUD outline and compass only)", nativehighlight::kOffGroup},
    {"everything off (search still active)", nativehighlight::kOffAll},
    {"no 3D copies (as through_walls = silhouette)", nativehighlight::kOffCopies},
};
constexpr int kFpsStateCount = sizeof(kFpsStates) / sizeof(kFpsStates[0]);
constexpr ULONGLONG kFpsStateMs = 4000, kFpsSettleMs = 1000;
constexpr int kFpsRounds = 3;
struct FpsSum { double seconds, tick, hud, update, worst; long frames, hudCalls, updates, ends; };
struct FpsTest {
    ULONGLONG search = 0, started = 0, entered = 0; // the search (g_glowsStart) tested, when, and when this state began
    int state = -1, round = 0, states = 0;
    LARGE_INTEGER last{};
    FpsSum now{}, total[kFpsStateCount]{};
    unsigned outlines = 0, vertices = 0;
    long paused = 0; // frames longer than a second (a menu, a loading screen, the window in the background): not counted
    nativehighlight::DebugTimes seen{};
} g_fps;

void FpsLine(const char* label, const char* name, const FpsSum& s, double freq)
{
    if (!s.frames || s.seconds <= 0) { Log("fps test: %s%s: no frames counted", label, name); return; }
    const double perFrame = 1000.0 / freq / s.frames;
    Log("fps test: %s%s: %.1f fps (%ld frames, slowest %.0f ms); Find per frame: search and keys %.3f ms, HUD drawing %.3f ms in %.1f calls, models %.3f ms in %.1f updates of %.0f system runs",
        label, name, s.frames / s.seconds, s.frames, s.worst * 1000.0, s.tick * perFrame, s.hud * perFrame,
        static_cast<double>(s.hudCalls) / s.frames, s.update * 1000.0 / s.frames, static_cast<double>(s.updates) / s.frames,
        static_cast<double>(s.ends) / s.frames);
}

void FpsTestEnd(const char* why)
{
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    nativehighlight::SetDebugOff(0);
    InterlockedExchange64(&g_devHoldUntil, 0);
    if (g_glowsUntil) g_glowsUntil = GetTickCount64() + (std::min)(g_glowLifetimeMs, 10000ULL); // the glow ends as usual from here
    Log("fps test: %s. All rounds together:", why);
    for (int i = 0; i < g_fps.states; ++i)
        FpsLine("  ", kFpsStates[i].name, g_fps.total[i], static_cast<double>(f.QuadPart));
    g_fps.state = -1;
}

void FpsTestFrame(LARGE_INTEGER now)
{
    FpsTest& t = g_fps;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    // This frame's share of Find's own time, taken every frame so a state starts from zero.
    const LONG64 tick = InterlockedExchange64(&g_tickTicks, 0), hud = InterlockedExchange64(&g_hudTicks, 0);
    const LONG hudCalls = InterlockedExchange(&g_hudCalls, 0);
    const nativehighlight::DebugTimes times = nativehighlight::TakeDebugTimes();
    const ULONGLONG ms = GetTickCount64();
    if (t.state < 0) {
        if (g_glows.empty() || g_glowsStart == t.search || g_open) { t.last = now; return; }
        t = FpsTest{};
        t.search = g_glowsStart;
        t.started = ms;
        t.states = nativehighlight::ThroughWalls() == 0 ? kFpsStateCount : kFpsStateCount - 1; // the last state is about copies
        const ULONGLONG length = kFpsStateMs * t.states * kFpsRounds;
        InterlockedExchange64(&g_devHoldUntil, static_cast<LONG64>(ms + length + 1000)); // reaching a chest doesn't end its glow
        g_glowsUntil = (std::max)(g_glowsUntil, ms + length + 5000);
        RECT r{};
        if (HWND w = GameWindow()) GetClientRect(w, &r);
        DISPLAY_DEVICEA card{};
        card.cb = sizeof(card);
        for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &card, 0); ++i)
            if (card.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) break;
        Log("fps test: graphics card: %s", card.DeviceString[0] ? card.DeviceString : "unknown");
        Log("fps test: started, %d states of %llu s, %d rounds (%llu s): stand still and keep the lit things in view. %zu lit (%zu NPCs and stations), through_walls %d%s, window %ld x %ld",
            t.states, kFpsStateMs / 1000, kFpsRounds, length / 1000, g_glows.size(), g_crafterGlows.size(),
            nativehighlight::ThroughWalls(), g_depositGlow ? ", debug_deposit_glow" : "", r.right, r.bottom);
    }
    if (g_glows.empty()) { FpsTestEnd("the search ended before the test was through"); t.last = now; return; }
    const ULONGLONG at = ms - t.started;
    const int index = static_cast<int>(at / kFpsStateMs);
    const int state = index % t.states;
    if (index / t.states != t.round || state != t.state) {
        if (t.state >= 0) {
            char label[32];
            snprintf(label, sizeof(label), "round %d, ", t.round + 1);
            FpsLine(label, kFpsStates[t.state].name, t.now, static_cast<double>(f.QuadPart));
            Log("fps test:   %u lit in the scene, %ld copies, %u HUD outlines with %u vertices, %ld frames left out (longer than 1 s)",
                t.seen.lit, t.seen.copies, t.outlines, t.vertices, t.paused);
            FpsSum& sum = t.total[t.state];
            sum.seconds += t.now.seconds; sum.tick += t.now.tick; sum.hud += t.now.hud; sum.update += t.now.update;
            sum.worst = (std::max)(sum.worst, t.now.worst);
            sum.frames += t.now.frames; sum.hudCalls += t.now.hudCalls; sum.updates += t.now.updates; sum.ends += t.now.ends;
        }
        if (index >= t.states * kFpsRounds) { FpsTestEnd("done"); t.last = now; return; }
        t.round = index / t.states;
        t.state = state;
        t.entered = ms;
        t.now = FpsSum{};
        t.outlines = t.vertices = 0;
        t.paused = 0;
        nativehighlight::SetDebugOff(kFpsStates[state].off);
    } else if (ms - t.entered >= kFpsSettleMs) {
        const double dt = static_cast<double>(now.QuadPart - t.last.QuadPart) / f.QuadPart;
        if (dt > 1.0) { ++t.paused; t.last = now; return; }
        t.now.seconds += dt;
        t.now.worst = (std::max)(t.now.worst, dt);
        ++t.now.frames;
        t.now.tick += static_cast<double>(tick);
        t.now.hud += static_cast<double>(hud);
        t.now.hudCalls += hudCalls;
        t.now.update += times.updateSeconds;
        t.now.updates += times.updates;
        t.now.ends += times.iteratorEnds;
        t.seen = times;
        unsigned outlines, vertices;
        silhouette::LastDraw(outlines, vertices);
        t.outlines = (std::max)(t.outlines, outlines);
        t.vertices = (std::max)(t.vertices, vertices);
    }
    t.last = now;
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
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    SafeTick();
    QueryPerformanceCounter(&t1);
    InterlockedExchangeAdd64(&g_tickTicks, t1.QuadPart - t0.QuadPart);
    if (g_fpsTest) FpsTestFrame(t1);
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
// --- Find's tip in the game's hint bar, on the Skills page ---
//
// 140ce8da0 doActionHints(span{entries, count}, options) draws the bar at the bottom of a menu
// screen; doPlayStateUi calls it for the menu screens. Each 0x48-byte entry is drawn by
// 140ce85b0 doActionHint(entry, params): +0x00 u8 input action (its key glyph comes from the
// action's binding record, *(G + 0xd0) + 0x10 + 0xf26c + action * 0xc4), +0x1a bit 2 shown,
// +0x1b: use the text at +0x20. The screens call the bar from six places. On the Skills page Find adds "<Shift+F> Find" first: a copy of an
// entry the page shows, its action's record saying Find's key and button while that one entry
// draws (as Quick Deposit's tips do; common/game_hints.h). Only calls are redirected (the
// screens' calls of the bar, the bar's call of the entry), not the functions.
struct HintSpan {
    uint8_t* data;
    uint64_t count;
};
using HintBarFn = void (*)(HintSpan* span, void* options);
using HintEntryFn = uint64_t (*)(uint8_t* entry, void* params);
HintBarFn g_originalHintBar = nullptr;
// 140c3a440 doMenuButtonHints(entries, align, margins, backdrop, compact): the menus' bottom bar. An empty list gets the
// game's Back entry. Find draws it on the Reskill page, which has no bar of its own, and HintBarHook adds Find's tip.
using ButtonHintsFn = void (*)(HintSpan* entries, uint16_t align, const float* margins, char backdrop, char compact);
ButtonHintsFn g_buttonHints = nullptr;
bool g_drawingOwnBar = false;      // Find's own bar on the Reskill page is drawing
HintEntryFn g_originalHintEntry = nullptr;
constexpr size_t kHintEntry = 0x48;
constexpr int kMaxHintEntries = 16;
constexpr size_t kUiBindings = 0x10 + 0xf26c;
alignas(16) uint8_t g_hintEntries[(kMaxHintEntries + 1) * kHintEntry];
uint8_t* g_findTip = nullptr; // Find's entry while the bar draws
ui::Text g_findTipText{};

uint8_t* ActionRecord(uint8_t action)
{
    auto* g = static_cast<uint8_t*>(ui::G());
    uint8_t* u = g ? *reinterpret_cast<uint8_t**>(g + 0xd0) : nullptr;
    return u && *reinterpret_cast<void**>(u) ? u + kUiBindings + action * greeneye::hints::kRecordSize : nullptr;
}

void HintBarBody(HintSpan* span, void* options)
{
    g_findTip = nullptr;
    const bool pad = greeneye::hints::PadInUse();
#ifdef FIND_DEV
    if (g_devNoTip) {
        g_originalHintBar(span, options);
        return;
    }
#endif
    // The page draws two bars in one place on alternate frames: its own (Zoom, Close) and one
    // with only Close. Their entries share the bar's widgets, so a tip in only one of them
    // flickers and mixes the other's up: both frames get the page's own bar, with the tip.
    static uint8_t page[kMaxHintEntries * kHintEntry]; // the page's own bar, as it last drew
    static uint64_t pageCount = 0;
    static ULONGLONG pageAt = 0;
    const ULONGLONG now = GetTickCount64();
    if (g_open || !skills::IsScreen(Screen()) || !span || !span->data || !span->count || span->count >= kMaxHintEntries ||
        !greeneye::hints::ShowsOn(g_keys.open, pad)) {
        g_originalHintBar(span, options);
        return;
    }
    if (span->count >= 2) {
        memcpy(page, span->data, span->count * kHintEntry);
        pageCount = span->count;
        pageAt = now;
    }
    const uint8_t* entries = span->data;
    uint64_t count = span->count;
    if (count < 2 && pageCount && now - pageAt < 500) { // the Close-only frame
        entries = page;
        count = pageCount;
    }
    // A shown, enabled entry with a keyboard key to copy (the page's Close; its Zoom is the
    // mouse wheel, drawn differently).
    int model = -1;
    for (uint64_t i = 0; i < count && model < 0; ++i) {
        const uint8_t* record = ActionRecord(entries[i * kHintEntry]);
        // On Reskill or the menu's Skills tab the only entry is the bar's own Back (another kind of key record): Find's key replaces it anyway.
        if ((entries[i * kHintEntry + 0x1a] & 7) == 7 && record && (record[0] == 1 || skills::NoOwnHintBar(Screen())))
            model = static_cast<int>(i);
    }
    if (model < 0) {
#ifdef FIND_DEV
        if (static int logged = 0; logged++ < 3 && count)
            DevLog("hint bar: no entry to copy on screen %d: entry 0 action %u flags %02x record kind %d", Screen(), entries[0],
                entries[0x1a], ActionRecord(entries[0]) ? ActionRecord(entries[0])[0] : -1);
#endif
        g_originalHintBar(span, options);
        return;
    }
    // Last, after Close.
    memcpy(g_hintEntries, entries, count * kHintEntry);
    uint8_t* tip = g_hintEntries + count * kHintEntry;
    memcpy(tip, entries + model * kHintEntry, kHintEntry);
    g_findTipText = ui::MakeText(strings::Get(strings::Title));
    memcpy(tip + 0x20, &g_findTipText, sizeof(g_findTipText));
    tip[0x1b] = 1;
    HintSpan mine{g_hintEntries, count + 1};
    g_findTip = tip;
    g_originalHintBar(&mine, options);
    g_findTip = nullptr;
}

void DrawReskillHints()
{
    if (!g_buttonHints || !g_originalHintBar || g_open || !skills::NoOwnHintBar(Screen()))
        return;
    // Inside a layer of Find's own: called outside one, the game's widget code faulted.
    ui::ProjectionFrame frame;
    if (!ui::CaptureProjection(frame))
        return;
    if (!skills::InOwnLayer("findReskillHints", [](void*) {
            __try {
                HintSpan none{nullptr, 0};
                alignas(16) const float margins[4] = {0.0f, 10.0f, 0.0f, 0.0f}; // as the inventory's bottom bar
                g_drawingOwnBar = true;
                g_buttonHints(&none, 0x303, margins, 0, 0); // bottom right
                g_drawingOwnBar = false;
            } __except (FaultFilter(GetExceptionInformation(), "reskill hint bar")) {
                g_drawingOwnBar = false;
                g_buttonHints = nullptr;
                Log("hint bar: fault drawing the Reskill page's bar, off until the game restarts");
            }
        }, nullptr, frame.viewport[2], frame.viewport[3]))
        g_buttonHints = nullptr;
}

void HintBarHook(HintSpan* span, void* options)
{
    hot::Lease lease;
    if (!lease || g_hotPaused)
        return g_originalHintBar(span, options);
    HintBarBody(span, options);
    if (skills::IsScreen(Screen()))
        skills::DrawInPage(); // the pulse inside the page, under its node card
}

uint64_t HintEntryHook(uint8_t* entry, void* params)
{
    hot::Lease lease;
    uint8_t* record = lease && entry && entry == g_findTip ? ActionRecord(entry[0]) : nullptr;
    if (!record)
        return g_originalHintEntry(entry, params);
    // The copied action shows Find's key and button for this one entry.
    uint8_t saved[greeneye::hints::kRecordSize];
    memcpy(saved, record, sizeof(saved));
    greeneye::hints::WriteRecord(record, g_keys.open);
    uint64_t result = g_originalHintEntry(entry, params);
    memcpy(record, saved, sizeof(saved));
    return result;
}

// Redirects the screens' calls of the hint bar and the bar's call of an entry. Optional.
void HookHintBar()
{
    uint8_t* bar = FindUnique("48 89 5C 24 20 55 56 57 41 54 41 55 48 8D 6C 24 C9 48 81 EC B0 00 00 00");
    uint8_t* entry = FindUnique("48 8B C4 55 56 57 41 56 48 8D A8 D8 FD FF FF 48 81 EC 08 03 00 00 F6 41 1A 04");
    uint8_t* calls[128];
    int barCalls = 0, entryCalls = 0;
    if (bar && entry) {
        int n = CallsTo(entry, calls, 128);
        for (int i = 0; i < n; ++i)
            if (calls[i] > bar && calls[i] < bar + 0x300)
                if (void* on = RedirectCall(calls[i], reinterpret_cast<void*>(&HintEntryHook))) {
                    g_originalHintEntry = reinterpret_cast<HintEntryFn>(on);
                    ++entryCalls;
                }
        if (g_originalHintEntry) {
            // Every screen's call of the bar (six in this build); Find only adds to the Skills page's.
            n = CallsTo(bar, calls, 128);
            for (int i = 0; i < n && n <= 16; ++i)
                    if (void* on = RedirectCall(calls[i], reinterpret_cast<void*>(&HintBarHook))) {
                        g_originalHintBar = reinterpret_cast<HintBarFn>(on);
                        ++barCalls;
                    }
        }
    }
    if (barCalls >= 1)
        g_buttonHints = reinterpret_cast<ButtonHintsFn>(FindUnique(
            "48 8B C4 48 89 58 10 48 89 70 18 55 57 41 56 48 8D A8 A8 FA FF FF 48 81 EC 40 06 00 00 44 0F 29"));
    skills::UsePageDrawing(barCalls >= 1);
    if (barCalls >= 1 && entryCalls == 1)
        Log("hint bar: Find's tip on the Skills page (%d screen calls)", barCalls);
    else
        Log("hint bar: not as expected (bar %p %d, entry %p %d) - no tip on the Skills page", bar, barCalls, entry, entryCalls);
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
    LARGE_INTEGER h0, h1;
    QueryPerformanceCounter(&h0);
    SafeDrawGlows();
    QueryPerformanceCounter(&h1);
    InterlockedExchangeAdd64(&g_hudTicks, h1.QuadPart - h0.QuadPart);
    InterlockedIncrement(&g_hudCalls);
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
    HookKeys(false); // the hook procedure is in this DLL
    nativehighlight::Clear();
    ui::LightIcons(0, 0);
    return true;
}
bool HotReady() {
    nativehighlight::Status s{sizeof(s)};
    return nativehighlight::GetStatus(&s) && s.applied == 0;
}
void HotResume() { g_hotPaused = false; }

#ifdef FIND_DEV
// "state" | "open" | "text <text>" | "click <line>" | "enter" | "cancel". Writes the state as JSON;
// requests apply on the next frame. Returns 0 if the command was not understood.
int DialogTest(const char* cmd, char* out, int size)
{
    int ok = 1;
    if (!strcmp(cmd, "open")) {
        g_dialogTest.open = true;
    } else if (!strncmp(cmd, "text ", 5)) {
        g_dialogTest.text = cmd + 5;
        g_dialogTest.setText = true;
    } else if (!strncmp(cmd, "click ", 6)) {
        g_dialogTest.clickLine = atoi(cmd + 6);
    } else if (!strcmp(cmd, "enter")) {
        g_dialogTest.enter = true;
    } else if (!strcmp(cmd, "cancel")) {
        g_dialogTest.cancel = true;
    } else if (strcmp(cmd, "state")) {
        ok = 0;
    }
    size_t len = 0;
    const char* raw = g_open && g_dialog ? ui::DialogText(g_dialog, &len) : nullptr;
    std::string text(raw ? raw : "", raw ? len : 0), safe;
    for (char c : text)
        if (c != '"' && c != '\\' && static_cast<unsigned char>(c) >= 0x20)
            safe += c;
    float area[4] = {};
    const bool haveArea = g_open && g_dialog && ui::DialogListArea(g_dialog, area);
    _snprintf_s(out, size, _TRUNCATE,
                "{\"open\":%s,\"screen\":%d,\"typing\":%d,\"text\":\"%s\",\"suggestions\":%d,\"top\":%d,\"selected\":%d,\"hover\":%d,"
                "\"listArea\":%s,\"area\":[%.1f,%.1f,%.1f,%.1f]}",
                g_open ? "true" : "false", g_dialogTest.screen, g_dialogTest.typing, safe.c_str(), static_cast<int>(g_suggested.size()), g_top,
                g_selected, g_hover, haveArea ? "true" : "false", area[0], area[1], area[2], area[3]);
    return ok;
}
#endif

bool QueueSearch(const char* text)
{
    if (!text || strlen(text) >= sizeof(g_request)) return false;
#ifdef FIND_DEV
    if (!strcmp(text,"@dev-profile-start")) { InterlockedExchange(&g_profileCommand,1); return true; }
    if (!strcmp(text,"@dev-profile-stop")) { InterlockedExchange(&g_profileCommand,2); return true; }
    if (!strcmp(text,"@dev-walls-glow")) { nativehighlight::SetThroughWalls(0); return true; }
    if (!strcmp(text,"@dev-walls-icon")) { nativehighlight::SetThroughWalls(1); return true; }
    if (!strcmp(text,"@dev-walls-silhouette")) { nativehighlight::SetThroughWalls(2); return true; }
    if (!strcmp(text,"@dev-fps-on")) { nativehighlight::SetDevSwitch(3,true); return true; }
    if (!strcmp(text,"@dev-fps-off")) { nativehighlight::SetDevSwitch(3,false); return true; }
    if (!strcmp(text,"@dev-pulse-off")) { nativehighlight::SetDevSwitch(1,true); return true; }
    if (!strcmp(text,"@dev-pulse-on")) { nativehighlight::SetDevSwitch(1,false); return true; }
    if (!strcmp(text,"@dev-sdf-keep")) { nativehighlight::SetDevSwitch(5,true); return true; }
    if (!strcmp(text,"@dev-sdf-drop")) { nativehighlight::SetDevSwitch(5,false); return true; }
    if (!strcmp(text,"@dev-cycle-on")) { nativehighlight::SetDevSwitch(7,true); return true; }
    if (!strcmp(text,"@dev-cycle-off")) { nativehighlight::SetDevSwitch(7,false); return true; }
    if (!strcmp(text,"@dev-jitter-on")) { nativehighlight::SetDevSwitch(6,true); return true; }
    if (!strcmp(text,"@dev-jitter-off")) { nativehighlight::SetDevSwitch(6,false); return true; }
    if (!strcmp(text,"@dev-move-always")) { nativehighlight::SetDevSwitch(4,true); return true; }
    if (!strcmp(text,"@dev-move-changed")) { nativehighlight::SetDevSwitch(4,false); return true; }
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
    g_keys = config.keys;
    g_exact = config.exactMatch;
    reflect::Init();
    ReadItemLayout();
    ui::ReadAssetLayout();
    inv::ReadLayout();
    recipes::ReadLayout();
    nativehighlight::SetThroughWalls(config.throughWalls);
    nativehighlight::SetDepositStyle(config.depositGlow);
    g_fpsTest = config.fpsTest;
    g_depositGlow = config.depositGlow;
    if (g_fpsTest)
        Log("debug_fps_test is on: after each search Find measures the frame rate for about 2 minutes, switching parts of the glow off in turn (the glow stays on meanwhile)");
    greeneye::hints::Init(hot::host);
    g_glowLifetimeMs = static_cast<ULONGLONG>(config.glow.glowSeconds * 1000.0f);
    g_arrivedMeters = config.glow.nearMeters;
    g_afterArrivalMs = static_cast<ULONGLONG>(config.glow.nearSeconds * 1000.0f);
    g_nativeReady = nativehighlight::Init();
    skills::Init(); // optional: Find on the Skills page
    {
        const reflect::Stats st = reflect::GetStats();
        Log("layout: %d game field offsets read from the game itself (%d moved since 2026-11-04, %d not found)", st.found, st.moved, st.missing);
    }
    uint8_t* playState = FindUnique("4C 8B DC 55 49 8D AB E8 FD FF FF 48 81 EC 10 03 00 00");
    if (!playState) {
        // Auto Loot (18-byte jump) or Creative Mode (safetyhook) may have hooked it already:
        // find the code after the first 18 bytes, which neither overwrites.
        uint8_t* after = FindUnique("48 8B 05 ?? ?? ?? ?? 48 8B 88 D0 00 00 00 48 8B 01 48 85 C0 0F 84 ?? ?? ?? ?? 4D 89 73 C8 4C 8B 70 38");
        if (after && IsHookPatch(after - 18))
            playState = after - 18;
    }
    uint8_t* spatial = FindUnique("48 89 54 24 10 55 57 41 56 48 8D AC 24 00 FE FF FF");
    uint8_t* loop = FindUnique("48 81 EC 98 00 00 00 48 8D 81 78 4F 03 00");
    if (!playState || !spatial || !loop) {
        Log("signatures: play state %p, markers %p, command loop %p", playState, spatial, loop);
        return false;
    }
    loop -= 13; // push rbx/rbp/rsi/rdi/r12/r13/r14/r15
    // UiCommandLoop: Quick Deposit's 12-byte jump + nop may be there already.
    g_originalLoop = reinterpret_cast<CommandLoopFn>(InstallHookChained(loop, reinterpret_cast<void*>(&CommandLoopHook), 13));
    // doPlayStateUi: mov r11, rsp; push rbp; lea rbp, [r11-218h]; sub rsp, 310h (18 bytes).
    g_originalPlayState = reinterpret_cast<PlayStateFn>(InstallHookChained(playState, reinterpret_cast<void*>(&PlayStateHook), 18));
    if (hot::host && !greeneye::input::InstallPadFilter(hot::host, &FindPadFilter))
        Log("XInput is not as expected; the game also sees the controller's search buttons");
    // markers: mov [rsp+10h], rdx; push rbp; push rdi; push r14; lea rbp, [rsp-200h] (17 bytes).
    g_originalSpatial = reinterpret_cast<SpatialMarkersFn>(InstallHookChained(spatial, reinterpret_cast<void*>(&SpatialMarkersHook), 17));
    if (!g_originalPlayState || !g_originalSpatial || !g_originalLoop) {
        Log("hooks: could not patch the game");
        return false;
    }
    HookHintBar();
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

int FoundTargets(float* xyz, uint32_t* entities, int max, uint32_t* generation)
{
    AcquireSRWLockShared(&g_shareLock);
    int n = static_cast<int>(g_shared.size());
    for (int i = 0; i < n && i < max; ++i) {
        if (xyz)
            memcpy(xyz + 3 * i, g_shared[i].pos, sizeof(g_shared[i].pos));
        if (entities)
            entities[i] = g_shared[i].chest;
    }
    if (generation)
        *generation = g_shareGeneration;
    ReleaseSRWLockShared(&g_shareLock);
    return n;
}

} // namespace find

#ifdef FIND_DEV
// Dev bridge submits searches without opening a dialog, sending keys, or changing focus.
extern "C" __declspec(dllexport) bool FindQueueSearch(const char* text)
{
    return find::QueueSearch(text);
}

extern "C" __declspec(dllexport) int FindDialogTest(const char* cmd, char* out, int size)
{
    return find::DialogTest(cmd, out, size);
}

extern "C" __declspec(dllexport) bool FindNativeStatus(nativehighlight::Status* out)
{
    return nativehighlight::GetStatus(out);
}
#endif
