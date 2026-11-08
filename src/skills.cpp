#include "skills.h"
// Find on the Skills page (build of 2026-11-04, from the exe's reflection tables and the skill
// tree screen FUN_140cba6f0, "skillTreeInternalRoot"):
//
// The screen keeps two UI states in the UI system's state map (any widget + 0x48: buckets at
// +0x1d8, mask at +0x1f8; entry {u64 widget, u64 hash, next, prev, record}, record header 0x60
// bytes: +0x38 hash, +0x48 size, then the data; 1411a06e0 looks them up):
//  - the skill tree, hash 0xae30eeb7033a146e, 0x5e00 bytes: +0x40 the canvas below, node entries
//    0x50 bytes each from +0x78 (u16 count at +0x5078): +0x00 node id, +0x08 {x, y} the node's
//    centre in the scaled canvas (zoom * position), +0x48 the node's record (0x44 bytes, made
//    by 140318540 from the keen::SkillTreeNode: +0x00 id, +0x08 name LocaTagId, +0x0c
//    description LocaTagId, +0x14 {x, y} uiPosition).
//  - the zoomable canvas, hash 0x3fada0ae386b6a51, 0x270 bytes: +0x08 start of a glide (UI time,
//    ns), +0x10 zoom, +0x20 centre (canvas units), +0x30 the same, where input moves it,
//    +0x18c zoom pivot, +0x1a8 where the glide starts, +0x1b0 the centre drawn, +0x1e8 the
//    view's size, +0x1f0 its middle, +0x1f8 zoom * centre drawn. The game glides from +0x1a8 to
//    the centre within 0.65 s of +0x08, so writing the centre and those two centres a node.
//  - A node's place on screen: view rect origin + middle + pivot - (zoom * centre) + entry +0x08.
//
// A LocaTagId is the text's 32-bit key, as the text lookup takes it.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <type_traits>
#include "log.h"
#include "scan.h"
#include "ui.h"

namespace skills {
namespace {

using UiCtxFn = void* (*)();
using BeginWidgetFn = void (*)(void* widget, uint64_t parent);
using EndWidgetFn = void (*)(void* widget);
using SetSizeFn = void (*)(void* handle, uint64_t packedSize);
using ImageFn = uint32_t* (*)(void* ctx, void* tmp, const void* ref);
using DrawImageFn = void (*)(void* widget, const float* rect, const uint32_t* image, const uint64_t* colour);
using LocaTextFn = ui::Text* (*)(ui::Text* out, uint32_t key);
using BeginLayerFn = void* (*)(void* layer, const ui::Text* name, uint8_t fullScreen, uint32_t order, int32_t a, uint32_t b);
using EndLayerFn = void (*)(void* layer);
using NamedWidgetFn = void (*)(void* widget, const ui::Text* name);
using PushDepthFn = void (*)(void* scope, uint8_t depth);
using PopDepthFn = void (*)(void* scope);

UiCtxFn g_uiCtx = nullptr;
BeginWidgetFn g_begin = nullptr;
EndWidgetFn g_end = nullptr;
SetSizeFn g_setSize = nullptr;
ImageFn g_image = nullptr;
DrawImageFn g_drawImage = nullptr;
LocaTextFn g_locaText = nullptr;
BeginLayerFn g_beginLayer = nullptr;
EndLayerFn g_endLayer = nullptr;
NamedWidgetFn g_named = nullptr;
PushDepthFn g_pushDepth = nullptr;
PopDepthFn g_popDepth = nullptr;
// The skill tree screen draws its parts at depths within its layer: 0x80 canvas, 0x81 links,
// 0x87 nodes, 0x88 the card of the node under the cursor (FUN_140cb9730 "chromeLayer").
constexpr uint8_t kNodeDepth = 0x87;
bool g_ready = false;

constexpr uint64_t kTreeHash = 0xae30eeb7033a146eull;
constexpr uint32_t kTreeSize = 0x5e00;
constexpr uint64_t kCanvasHash = 0x3fada0ae386b6a51ull;
constexpr uint32_t kCanvasSize = 0x270;
constexpr size_t kWidgetSystem = 0x48;    // any widget: the UI system
constexpr size_t kStateBuckets = 0x1d8;   // UI system: state map buckets
constexpr size_t kStateMask = 0x1f8;
constexpr size_t kRecordHash = 0x38, kRecordSize = 0x48, kRecordData = 0x60;
constexpr size_t kTreeCanvas = 0x40;
constexpr size_t kTreeEntries = 0x78, kEntrySize = 0x50, kTreeCount = 0x5078;
constexpr size_t kEntryScaled = 0x08, kEntryRecord = 0x48;
constexpr size_t kRecordName = 0x08, kRecordDesc = 0x0c, kRecordPos = 0x14; // the node's record
constexpr size_t kCanvasGlideStart = 0x08, kCanvasZoom = 0x10, kCanvasCentre = 0x20, kCanvasTarget = 0x30;
constexpr size_t kCanvasWidget = 0x18, kCanvasPivot = 0x18c, kCanvasFrom = 0x1a8, kCanvasShown = 0x1b0;
constexpr size_t kCanvasSizeXY = 0x1e8, kCanvasMiddle = 0x1f0, kCanvasScaledShown = 0x1f8;
constexpr size_t kWidgetRect = 0x114;
constexpr size_t kUiTime = 0x10;           // G: +0x10 -> +0x08 UI time (ns)
// Assets (keen::FbUiBundle): menu (0xea10) + skilltree (0x20) = keen::FbUiSkillTree.
constexpr size_t kSkillTree = 0xea10 + 0x20;
constexpr size_t kNodeHighlight = kSkillTree + 0x18;           // UiTexture nodeHighlight
// nodes (keen::FbUiSkillNodes): each type's FbUiSkillNode, whose nominalWidth comes first, by
// keen::SkillNodeType (record + 0x04): Root, Attribute, ActiveSkill, PassiveSkill.
constexpr size_t kNodeWidths[4] = {kSkillTree + 0xa8 + 0x08, kSkillTree + 0xa8 + 0x6c, kSkillTree + 0xa8 + 0x134,
                                   kSkillTree + 0xa8 + 0xd0};
constexpr size_t kRecordType = 0x04;
// keen::FbUiSkillTree + 0x78 classHints: BlobArray<UiSkillTreeClassHint> (0x24 each: +0x00 label
// UiLocaTagReference, +0x10 isPrimary, +0x14 colour, +0x1c directionDegrees, +0x20 distanceFromRing).
constexpr size_t kClassHints = 0x78, kHintSize = 0x24, kHintDegrees = 0x1c;
constexpr float kBarBottom = 100.0f; // the menu bar over the tree (UI units): no pulse under it
constexpr size_t kRingGlow = 0xd3d0 + 0x30;                    // primitives: ringSoftGlow
constexpr uint32_t kGold = 0xFF3CC8FF;                         // 0xAABBGGRR, as the chest pulse

struct Widget {
    alignas(16) uint8_t raw[0x80] = {};
    uint8_t* Handle() { return *reinterpret_cast<uint8_t**>(raw + 8); }
};

uint64_t Pack(float x, float y)
{
    uint64_t v;
    float xy[2] = {x, y};
    memcpy(&v, xy, 8);
    return v;
}

template <class T> T ReadAt(const void* p, size_t off)
{
    T v;
    memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof(T));
    return v;
}

uint8_t* Assets()
{
    auto* g = static_cast<uint8_t*>(ui::G());
    return g ? *reinterpret_cast<uint8_t**>(g + 200) : nullptr;
}

// --- the screen's UI states ---

struct LayerScope {
    alignas(16) uint8_t layer[0x400] = {};
    Widget root;
    explicit LayerScope(const ui::Text& name)
    {
        g_beginLayer(layer, &name, 1, 0x16, -1, 0);
        g_named(root.raw, &name);
    }
    ~LayerScope()
    {
        g_end(root.raw);
        g_endLayer(layer);
    }
};

uint8_t* g_system = nullptr;

bool LooksLikeSystem(uint8_t* sys)
{
    __try {
        uint32_t mask = *reinterpret_cast<uint32_t*>(sys + kStateMask);
        return mask && !(mask & (mask + 1)) && mask <= (1u << 22) && *reinterpret_cast<void**>(sys + kStateBuckets);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The UI system that keeps the states, read from a widget of our own (an empty layer).
uint8_t* System()
{
    if (g_system)
        return g_system;
    uint8_t* sys = nullptr;
    {
        static const ui::Text name = ui::MakeText("findSkillProbe");
        LayerScope probe(name);
        if (probe.root.Handle())
            sys = *reinterpret_cast<uint8_t**>(probe.root.Handle() + kWidgetSystem);
    }
    if (sys && LooksLikeSystem(sys))
        g_system = sys;
    Log("skills: UI state map %p%s", sys, g_system ? "" : " (not as expected)");
    return g_system;
}

uint8_t* FindState(uint64_t hash, uint32_t size)
{
    uint8_t* sys = System();
    if (!sys)
        return nullptr;
    __try {
        uint32_t mask = *reinterpret_cast<uint32_t*>(sys + kStateMask);
        auto** buckets = *reinterpret_cast<uint8_t***>(sys + kStateBuckets);
        if (!buckets || !mask || (mask & (mask + 1)) || mask > (1u << 22))
            return nullptr;
        uint8_t* best = nullptr;
        for (uint32_t b = 0; b <= mask; ++b) {
            int guard = 0;
            for (uint8_t* e = buckets[b]; e && guard < 4096; e = *reinterpret_cast<uint8_t**>(e + 0x10), ++guard) {
                if (*reinterpret_cast<uint64_t*>(e + 8) != hash)
                    continue;
                uint8_t* rec = *reinterpret_cast<uint8_t**>(e + 0x20);
                if (rec && *reinterpret_cast<uint64_t*>(rec + kRecordHash) == hash &&
                    *reinterpret_cast<uint32_t*>(rec + kRecordSize) == size)
                    best = rec + kRecordData;
            }
        }
        return best;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool StillState(const uint8_t* data, uint64_t hash, uint32_t size)
{
    __try {
        return data && ReadAt<uint64_t>(data - kRecordData, kRecordHash) == hash &&
               ReadAt<uint32_t>(data - kRecordData, kRecordSize) == size;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint8_t* g_tree = nullptr;

// The open skill tree's state (its canvas at +0x40), or null.
uint8_t* Tree()
{
    if (!StillState(g_tree, kTreeHash, kTreeSize))
        g_tree = FindState(kTreeHash, kTreeSize);
    return g_tree;
}

uint8_t* Canvas(uint8_t* tree)
{
    __try {
        uint8_t* c = *reinterpret_cast<uint8_t**>(tree + kTreeCanvas);
        return StillState(c, kCanvasHash, kCanvasSize) ? c : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool CallLoca(ui::Text* t, uint32_t key)
{
    __try {
        g_locaText(t, key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string LocaText(uint32_t key)
{
    if (!key || !g_locaText)
        return {};
    ui::Text t = ui::MakeText("");
    if (!CallLoca(&t, key))
        return {};
    return std::string(t.utf8 ? t.utf8 : "", t.utf8 ? t.len : 0);
}

struct NodeText {
    uint32_t id, name, desc;
};

// The nodes of the open tree with their texts' keys (0x100 at most).
int Nodes(uint8_t* tree, NodeText* out, int max)
{
    __try {
        int n = ReadAt<uint16_t>(tree, kTreeCount);
        n = (std::min)(n, max);
        int k = 0;
        for (int i = 0; i < n; ++i) {
            uint8_t* e = tree + kTreeEntries + i * kEntrySize;
            uint8_t* rec = ReadAt<uint8_t*>(e, kEntryRecord);
            if (rec)
                out[k++] = {ReadAt<uint32_t>(e, 0), ReadAt<uint32_t>(rec, kRecordName), ReadAt<uint32_t>(rec, kRecordDesc)};
        }
        return k;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Game text without its markup: <b>..</b>, <icon ...>, {value}-style fields become spaces.
std::string Plain(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    int depth = 0;
    char close = 0;
    for (char c : s) {
        if (!depth && (c == '<' || c == '{')) {
            depth = 1;
            close = c == '<' ? '>' : '}';
            continue;
        }
        if (depth) {
            if (c == close)
                depth = 0;
            continue;
        }
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' '))
            continue;
        out += c;
    }
    // Values the text lookup can't fill come out as "[Missing ...]": not words of the skill.
    for (size_t at = out.find("[Missing"); at != std::string::npos; at = out.find("[Missing", at)) {
        size_t end = out.find(']', at);
        out.erase(at, end == std::string::npos ? std::string::npos : end + 1 - at);
        if (at > 0 && at < out.size() && out[at - 1] == ' ' && out[at] == ' ')
            out.erase(at, 1);
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// --- the pulse ---

std::vector<uint32_t> g_lit;
ULONGLONG g_litStart = 0, g_litUntil = 0;
uint32_t g_centreOn = 0;   // a node to glide to on the next frame of the page
bool g_centrePoint = false; // or this point (a category's middle)
float g_centreAt[2] = {};
ULONGLONG g_centreUntil = 0;
bool g_drawFault = false;

ULONGLONG UiTimeNs(bool& ok)
{
    ok = false;
    auto* g = static_cast<uint8_t*>(ui::G());
    __try {
        uint8_t* t = g ? *reinterpret_cast<uint8_t**>(g + kUiTime) : nullptr;
        if (!t)
            return 0;
        ok = true;
        return *reinterpret_cast<ULONGLONG*>(t + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Glides the view to the node (the game's own 0.65 s glide). True when done.
// Glides the view to a point of the canvas (the game's own 0.65 s glide). True when done.
bool GlideTo(uint8_t* canvas, float x, float y)
{
    bool timeOk;
    ULONGLONG now = UiTimeNs(timeOk);
    if (!timeOk || !std::isfinite(x) || !std::isfinite(y))
        return false;
    float shown[2];
    memcpy(shown, canvas + kCanvasShown, 8);
    memcpy(canvas + kCanvasFrom, shown, 8);
    float xy[2] = {x, y};
    memcpy(canvas + kCanvasCentre, xy, 8);
    memcpy(canvas + kCanvasTarget, xy, 8);
    *reinterpret_cast<ULONGLONG*>(canvas + kCanvasGlideStart) = now;
    Log("skills: centring on %.1f %.1f (from %.1f %.1f)", x, y, shown[0], shown[1]);
    return true;
}

bool Centre(uint8_t* tree, uint8_t* canvas, uint32_t node)
{
    uint16_t n = ReadAt<uint16_t>(tree, kTreeCount);
    for (uint16_t i = 0; i < n && i < 0x100; ++i) {
        uint8_t* e = tree + kTreeEntries + i * kEntrySize;
        if (ReadAt<uint32_t>(e, 0) != node)
            continue;
        uint8_t* rec = ReadAt<uint8_t*>(e, kEntryRecord);
        if (!rec)
            return false;
        return GlideTo(canvas, ReadAt<float>(rec, kRecordPos), ReadAt<float>(rec, kRecordPos + 4));
    }
    return false;
}

struct Spot {
    float x, y, size;
};

// Screen spots (UI units) of the lit nodes, in the view's coordinates.
int Spots(uint8_t* tree, uint8_t* canvas, const float origin[2], Spot* out, int max)
{
    float middle[2], pivot[2], scaled[2];
    memcpy(middle, canvas + kCanvasMiddle, 8);
    memcpy(pivot, canvas + kCanvasPivot, 8);
    memcpy(scaled, canvas + kCanvasScaledShown, 8);
    int k = 0;
    uint16_t n = ReadAt<uint16_t>(tree, kTreeCount);
    for (uint16_t i = 0; i < n && i < 0x100 && k < max; ++i) {
        uint8_t* e = tree + kTreeEntries + i * kEntrySize;
        uint32_t id = ReadAt<uint32_t>(e, 0);
        if (std::find(g_lit.begin(), g_lit.end(), id) == g_lit.end())
            continue;
        // The record's uiPosition times the zoom (the entry's own scaled copy lags behind).
        uint8_t* rec = ReadAt<uint8_t*>(e, kEntryRecord);
        if (!rec)
            continue;
        float zoom = ReadAt<float>(canvas, kCanvasZoom);
        if (!(zoom > 0.0f && zoom < 100.0f))
            continue;
        float at[2] = {ReadAt<float>(rec, kRecordPos) * zoom, ReadAt<float>(rec, kRecordPos + 4) * zoom};
        // The canvas draws a node at its type's nominalWidth (canvas units), scaled by the zoom.
        uint32_t type = ReadAt<uint32_t>(rec, kRecordType);
        float width = ReadAt<float>(Assets(), kNodeWidths[type < 4 ? type : 2]);
        if (!(width > 1.0f && width < 1000.0f))
            width = 100.0f;
        Spot spot{origin[0] + middle[0] + pivot[0] - scaled[0] + at[0], origin[1] + middle[1] + pivot[1] - scaled[1] + at[1],
                  width * zoom};
        if (spot.y - spot.size * 0.5f < kBarBottom)
            continue;
        out[k++] = spot;
    }
    return k;
}

void DrawImageAt(Widget& parent, float x, float y, float size, size_t asset, uint32_t colour, float alpha)
{
    (void)parent;
    uint8_t* assets = Assets();
    Widget w;
    g_begin(w.raw, 0);
    g_setSize(w.Handle(), Pack(size, size));
    *reinterpret_cast<uint64_t*>(w.Handle() + 0x70) = Pack(x - size * 0.5f, y - size * 0.5f);
    float rect[4];
    memcpy(rect, w.Handle() + kWidgetRect, sizeof(rect));
    struct {
        uint32_t rgba;
        float brightness;
    } c{(colour & 0x00ffffffu) | (static_cast<uint32_t>((colour >> 24) * (std::max)(0.0f, (std::min)(1.0f, alpha)) + 0.5f) << 24), 1.0f};
    uint64_t packed;
    memcpy(&packed, &c, 8);
    alignas(16) uint8_t tmp[16];
    g_drawImage(w.raw, rect, g_image(g_uiCtx(), tmp, assets + asset), &packed);
    g_end(w.raw);
}

struct Frame {
    Spot spots[64];
    int count = 0;
    float origin[2] = {}; // the view's top left
};

// Everything the pulse needs from the game's memory, read before any layer opens (a fault
// inside an open layer would leave it open: the game dies a few frames later).
bool ReadFrame(uint8_t* tree, uint8_t* canvas, Frame& f)
{
    __try {
        if (g_centreOn) {
            if (Centre(tree, canvas, g_centreOn) || GetTickCount64() > g_centreUntil)
                g_centreOn = 0;
        } else if (g_centrePoint) {
            if (GlideTo(canvas, g_centreAt[0], g_centreAt[1]) || GetTickCount64() > g_centreUntil)
                g_centrePoint = false;
        }
        uint8_t* view = ReadAt<uint8_t*>(canvas, kCanvasWidget);
        if (view)
            memcpy(f.origin, view + kWidgetRect, 8);
        float zero[2] = {};
        f.count = Spots(tree, canvas, zero, f.spots, 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void DrawSpots(const Frame& f, Widget& root)
{
    ULONGLONG now = GetTickCount64();
    float t = static_cast<float>(now - g_litStart) / 1000.0f;
    float fade = 1.0f;
    if (g_litUntil > now && g_litUntil - now < 600)
        fade = static_cast<float>(g_litUntil - now) / 600.0f;
    float pulse = 0.5f + 0.5f * std::sin(t * 6.2831853f / 1.2f); // as the chest pulse (1.2 s)
    float at[4] = {};
    if (root.Handle())
        memcpy(at, root.Handle() + kWidgetRect, sizeof(at));
    // Children are placed relative to the root.
    float dx = f.origin[0] - at[0], dy = f.origin[1] - at[1];
    for (int i = 0; i < f.count; ++i) {
        float x = f.spots[i].x + dx, y = f.spots[i].y + dy;
        // Drawn over the node: a halo ring and the tree's own highlight ring, so its icon stays visible.
        float size = f.spots[i].size;
        DrawImageAt(root, x, y, size * (1.8f + 0.3f * pulse), kRingGlow, kGold, fade * (0.55f + 0.45f * pulse));
        DrawImageAt(root, x, y, size * (1.25f + 0.1f * pulse), kNodeHighlight, kGold, fade * (0.3f + 0.3f * pulse));
    }
}

// On a layer of its own, over everything (when the page can't be drawn into).
void DrawPulse(const Frame& f)
{
    static const ui::Text name = ui::MakeText("findSkillPulse");
    LayerScope scope(name);
    DrawSpots(f, scope.root);
}

// Inside the page, at the nodes' depth: over the nodes, under the card of the node under the
// cursor. The depth and the widget close in destructors.
struct DepthScope {
    alignas(16) uint8_t raw[0x40] = {};
    explicit DepthScope(uint8_t depth) { g_pushDepth(raw, depth); }
    ~DepthScope() { g_popDepth(raw); }
};

void DrawPulseInPage(const Frame& f)
{
    static const ui::Text name = ui::MakeText("findSkillPulse");
    DepthScope depth(kNodeDepth);
    Widget root;
    g_named(root.raw, &name);
    if (uint8_t* node = root.Handle()) {
        // Placed, not laid out (as game_hints.h's tips), and of no size: it sits beside the hint
        // bar, whose layout would otherwise make room for the pulse (the tips bounced).
        *reinterpret_cast<uint16_t*>(node + 0x6e) = 0x0101;
        *reinterpret_cast<uint64_t*>(node + 0x70) = 0;
        g_setSize(node, 0);
    }
    DrawSpots(f, root);
    g_end(root.raw);
}

Frame g_frame;           // read this frame (Draw), drawn by the page (DrawInPage)
bool g_frameReady = false;
bool g_inPage = false;   // the page draws the pulse (its hint bar call is Find's)

} // namespace

bool Init()
{
    auto find = [](auto& out, const char* name, const char* pattern) {
        out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(FindUnique(pattern));
        if (!out)
            Log("skills: signature not found: %s", name);
        return out != nullptr;
    };
    bool ok = true;
    ok &= find(g_uiCtx, "UiContext", "48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 4C 8B 41 28");
    ok &= find(g_begin, "BeginWidget", "40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B D9 45 33 C0");
    ok &= find(g_end, "EndWidget", "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 60 80 79 20 00");
    ok &= find(g_setSize, "SetSize", "48 89 54 24 08 0F 57 C0 F3 0F 10 54 24 08");
    ok &= find(g_image, "Image", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 60 41 0F 10 10 33 C0 C7 44 24 30 97 69 E8 F9 48 89 44 24 34 48 8B D9 49 8B 48 08 48 8B FA 89 44 24 3C 66 48 0F 7E D0 48 0B C8 0F 10 44 24 30 0F 11 54 24 40 0F 11 44 24 50 75 ?? 48 83 C3 10 EB ?? 48 8B 43 10 48 8D 54 24 40 48 83 C3 10 45 33 C0");
    ok &= find(g_drawImage, "DrawImage", "4D 8B 09 45 8B 00 E9 ?? ?? ?? ?? CC");
    ok &= find(g_beginLayer, "BeginLayer", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 45 8B D9 44 8B 8C 24 80 00 00 00 41 0F B6 D8");
    ok &= find(g_endLayer, "EndLayer", "40 53 48 83 EC 20 48 89 74 24 38 48 8B D9 48 8B 35 ?? ?? ?? ?? 48 89 7C 24 40 48 8B 79 10 48 8B 09 E8");
    if (!find(g_pushDepth, "PushDepth", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 8B F1 0F B6 FA 48 8B 4B 18 E8") ||
        !find(g_popDepth, "PopDepth", "48 8B 0D ?? ?? ?? ?? 48 8B 49 18 E9 ?? ?? ?? ?? 48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 45 33 C0"))
        g_pushDepth = nullptr; // the pulse goes over everything instead
    ok &= find(g_named, "BeginNamedWidget", "48 89 5C 24 08 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B F9 48 8B DA 45 33 C0 48 8B 52 10 48 8B 48 18 48 89 0F 48 C7 47");
    // Creative Mode hooks the text lookup (see ui.cpp): also found by the code after its first 18 bytes.
    g_locaText = reinterpret_cast<LocaTextFn>(FindUnique("48 89 5C 24 10 48 89 74 24 18 55 57 41 55 41 56 41 57 48 8D AC 24 F0 F8 FF FF"));
    if (!g_locaText) {
        uint8_t* after = FindUnique("48 8D AC 24 F0 F8 FF FF 48 81 EC 10 08 00 00 4C 8B F1 48 C7 85 F0 00 00 00 40 00 00 00");
        if (after && IsHookPatch(after - 18))
            g_locaText = reinterpret_cast<LocaTextFn>(after - 18);
    }
    ok &= g_locaText != nullptr;
    g_ready = ok;
    Log(ok ? "skills: ready" : "skills: off (signatures above)");
    return ok;
}

std::vector<Skill> List(std::string& why)
{
    std::vector<Skill> out;
    if (!g_ready) {
        why = "signatures missing";
        return out;
    }
    uint8_t* tree = Tree();
    if (!tree) {
        why = "skill tree state not found";
        return out;
    }
    std::map<std::string, size_t> byName;
    static NodeText nodes[0x100];
    int n = Nodes(tree, nodes, 0x100);
    int unnamed = 0;
    for (int i = 0; i < n; ++i) {
        uint32_t id = nodes[i].id;
        std::string name = Plain(LocaText(nodes[i].name));
        if (name.empty()) {
            ++unnamed;
            continue;
        }
        auto it = byName.find(name);
        if (it == byName.end()) {
            Skill s;
            s.name = name;
            s.desc = Plain(LocaText(nodes[i].desc));
            byName[name] = out.size();
            out.push_back(std::move(s));
            it = byName.find(name);
        }
        out[it->second].nodes.push_back(id);
    }
    Log("skills: %d nodes on the page, %zu skills by name, %d without a name", n, out.size(), unnamed);
    if (out.empty())
        why = "no named skills";
    return out;
}

namespace {
struct NodePlace {
    uint32_t id, type;
    float x, y;
};

int Places(uint8_t* tree, NodePlace* out, int max)
{
    __try {
        int n = (std::min)(static_cast<int>(ReadAt<uint16_t>(tree, kTreeCount)), max), k = 0;
        for (int i = 0; i < n; ++i) {
            uint8_t* rec = ReadAt<uint8_t*>(tree + kTreeEntries + i * kEntrySize, kEntryRecord);
            if (rec)
                out[k++] = {ReadAt<uint32_t>(rec, 0), ReadAt<uint32_t>(rec, kRecordType), ReadAt<float>(rec, kRecordPos),
                            ReadAt<float>(rec, kRecordPos + 4)};
        }
        return k;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct Hint {
    uint8_t label[16];
    int32_t degrees;
};

int Hints(Hint* out, int max)
{
    __try {
        uint8_t* field = Assets() + kSkillTree + kClassHints;
        int32_t rel = ReadAt<int32_t>(field, 0);
        int n = (std::min)(static_cast<int>(ReadAt<uint32_t>(field, 4)), max);
        for (int i = 0; i < n; ++i) {
            const uint8_t* h = field + rel + i * kHintSize;
            memcpy(out[i].label, h, 16);
            out[i].degrees = ReadAt<int32_t>(h, kHintDegrees);
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
} // namespace

std::vector<Category> Categories()
{
    std::vector<Category> out;
    uint8_t* tree = g_ready ? Tree() : nullptr;
    if (!tree)
        return out;
    static Hint hints[32];
    static NodePlace nodes[0x100];
    int h = Hints(hints, 32), n = Places(tree, nodes, 0x100);
    // The middle of the tree: the ring of root nodes (Spark) around it.
    float cx = 0, cy = 0;
    int roots = 0;
    for (int i = 0; i < n; ++i)
        if (nodes[i].type == 0) {
            cx += nodes[i].x;
            cy += nodes[i].y;
            ++roots;
        }
    if (!h || !roots)
        return out;
    cx /= roots;
    cy /= roots;
    out.resize(h);
    for (int i = 0; i < h; ++i)
        out[i].name = Plain(ui::LocaKey(hints[i].label));
    // A label stands at its direction from the middle (degrees, as atan2 of the canvas's y and
    // x); each node belongs to the label nearest its own direction (30-degree sectors).
    for (int i = 0; i < n; ++i) {
        if (nodes[i].type == 0)
            continue;
        float a = std::atan2(nodes[i].y - cy, nodes[i].x - cx) * 57.29578f;
        int best = -1;
        float bestD = 1e9f;
        for (int k = 0; k < h; ++k) {
            float d = std::fmod(a - static_cast<float>(hints[k].degrees) + 720.0f, 360.0f);
            d = (std::min)(d, 360.0f - d);
            if (d < bestD) {
                bestD = d;
                best = k;
            }
        }
        if (best < 0)
            continue;
        out[best].nodes.push_back(nodes[i].id);
        out[best].centre[0] += nodes[i].x;
        out[best].centre[1] += nodes[i].y;
    }
    std::vector<Category> named;
    for (Category& c : out)
        if (!c.name.empty() && !c.nodes.empty()) {
            c.centre[0] /= static_cast<float>(c.nodes.size());
            c.centre[1] /= static_cast<float>(c.nodes.size());
            named.push_back(std::move(c));
        }
    return named;
}

void Highlight(const std::vector<uint32_t>& nodes, unsigned ms, const float* centre)
{
    g_lit = nodes;
    g_litStart = GetTickCount64();
    g_litUntil = g_litStart + ms;
    g_centreOn = nodes.size() == 1 ? nodes[0] : 0;
    g_centrePoint = !g_centreOn && centre;
    if (centre)
        memcpy(g_centreAt, centre, sizeof(g_centreAt));
    g_centreUntil = g_litStart + 2000;
    g_drawFault = false;
}

void Clear()
{
    g_lit.clear();
    g_litUntil = 0;
    g_centreOn = 0;
    g_centrePoint = false;
}

void Draw(int screen)
{
    if (!g_ready || g_lit.empty() || g_drawFault)
        return;
    ULONGLONG now = GetTickCount64();
    if (now >= g_litUntil) {
        Clear();
        return;
    }
    if (screen != kScreen)
        return;
    uint8_t* tree = Tree();
    uint8_t* canvas = tree ? Canvas(tree) : nullptr;
    if (!canvas)
        return;
    Frame& f = g_frame;
    g_frameReady = false;
    if (!ReadFrame(tree, canvas, f)) {
        g_drawFault = true; // one fault and the pulse stays off
        Log("skills: fault reading the skill tree, the pulse is off until the next search");
        return;
    }
    static ULONGLONG logged = 0;
    if (logged != g_litStart) {
        logged = g_litStart;
        Log("skills: pulse on %d nodes on screen, first at %.0f %.0f (view %.0f %.0f), size %.0f", f.count, f.count ? f.spots[0].x : 0.0f,
            f.count ? f.spots[0].y : 0.0f, f.origin[0], f.origin[1], f.count ? f.spots[0].size : 0.0f);
    }
    if (!f.count)
        return;
    if (g_inPage && g_pushDepth)
        g_frameReady = true; // the page draws it (DrawInPage)
    else
        DrawPulse(f);
}

void UsePageDrawing(bool on) { g_inPage = on; }

void DrawInPage()
{
    if (!g_frameReady)
        return;
    g_frameReady = false;
    DrawPulseInPage(g_frame);
}

#ifdef FIND_DEV
namespace {
void DevDumpBody()
{
    {
        // The class labels (categories): keen::FbUiSkillTree + 0x78 classHints, 0x24 each.
        uint8_t* assets = Assets();
        uint8_t* field = assets + kSkillTree + 0x78;
        int32_t rel = ReadAt<int32_t>(field, 0);
        uint32_t n = ReadAt<uint32_t>(field, 4);
        for (uint32_t i = 0; i < n && i < 64; ++i) {
            uint8_t* h = field + rel + i * 0x24;
            Log("dev skills: hint %u '%s' primary %u colour %08x dir %d dist %.1f", i, ui::LocaKey(h).c_str(), h[0x10],
                ReadAt<uint32_t>(h, 0x14), ReadAt<int32_t>(h, 0x1c), ReadAt<float>(h, 0x20));
        }
        uint8_t* tree = Tree();
        uint16_t count = tree ? ReadAt<uint16_t>(tree, kTreeCount) : 0;
        for (uint16_t i = 0; i < count; ++i) {
            uint8_t* rec = ReadAt<uint8_t*>(tree + kTreeEntries + i * kEntrySize, kEntryRecord);
            if (rec)
                Log("dev skills: n %u type %u class %u pos %.0f %.0f '%s'", ReadAt<uint32_t>(rec, 0), ReadAt<uint32_t>(rec, 4),
                    rec[0x1e], ReadAt<float>(rec, kRecordPos), ReadAt<float>(rec, kRecordPos + 4),
                    LocaText(ReadAt<uint32_t>(rec, kRecordName)).c_str());
        }
    }
    {
        uint8_t* sys = System();
        Log("dev skills: sys %p mask %x buckets %p", sys, sys ? ReadAt<uint32_t>(sys, kStateMask) : 0,
            sys ? ReadAt<void*>(sys, kStateBuckets) : nullptr);
        uint8_t* tree = Tree();
        uint8_t* canvas = tree ? Canvas(tree) : nullptr;
        Log("dev skills: tree %p canvas %p (canvas by hash %p)", tree, canvas, FindState(kCanvasHash, kCanvasSize));
        if (!tree)
            return;
        uint16_t n = ReadAt<uint16_t>(tree, kTreeCount);
        Log("dev skills: %u nodes, tree +0x50 %llx +0x60 %llx +0x68 %llx", n, ReadAt<uint64_t>(tree, 0x50),
            ReadAt<uint64_t>(tree, 0x60), ReadAt<uint64_t>(tree, 0x68));
        if (canvas) {
            const float* f = reinterpret_cast<const float*>(canvas);
            Log("dev skills: canvas zoom %.3f centre %.1f %.1f target %.1f %.1f shown %.1f %.1f from %.1f %.1f size %.1f %.1f "
                "middle %.1f %.1f pivot %.1f %.1f scaled %.1f %.1f model %.1f %.1f widget %p",
                f[0x10 / 4], f[0x20 / 4], f[0x24 / 4], f[0x30 / 4], f[0x34 / 4], f[0x1b0 / 4], f[0x1b4 / 4], f[0x1a8 / 4],
                f[0x1ac / 4], f[0x1e8 / 4], f[0x1ec / 4], f[0x1f0 / 4], f[0x1f4 / 4], f[0x18c / 4], f[0x190 / 4],
                f[0x1f8 / 4], f[0x1fc / 4], f[0x38 / 4], f[0x3c / 4], ReadAt<void*>(canvas, kCanvasWidget));
            uint8_t* view = ReadAt<uint8_t*>(canvas, kCanvasWidget);
            if (view) {
                const float* r = reinterpret_cast<const float*>(view + kWidgetRect);
                Log("dev skills: view rect %.1f %.1f %.1f %.1f", r[0], r[1], r[2], r[3]);
            }
            bool ok = false;
            ULONGLONG now = UiTimeNs(ok);
            Log("dev skills: ui time %llu (ok %d) glide start %llu", now, ok, ReadAt<ULONGLONG>(canvas, 8));
        }
        for (uint16_t i = 0; i < n && i < 6; ++i) {
            uint8_t* e = tree + kTreeEntries + i * kEntrySize;
            uint8_t* rec = ReadAt<uint8_t*>(e, kEntryRecord);
            char hex[0x44 * 3 + 1] = {};
            for (int b = 0; rec && b < 0x44; ++b)
                sprintf_s(hex + b * 3, 4, "%02x ", rec[b]);
            Log("dev skills: node %u scaled %.1f %.1f rec %p name '%s' | %s", ReadAt<uint32_t>(e, 0), ReadAt<float>(e, 8),
                ReadAt<float>(e, 12), rec, rec ? LocaText(ReadAt<uint32_t>(rec, kRecordName)).c_str() : "?", hex);
        }
    }
}
} // namespace

void DevDump()
{
    __try {
        DevDumpBody();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("dev skills: fault");
    }
}
#endif

} // namespace skills
