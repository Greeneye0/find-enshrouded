#include "hotapi.h"
// The game's immediate-mode UI (build 1269579).
//
// G = *0x14281b498. *(G + 200): UI assets and localized texts (image references are 16 bytes,
// texts are 16-byte keys hashed with fnv1a32). *(**(G + 0xd0) + 0x38): the client.
//
// World markers copy 140c32210, the HUD's waypoint marker (inside "SpatialElementsWindow"):
//   140a43f80 Project(G, &out{x, y, ..}, client + 0x1d20 view, client + 0x1d60 projection,
//             &worldPos, &clamp{4 floats, u8 enabled at +0x10}) -> in front of the camera
//   a child widget (140a2e6c0 Begin, handle at +8) placed at the point, holding a 32x32 child
//   centred on it that draws three images: fill (assets + 0xb968, colour assets + 0xe2c8),
//   outline (+0xb978, 0xcd000000) and the marker icon (+0xb988, white).
//   Widget handle: +0x70 packed position (x, y floats), size via 1411b1050, rect at +0x114.
//   Image handle: 140a3d3c0(140a3c2c0(), &tmp, &ref); drawn by 140a3a6c0(widget, rect,
//   &image, &colour{u32, float}).
//
// Text input dialog: 140cc8e30(desc) draws it for one frame (0 open, 1 cancel, 2 confirm).
//   desc (initialised by 140b12d70): +0x160 1 = password (masked, eye toggle), +0x18 -> text {ptr, len, cap, ..},
//   +0x20 button pointers {data, count, capacity}, +0xa0 title text, +0xb8 the field's
//   starting text (copied in when the dialog opens), +0xd0 lines shown between the field and
//   the buttons {Text* data, count, capacity; 4 inline}, +0x158 max length (the text buffer
//   has no grow function, so never more than its capacity),
//   +0x164 width.
//   button: +0x04 0x10202 (+0x07 set when clicked), +0x08 0x100, +0x10 3, +0x4c 10.0,
//   +0x50 5.0, +0x54 1.0, +0x58 1, +0x78 label text.
//   The field (140b12120) keeps what is typed in its own edit state, looked up every frame by
//   140af9040 FieldState(field, 0x64d2790860d9c7f6, 1) (0x1990 bytes); the state takes the
//   desc's text only when it is created. 1411b0370 SetEditText(&result, state + 0x10, &utf8)
//   replaces the edited text and puts the cursor at its end; Find calls it from a hook on
//   FieldState while its own dialog draws.
//   Under the buttons Find adds a small hint line: the dialog ends its button row with
//   140a31b70 EndWidget(&row) (the call followed by "mov rsi, [rsp+0xc00]; cmp [rbp+0x28], r15b");
//   a hook on EndWidget draws a text label right after that call, so the label is laid out
//   below the buttons. Labels as the main menu draws its small texts:
//   140b0c760 BeginLabel(label, text, style, 0) (label: +0x08 widget, whose +0x6c is the
//   alignment; +0x3d 1; +0xb0 colour u32 + alpha float), 140b13f50 EndLabel(label); the small
//   text style is assets + 0x10a0c.
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>
#include "log.h"
#include "scan.h"
#include "ui.h"
#include "projection_frame.h"

namespace ui {
namespace {

using ProjectFn = bool (*)(void* g, float* out, const void* view, const void* proj, const float* pos, const void* clamp);
using BeginWidgetFn = void (*)(void* widget, uint64_t parent);
using EndWidgetFn = void (*)(void* widget);
using SetSizeFn = void (*)(void* handle, uint64_t packedSize);
using UiCtxFn = void* (*)();
using ImageFn = uint32_t* (*)(void* ctx, void* tmp, const void* ref);
using DrawImageFn = void (*)(void* widget, const float* rect, const uint32_t* image, const uint64_t* colour);
using DialogFn = int (*)(void* desc);
using DialogInitFn = void (*)(void* desc);
using LocaTextFn = Text* (*)(Text* out, uint32_t key);
using FieldStateFn = uint8_t* (*)(void* field, uint64_t id, uint32_t create);
using SetEditTextFn = uint64_t* (*)(uint64_t* result, uint8_t* edit, const void* utf8);
using BeginLabelFn = void (*)(uint8_t* label, const Text* text, const uint8_t* style, uint8_t flag);
using EndLabelFn = void (*)(uint8_t* label);

void** g_root = nullptr;
ProjectFn g_project = nullptr;
BeginWidgetFn g_begin = nullptr;
EndWidgetFn g_end = nullptr;
SetSizeFn g_setSize = nullptr;
UiCtxFn g_uiCtx = nullptr;
ImageFn g_image = nullptr;
DrawImageFn g_drawImage = nullptr;
DialogFn g_dialog = nullptr;
DialogInitFn g_dialogInit = nullptr;
LocaTextFn g_locaText = nullptr;
FieldStateFn g_originalFieldState = nullptr;
SetEditTextFn g_setEditText = nullptr;
constexpr uint64_t kTextFieldId = 0x64d2790860d9c7f6ull;
constexpr size_t kStateEdit = 0x10;
using CloseKeyboardFn = void (*)(void*, uint32_t);
CloseKeyboardFn g_closeKeyboard = nullptr;
uint8_t* g_findEditState = nullptr;
uint8_t* g_findEditOwner = nullptr;
Dialog* g_drawing = nullptr; // the Find dialog while it draws
EndWidgetFn g_originalEnd = nullptr;
BeginLabelFn g_beginLabel = nullptr;
EndLabelFn g_endLabel = nullptr;
void* g_afterButtons = nullptr; // return address of the dialog's "end the button row" call
constexpr size_t kAssetSmallText = 0x10a0c;
constexpr uint32_t kHintColour = 0xffc8c8c8;
constexpr float kHintAlpha = 0.6f;

constexpr size_t kAssetMarkerFill = 0xb968;
constexpr size_t kAssetMarkerOutline = 0xb978;
constexpr size_t kAssetMarkerIcon = 0xb988;
constexpr size_t kAssetMarkerFillColour = 0xe2c8;
constexpr size_t kLocaCancel = 0x2110;
// FbUiPrimitives (assets + 0xd250): circle +0x00, circleOutline +0x10, circleSoftGlow +0x20,
// ringSoftGlow +0x30, triangle +0x50 (the menu arrow, 0xd2a0), gradients +0xa0.
constexpr size_t kAssetSoftGlow = 0xd250 + 0x20;
constexpr size_t kAssetRingGlow = 0xd250 + 0x30;

uint8_t* Assets()
{
    void* g = *g_root;
    return g ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(g) + 200) : nullptr;
}

uint64_t Pack(float x, float y)
{
    uint64_t v;
    float xy[2] = {x, y};
    memcpy(&v, xy, 8);
    return v;
}

// Widget: 0x80 bytes of the game's widget state; the handle is at +8.
struct Widget {
    alignas(16) uint8_t raw[0x80] = {};
    uint8_t* Handle() { return *reinterpret_cast<uint8_t**>(raw + 8); }
};

template <class T> bool Find(T& out, const char* name, const char* pattern)
{
    out = reinterpret_cast<T>(FindUnique(pattern));
    if (!out)
        Log("signature not found: %s", name);
    return out != nullptr;
}

uint32_t Fnv1a32(const uint8_t* p, size_t n)
{
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 0x1000193u;
    return h;
}

struct PendingText {
    bool set = false;
    std::string text;
};
PendingText g_pending;

uint8_t* FieldStateHookBody(void* field, uint64_t id, uint32_t create);
uint8_t* FieldStateHook(void* field, uint64_t id, uint32_t create)
{
    hot::Lease lease;
    if (!lease) return g_originalFieldState(field, id, create);
    return FieldStateHookBody(field, id, create);
}
void DrawHighlights(Dialog* d, void* widget);
bool g_barsDrawn = false; // this frame's highlight bars are drawn (FieldState runs twice a frame)

// Where the dialog's field and button row were laid out (UI units {x, y, w, h}); the lines
// between them are the suggestions, picked with the mouse.
float g_fieldRect[4] = {};
float g_rowRect[4] = {};

void CopyRect(const void* widget, float out[4])
{
    uint8_t* handle = nullptr;
    hot::Read(static_cast<const uint8_t*>(widget) + 8, &handle, sizeof(handle));
    if (handle)
        hot::Read(handle + 0x114, out, 4 * sizeof(float));
}

uint8_t* FieldStateHookBody(void* field, uint64_t id, uint32_t create)
{
    uint8_t* state = g_originalFieldState(field, id, create);
    if (g_drawing && id == kTextFieldId && state) {
        g_findEditState = state;
        auto root = static_cast<uint8_t*>(G());
        g_findEditOwner = root ? *reinterpret_cast<uint8_t**>(root + 0xd0) : nullptr;
    }
    if (g_drawing && id == kTextFieldId && field) {
        CopyRect(field, g_fieldRect);
        // The lines come after the field: bars drawn now sit behind their text (positions are
        // last frame's, as the lines do not move between frames).
        if (!g_barsDrawn) {
            g_barsDrawn = true;
            DrawHighlights(g_drawing, field);
        }
    }
    if (g_drawing && g_pending.set && id == kTextFieldId && state && g_setEditText) {
        g_pending.set = false;
        struct {
            const char* ptr;
            size_t len;
        } utf8{g_pending.text.c_str(), g_pending.text.size()};
        uint64_t result = 0;
        __try {
            g_setEditText(&result, state + kStateEdit, &utf8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_setEditText = nullptr;
            Log("dialog: could not set the field's text; Tab only selects from now on");
        }
    }
    return state;
}

void DrawHint(Dialog* d);
void DrawScroll(Dialog* d, void* widget);

void EndWidgetHookBody(void* widget, void* returnSite);
void EndWidgetHook(void* widget)
{
    void* site = _ReturnAddress();
    hot::Lease lease;
    if (!lease) return g_originalEnd(widget);
    EndWidgetHookBody(widget, site);
}
void EndWidgetHookBody(void* widget, void* returnSite)
{
    g_originalEnd(widget);
    if (g_drawing && returnSite == g_afterButtons) {
        CopyRect(widget, g_rowRect);
        DrawScroll(g_drawing, widget);
        DrawHint(g_drawing);
    }
}

// FieldState starts "push rbx; sub rsp, 30h; mov rcx, [rcx+8]; lea r9, [rip+x]" (17 bytes).
// The trampoline runs the first three and loads r9 with the absolute address.
bool HookFieldState(uint8_t* start)
{
    if (IsAbsJumpPatch(start)) {
        Log("dialog: the text field is hooked by another mod; Tab only selects");
        return false;
    }
    uint8_t* target = start + 17 + ([&]{ int32_t d; hot::Read(start + 13, &d, 4); return d; }());
    uint8_t prologue[10 + 10];
    hot::Read(start, prologue, 10);
    prologue[10] = 0x49; // mov r9, imm64
    prologue[11] = 0xB9;
    memcpy(prologue + 12, &target, 8);
    g_originalFieldState = reinterpret_cast<FieldStateFn>(
        InstallHook(start, reinterpret_cast<void*>(&FieldStateHook), 17, prologue, sizeof(prologue)));
    return g_originalFieldState != nullptr;
}

} // namespace

Text MakeText(const char* utf8)
{
    Text t{utf8, strlen(utf8), 0xcbf29ce484222325ull};
    for (size_t i = 0; i < t.len; ++i)
        t.hash = (t.hash ^ static_cast<uint8_t>(utf8[i])) * 0x100000001b3ull;
    return t;
}

std::string LocaKey(const uint8_t* key16)
{
    Text t = MakeText("");
    g_locaText(&t, Fnv1a32(key16, 16));
    return std::string(t.utf8 ? t.utf8 : "", t.utf8 ? t.len : 0);
}

Text Loca(size_t offset)
{
    Text t = MakeText("");
    uint8_t* assets = Assets();
    if (assets)
        g_locaText(&t, Fnv1a32(assets + offset, 16));
    return t;
}

// --- item icons that light up ---
//
// Item icons are images (ItemInfo + 0x56c, a 16-byte UiImageReference) turned into an image id
// by Image() and drawn by DrawImage(widget, rect, &id, &colour), a thunk to
// 14118ca30 DrawImageReal(widget, rect, u32 id, u64 colour). Image() is hooked to learn which
// ids are the wanted items' icons; DrawImageReal is hooked to draw a soft glow behind them and
// a ring over them, in the same rect, while lighting is on.
// Inventory icons are not drawn from that reference, though: an inventory slot is drawn by
// 140bf4180 doItemSlot(slot, style, ..), *slot -> slot data, its + 8 the item id (0: empty); it
// draws the slot background (140d63540 renderItemSlotBackground) and the item. It is hooked: while it
// draws a wanted item, the slot's first image (its background) is followed by the game's own
// nine-slice glow boxes (FbUiPrimitives boxInnerGlow +0x120, glowBoxSmall +0x140) in gold,
// drawn with 140a3a1b0 DrawNineSlice(widget, rect{x, y, w, h}, ref, &borders[4], colour).
namespace {

using DrawRealFn = void (*)(void* widget, const float* rect, uint32_t id, uint64_t colour);
using SlotFn = void (*)(uint8_t** slot, void* style, void* extra);
using NineSliceFn = void (*)(void* widget, const float* rect, const void* ref, const float* borders, uint64_t colour);
SlotFn g_originalSlot = nullptr;
NineSliceFn g_nineSlice = nullptr;
std::vector<uint32_t> g_slotItems; // the wanted items, by id
bool g_inWantedSlot = false;        // DrawItemSlot is drawing a wanted item
bool g_slotLit = false;             // ...and its highlight is drawn
int g_slotCalls = 0, g_slotWanted = 0, g_slotDraws = 0; // diagnostics while lighting is on
uint32_t g_slotSeen[8] = {};
std::vector<uint32_t> g_slotIds; // diagnostics: distinct item ids the slots showed
constexpr size_t kAssetBoxInnerGlow = 0xd250 + 0x120;
constexpr size_t kAssetGlowBoxSmall = 0xd250 + 0x140;
ImageFn g_originalImage = nullptr;
DrawRealFn g_originalDrawReal = nullptr;
std::vector<std::array<uint8_t, 16>> g_iconRefs; // the wanted items' icon references
std::vector<uint32_t> g_iconIds;                 // their image ids, as learned
float g_iconAlpha = 0.0f;                        // 0: off
uint32_t g_iconColour = 0;
bool g_inIconDraw = false;
int g_iconHits = 0;

uint32_t* ImageHookBody(void* ctx, void* tmp, const void* ref);
uint32_t* ImageHook(void* ctx, void* tmp, const void* ref)
{
    hot::Lease lease;
    if (!lease) return g_originalImage(ctx, tmp, ref);
    return ImageHookBody(ctx, tmp, ref);
}
uint32_t* ImageHookBody(void* ctx, void* tmp, const void* ref)
{
    uint32_t* id = g_originalImage(ctx, tmp, ref);
    if (!g_iconRefs.empty() && id && ref) {
        for (const auto& want : g_iconRefs) {
            if (memcmp(ref, want.data(), 16) == 0) {
                if (std::find(g_iconIds.begin(), g_iconIds.end(), *id) == g_iconIds.end() && g_iconIds.size() < 256)
                    g_iconIds.push_back(*id);
                break;
            }
        }
    }
    return id;
}

uint32_t AssetImage(size_t offset)
{
    uint8_t* assets = Assets();
    if (!assets)
        return 0;
    alignas(16) uint8_t tmp[16];
    uint32_t* id = g_image(g_uiCtx(), tmp, assets + offset);
    return id ? *id : 0;
}

uint64_t Colour(uint32_t rgba, float alpha)
{
    struct {
        uint32_t rgba;
        float alpha;
    } c{rgba, alpha};
    uint64_t v;
    memcpy(&v, &c, 8);
    return v;
}

void DrawRealHookBody(void* widget, const float* rect, uint32_t id, uint64_t colour);
// Diagnostics: which code draws images during one traced frame (offsets into the exe).
bool g_tracing = false;
struct TraceSite {
    uintptr_t site;
    int count;
    float w, h;
};
TraceSite g_trace[96];
int g_traceSites = 0;

void TraceDraw(void* returnAddress, const float* rect)
{
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t site = reinterpret_cast<uintptr_t>(returnAddress) - base;
    for (int i = 0; i < g_traceSites; ++i) {
        if (g_trace[i].site == site) {
            ++g_trace[i].count;
            return;
        }
    }
    if (g_traceSites < 96)
        g_trace[g_traceSites++] = {site, 1, rect ? rect[2] : 0.0f, rect ? rect[3] : 0.0f};
}

void DrawRealHook(void* widget, const float* rect, uint32_t id, uint64_t colour)
{
    hot::Lease lease;
    if (!lease) return g_originalDrawReal(widget, rect, id, colour);
    if (g_tracing)
        TraceDraw(_ReturnAddress(), rect);
    DrawRealHookBody(widget, rect, id, colour);
}
// The highlight of a wanted item's slot, over its background.
void DrawSlotHighlight(void* widget, const float* rect)
{
    uint8_t* assets = Assets();
    if (!assets || !g_nineSlice)
        return;
    float w = rect[2], h = rect[3];
    if (!(w > 4.0f && h > 4.0f) || w > 1000.0f || h > 1000.0f)
        return;
    float b = (w < h ? w : h) * 0.3f;
    float borders[4] = {b, b, b, b};
    // outer glow, a little larger than the slot
    float grow = (w < h ? w : h) * 0.18f;
    float outer[4] = {rect[0] - grow, rect[1] - grow, w + 2 * grow, h + 2 * grow};
    float ob = b + grow;
    float outerBorders[4] = {ob, ob, ob, ob};
    g_nineSlice(widget, outer, assets + kAssetGlowBoxSmall, outerBorders, Colour(g_iconColour, g_iconAlpha));
    g_nineSlice(widget, rect, assets + kAssetBoxInnerGlow, borders, Colour(g_iconColour, (g_iconAlpha + 1.0f) * 0.5f));
}

void DrawRealHookBody(void* widget, const float* rect, uint32_t id, uint64_t colour)
{
    if (g_inWantedSlot)
        ++g_slotDraws;
    if (g_inWantedSlot && !g_slotLit && !g_inIconDraw) {
        g_slotLit = true;
        g_inIconDraw = true;
        ++g_iconHits;
        g_originalDrawReal(widget, rect, id, colour); // the slot's background
        DrawSlotHighlight(widget, rect);
        g_inIconDraw = false;
        return;
    }
    bool light = g_iconAlpha > 0.0f && !g_inIconDraw && std::find(g_iconIds.begin(), g_iconIds.end(), id) != g_iconIds.end();
    if (!light) {
        g_originalDrawReal(widget, rect, id, colour);
        return;
    }
    g_inIconDraw = true;
    ++g_iconHits;
    uint32_t glow = AssetImage(kAssetSoftGlow);
    uint32_t ring = AssetImage(kAssetRingGlow);
    if (glow)
        g_originalDrawReal(widget, rect, glow, Colour(g_iconColour, g_iconAlpha));
    g_originalDrawReal(widget, rect, id, colour);
    if (ring)
        g_originalDrawReal(widget, rect, ring, Colour(g_iconColour, g_iconAlpha * 0.7f));
    g_inIconDraw = false;
}

void SlotHookBody(uint8_t** slot, void* style, void* extra);
void SlotHook(uint8_t** slot, void* style, void* extra)
{
    hot::Lease lease;
    if (!lease) return g_originalSlot(slot, style, extra);
    SlotHookBody(slot, style, extra);
}
void SlotHookBody(uint8_t** slot, void* style, void* extra)
{
    bool wanted = false;
    uint8_t* data = nullptr;
    if (slot)
        hot::Read(slot, &data, sizeof(data));
    if (!g_slotItems.empty() && data) { // counted always, lit only while lighting is on
        uint32_t item = 0;
        hot::Read(data + 8, &item, sizeof(item));
        bool match = item && std::find(g_slotItems.begin(), g_slotItems.end(), item) != g_slotItems.end();
        wanted = match && g_iconAlpha > 0.0f;
        if (g_slotCalls < 8)
            g_slotSeen[g_slotCalls] = item;
        if (item && g_slotIds.size() < 96 && std::find(g_slotIds.begin(), g_slotIds.end(), item) == g_slotIds.end())
            g_slotIds.push_back(item);
        ++g_slotCalls;
        g_slotWanted += match;
    }
    bool outerWanted = g_inWantedSlot, outerLit = g_slotLit;
    g_inWantedSlot = wanted;
    g_slotLit = false;
    g_originalSlot(slot, style, extra);
    g_inWantedSlot = outerWanted;
    g_slotLit = outerLit;
}

// ABI 1 identifies startup through its documented shadow-copy name. Optional new
// hook sites must wait for startup instead of poisoning a live reload transaction.
// Existing managed sites differ from their canonical original bytes.
bool SlotHookAvailable(uint8_t* target) {
    if (!hot::host) return true;
    uint8_t original[13]; hot::Read(target,original,sizeof(original));
    if (memcmp(target,original,sizeof(original)) != 0) return true;
    HMODULE module=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<const wchar_t*>(&SlotHookAvailable),&module)) return false;
    wchar_t path[MAX_PATH]; DWORD n=GetModuleFileNameW(module,path,MAX_PATH);
    return n>=6 && n<MAX_PATH && _wcsicmp(path+n-6,L".1.dll")==0;
}

bool HookIcons()
{
    auto* image = reinterpret_cast<uint8_t*>(g_image);
    auto* thunk = reinterpret_cast<uint8_t*>(g_drawImage);
    if (!image || !thunk || thunk[6] != 0xE9)
        return false;
    uint8_t* real = thunk + 11 + *reinterpret_cast<int32_t*>(thunk + 7);
    // DrawImageReal: mov rax, [rcx+8]; mov rcx, [rax+50h]; mov rcx, [rcx+18h] (12 bytes)
    static const uint8_t kReal[] = {0x48, 0x8B, 0x41, 0x08, 0x48, 0x8B, 0x48, 0x50, 0x48, 0x8B, 0x49, 0x18};
    if (!hot::Equal(real, kReal, sizeof(kReal)) || IsAbsJumpPatch(image))
        return false;
    // Image: mov [rsp+8], rbx; mov [rsp+10h], rsi; push rdi; sub rsp, 60h (15 bytes)
    g_originalImage = reinterpret_cast<ImageFn>(InstallHook(image, reinterpret_cast<void*>(&ImageHook), 15));
    if (!g_originalImage)
        return false;
    g_image = g_originalImage; // Find's own images skip the hook
    g_originalDrawReal = reinterpret_cast<DrawRealFn>(InstallHook(real, reinterpret_cast<void*>(&DrawRealHook), 12));
    if (!g_originalDrawReal)
        return false;
    // doItemSlot: mov rax, rsp; push rbp; push r15; lea rbp, [rax-228h] (13 bytes)
    uint8_t* slot = FindUnique("48 8B C4 55 41 57 48 8D A8 D8 FD FF FF");
    g_nineSlice = reinterpret_cast<NineSliceFn>(FindUnique("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 83 EC 30 48 8B F2"));
    if (slot && g_nineSlice && !IsAbsJumpPatch(slot) && SlotHookAvailable(slot))
        g_originalSlot = reinterpret_cast<SlotFn>(InstallHook(slot, reinterpret_cast<void*>(&SlotHook), 13));
    if (!g_originalSlot)
        Log("signatures: item slot %p, nine-slice %p - found items in a chest are not boxed", slot,
            reinterpret_cast<void*>(g_nineSlice));
    return true;
}

} // namespace

void TraceDraws(bool on)
{
    if (on) {
        g_traceSites = 0;
        g_tracing = true;
        return;
    }
    g_tracing = false;
    std::string line;
    for (int i = 0; i < g_traceSites; ++i) {
        char buf[64];
        _snprintf_s(buf, _TRUNCATE, " +%llx x%d (%.0fx%.0f)", static_cast<unsigned long long>(g_trace[i].site), g_trace[i].count,
                    g_trace[i].w, g_trace[i].h);
        line += buf;
        if (line.size() > 900) {
            Log("draw sites:%s", line.c_str());
            line.clear();
        }
    }
    Log("draw sites:%s (%d sites)", line.c_str(), g_traceSites);
}

void SetSlotTargets(const std::vector<uint32_t>& items)
{
    g_slotItems = items;
}

void SetIconTargets(const std::vector<std::array<uint8_t, 16>>& refs)
{
    g_iconRefs = refs;
    g_iconIds.clear();
}

void LightIcons(uint32_t colour, float alpha)
{
    g_iconColour = colour;
    g_iconAlpha = g_originalDrawReal ? alpha : 0.0f;
}

int IconIdsLearned()
{
    return static_cast<int>(g_iconIds.size());
}

std::vector<uint32_t> TakeSlotIds()
{
    std::vector<uint32_t> ids;
    ids.swap(g_slotIds);
    return ids;
}

void LogSlotDiagnostics()
{
    Log("slots: %d drawn, %d match, %d images in lit slots; wanted %u (%d ids), seen %u %u %u %u %u %u %u %u",
        g_slotCalls, g_slotWanted, g_slotDraws, g_slotItems.empty() ? 0u : g_slotItems[0], static_cast<int>(g_slotItems.size()),
        g_slotSeen[0], g_slotSeen[1], g_slotSeen[2], g_slotSeen[3], g_slotSeen[4], g_slotSeen[5], g_slotSeen[6], g_slotSeen[7]);
    g_slotCalls = g_slotWanted = g_slotDraws = 0;
    memset(g_slotSeen, 0, sizeof(g_slotSeen));
}

int TakeIconHits()
{
    int n = g_iconHits;
    g_iconHits = 0;
    return n;
}

bool Init()
{
    bool ok = true;
    g_root = reinterpret_cast<void**>(ResolveRipConsensus("48 8B 05 ?? ?? ?? ?? 48 8B 88 D0 00 00 00", 3, 7));
    if (!g_root) {
        Log("signature not found: G");
        ok = false;
    }
    ok &= Find(g_project, "Project", "4C 8B DC 53 48 81 EC 10 01 00 00 F3 0F 10 2D ?? ?? ?? ??");
    ok &= Find(g_begin, "BeginWidget", "40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B D9 45 33 C0");
    ok &= Find(g_end, "EndWidget", "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 60 80 79 20 00");
    ok &= Find(g_setSize, "SetSize", "48 89 54 24 08 0F 57 C0 F3 0F 10 54 24 08");
    ok &= Find(g_uiCtx, "UiContext", "48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 4C 8B 41 28");
    ok &= Find(g_image, "Image", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 60 41 0F 10 10 33 C0 C7 44 24 30 97 69 E8 F9 48 89 44 24 34 48 8B D9 49 8B 48 08 48 8B FA 89 44 24 3C 66 48 0F 7E D0 48 0B C8 0F 10 44 24 30 0F 11 54 24 40 0F 11 44 24 50 75 ?? 48 83 C3 10 EB ?? 48 8B 43 10 48 8D 54 24 40 48 83 C3 10 45 33 C0");
    ok &= Find(g_drawImage, "DrawImage", "4D 8B 09 45 8B 00 E9 ?? ?? ?? ?? CC");
    ok &= Find(g_dialog, "TextDialog", "48 8B C4 55 53 41 55 41 56 48 8D A8 18 F5 FF FF");
    ok &= Find(g_dialogInit, "DialogInit", "48 83 EC 28 0F 10 0D ?? ?? ?? ?? 48 C7 41 30 0A 00 00 00");
    // Creative Mode hooks the text lookup; Find only calls it (through that hook, as the game
    // does), so it is also found by the code after its first 18 bytes.
    g_locaText = reinterpret_cast<LocaTextFn>(FindUnique("48 89 5C 24 10 48 89 74 24 18 55 57 41 55 41 56 41 57 48 8D AC 24 F0 F8 FF FF"));
    if (!g_locaText) {
        uint8_t* after = FindUnique("48 8D AC 24 F0 F8 FF FF 48 81 EC 10 08 00 00 4C 8B F1 48 C7 85 F0 00 00 00 40 00 00 00");
        if (after && IsHookPatch(after - 18))
            g_locaText = reinterpret_cast<LocaTextFn>(after - 18);
    }
    if (!g_locaText) {
        Log("signature not found: LocaText");
        ok = false;
    }
    g_closeKeyboard = reinterpret_cast<CloseKeyboardFn>(FindUnique("48 89 5C 24 08 48 89 74 24 18 89 54 24 10 57 48 83 EC 50 0F 10 05 ?? ?? ?? ?? 8B C2 C7 44 24 20 27 00 00 00 83 E0 0F"));
    // Optional: item icons that light up (see HighlightIcons).
    if (!HookIcons())
        Log("signatures: image/draw - found items in an open chest do not light up");
    // Optional: the small hint line under the buttons.
    g_beginLabel = reinterpret_cast<BeginLabelFn>(FindUnique("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 50 48 8B DA 41 0F B6 F9"));
    g_endLabel = reinterpret_cast<EndLabelFn>(FindUnique("40 55 56 57 48 8D 6C 24 B9 48 81 EC F0 00 00 00 0F 29 BC 24 B0 00 00 00"));
    uint8_t* rowEnd = FindUnique("48 8D 4D A0 E8 ?? ?? ?? ?? 48 8B B4 24 00 0C 00 00 44 38 7D 28");
    uint8_t* rowEndTarget = rowEnd ? rowEnd + 9 + *reinterpret_cast<int32_t*>(rowEnd + 5) : nullptr;
    if (g_beginLabel && g_endLabel && g_end && rowEndTarget == reinterpret_cast<uint8_t*>(g_end) &&
        !IsAbsJumpPatch(reinterpret_cast<uint8_t*>(g_end))) {
        g_afterButtons = rowEnd + 9;
        // EndWidget: mov [rsp+18h], rbx; mov [rsp+20h], rsi; push rdi; sub rsp, 60h (15 bytes).
        g_originalEnd = reinterpret_cast<EndWidgetFn>(InstallHook(reinterpret_cast<uint8_t*>(g_end),
                                                                  reinterpret_cast<void*>(&EndWidgetHook), 15));
        if (g_originalEnd)
            g_end = g_originalEnd; // Find's own widgets skip the hook
    }
    if (!g_originalEnd) {
        g_beginLabel = nullptr;
        Log("signatures: label %p/%p, button row %p - no hint line under the buttons",
            reinterpret_cast<void*>(g_beginLabel), reinterpret_cast<void*>(g_endLabel), rowEnd);
    }
    // Optional: without these Tab still selects, but cannot fill in the field.
    auto* fieldState = FindUnique("40 53 48 83 EC 30 48 8B 49 08 4C 8D 0D ?? ?? ?? ?? 44 89 44 24 20 41 B8 90 19 00 00");
    g_setEditText = reinterpret_cast<SetEditTextFn>(FindUnique("48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 B8 40 18 00 00"));
    if (!fieldState || !g_setEditText || !HookFieldState(fieldState)) {
        g_setEditText = nullptr;
        Log("signatures: text field state %p, set text %p - Tab cannot fill in the field", fieldState,
            reinterpret_cast<void*>(g_setEditText));
    }
    return ok;
}

void* G()
{
    return g_root ? *g_root : nullptr;
}

void* Client()
{
    auto* g = static_cast<uint8_t*>(G());
    if (!g)
        return nullptr;
    auto** ui = *reinterpret_cast<uint8_t***>(g + 0xd0);
    if (!ui || !*ui)
        return nullptr;
    return *reinterpret_cast<void**>(*ui + 0x38);
}

bool CaptureProjection(ProjectionFrame& frame) {
    auto* root=static_cast<uint8_t*>(G());auto* client=static_cast<uint8_t*>(Client());
    if(!root||!client)return false;
    auto* window=*reinterpret_cast<uint8_t**>(root+0x10);if(!window)return false;
    memcpy(frame.view,client+0x1d20,sizeof(frame.view));
    memcpy(frame.projection,client+0x1d60,sizeof(frame.projection));
    memcpy(frame.viewport,window+0x18,sizeof(frame.viewport));
    return frame.viewport[2]>=1 && frame.viewport[3]>=1;
}

bool Project(const float world[3], float& x, float& y, float* depth)
{
    auto* client = static_cast<uint8_t*>(Client());
    if (!client)
        return false;
    alignas(16) float out[8] = {};
    alignas(16) uint8_t clamp[0x20] = {}; // no clamping to the screen edge
    if (!g_project(G(), out, client + 0x1d20, client + 0x1d60, world, clamp))
        return false;
    x = out[0];
    y = out[1];
    // Same view-space Z used by the native Project function (before divide).
    if(depth) {
        const float* view=reinterpret_cast<const float*>(client+0x1d20);
        *depth=world[0]*view[2]+world[1]*view[6]+world[2]*view[10]+view[14];
    }
    return x != 0.0f || y != 0.0f;
}

// Native white-texture fallback, submitted inside the existing spatial HUD window.
// Each strip is owned by this frame; no renderer instance or retained callback.
void DrawSolids(const SolidRect* rects, size_t count, uint32_t colour, float alpha)
{
    if (!G() || !g_originalDrawReal || !count || count>65536) return;
    Widget widget; g_begin(widget.raw,0);
    for (size_t i=0;i<count;++i) {
        const auto& r=rects[i];
        if (r.w>0 && r.h>0) g_originalDrawReal(widget.raw,&r.x,0,Colour((colour&0x00ffffffu)|(static_cast<uint32_t>((colour>>24)*(std::max)(0.f,(std::min)(1.f,alpha)))<<24),1.f));
    }
    g_end(widget.raw);
}

void DrawMarker(float x, float y, float alpha)
{
    uint8_t* assets = Assets();
    if (!assets)
        return;
    Widget anchor;
    g_begin(anchor.raw, 0);
    *reinterpret_cast<uint64_t*>(anchor.Handle() + 0x70) = Pack(x, y);

    Widget icon;
    g_begin(icon.raw, 0);
    g_setSize(icon.Handle(), Pack(32.0f, 32.0f));
    *reinterpret_cast<uint64_t*>(icon.Handle() + 0x70) = Pack(-16.0f, -16.0f);
    float rect[4];
    memcpy(rect, icon.Handle() + 0x114, sizeof(rect));

    struct Colour {
        uint32_t rgba;
        float alpha;
    };
    Colour fill;
    memcpy(&fill, assets + kAssetMarkerFillColour, sizeof(fill));
    fill.alpha *= alpha;
    Colour outline{0xcd000000u, alpha};
    Colour white{0xffffffffu, alpha};
    void* ctx = g_uiCtx();
    alignas(16) uint8_t tmp[16];
    uint64_t c;
    memcpy(&c, &fill, 8);
    g_drawImage(icon.raw, rect, g_image(ctx, tmp, assets + kAssetMarkerFill), &c);
    memcpy(&c, &outline, 8);
    g_drawImage(icon.raw, rect, g_image(ctx, tmp, assets + kAssetMarkerOutline), &c);
    memcpy(&c, &white, 8);
    g_drawImage(icon.raw, rect, g_image(ctx, tmp, assets + kAssetMarkerIcon), &c);
    g_end(icon.raw);
    g_end(anchor.raw);
}

void DrawGlow(float x, float y, float radius, uint32_t colour, float alpha, bool ring)
{
    uint8_t* assets = Assets();
    if (!assets || radius <= 0.0f)
        return;
    Widget anchor;
    g_begin(anchor.raw, 0);
    *reinterpret_cast<uint64_t*>(anchor.Handle() + 0x70) = Pack(x, y);
    Widget glow;
    g_begin(glow.raw, 0);
    g_setSize(glow.Handle(), Pack(2.0f * radius, 2.0f * radius));
    *reinterpret_cast<uint64_t*>(glow.Handle() + 0x70) = Pack(-radius, -radius);
    float rect[4];
    memcpy(rect, glow.Handle() + 0x114, sizeof(rect));
    struct {
        uint32_t rgba;
        float alpha;
    } c{colour, alpha};
    uint64_t packed;
    memcpy(&packed, &c, 8);
    alignas(16) uint8_t tmp[16];
    g_drawImage(glow.raw, rect, g_image(g_uiCtx(), tmp, assets + (ring ? kAssetRingGlow : kAssetSoftGlow)), &packed);
    g_end(glow.raw);
    g_end(anchor.raw);
}

struct TextSlice {
    char* ptr;
    size_t len;
    size_t cap;
    void* grow;
    void* growContext;
};

struct Dialog;
namespace {
void DrawHintLabel(const Text& text);
}

struct Dialog {
    alignas(16) uint8_t desc[0x400];
    alignas(16) uint8_t confirm[0x100];
    alignas(16) uint8_t cancel[0x100];
    TextSlice text;
    char buffer[64];
    Text title;
    Text confirmLabel;
    std::string lineStore[8];
    Text lines[8];
    std::string hintStore;
    Text hint{};
    int scrollFirst = 0, scrollVisible = 0, scrollTotal = 0; // the list's scroll position
    int selectedLine = -1, hoverLine = -1, shownLines = 0;   // bars behind lines (0-based, shown)
    uint32_t tints[8] = {};                                   // per shown line: 0xAABBGGRR, 0: none
};

namespace {

void DrawHint(Dialog* d)
{
    if (d->hint.len)
        DrawHintLabel(d->hint);
}

void DrawHintLabel(const Text& text)
{
    uint8_t* assets = Assets();
    if (!assets || !g_beginLabel)
        return;
    __try {
        alignas(16) uint8_t label[0x400] = {};
        g_beginLabel(label, &text, assets + kAssetSmallText, 0);
        label[0x3d] = 1;
        uint8_t* widget = *reinterpret_cast<uint8_t**>(label + 8);
        if (widget)
            *reinterpret_cast<uint16_t*>(widget + 0x6c) = 0x202;
        *reinterpret_cast<uint32_t*>(label + 0xb0) = kHintColour;
        *reinterpret_cast<float*>(label + 0xb4) = kHintAlpha;
        g_endLabel(label);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_beginLabel = nullptr;
        Log("dialog: could not draw the hint line; left out from now on");
    }
}

} // namespace

namespace {

void InitButton(uint8_t* b, const Text& label)
{
    memset(b, 0, 0x100);
    *reinterpret_cast<uint32_t*>(b + 0x04) = 0x10202;
    *reinterpret_cast<uint16_t*>(b + 0x08) = 0x100;
    *reinterpret_cast<uint32_t*>(b + 0x10) = 3;
    *reinterpret_cast<float*>(b + 0x4c) = 10.0f;
    *reinterpret_cast<float*>(b + 0x50) = 5.0f;
    *reinterpret_cast<float*>(b + 0x54) = 1.0f;
    b[0x58] = 1;
    memcpy(b + 0x78, &label, sizeof(label));
}

bool AddButton(uint8_t* desc, uint8_t* button)
{
    auto** data = *reinterpret_cast<uint8_t***>(desc + 0x20);
    uint64_t& count = *reinterpret_cast<uint64_t*>(desc + 0x28);
    uint64_t cap = *reinterpret_cast<uint64_t*>(desc + 0x30);
    if (!data || count >= cap)
        return false;
    data[count++] = button;
    return true;
}

} // namespace

Dialog* NewDialog(const char* title, const char* confirm)
{
    auto* d = new Dialog{};
    d->title = MakeText(title);
    d->confirmLabel = MakeText(confirm);
    g_dialogInit(d->desc);
    memcpy(d->desc + 0xa0, &d->title, sizeof(Text));
    *reinterpret_cast<float*>(d->desc + 0x164) = 800.0f;
    d->text = {d->buffer, 0, sizeof(d->buffer), nullptr, nullptr};
    *reinterpret_cast<TextSlice**>(d->desc + 0x18) = &d->text;
    InitButton(d->confirm, d->confirmLabel);
    InitButton(d->cancel, Loca(kLocaCancel));
    if (!AddButton(d->desc, d->confirm) || !AddButton(d->desc, d->cancel))
        Log("dialog: no room for the buttons");
    return d;
}

int DrawDialog(Dialog* d)
{
    g_drawing = d;
    g_barsDrawn = false;
    int r = g_dialog(d->desc);
    g_drawing = nullptr;
    if (r == 0 && d->confirm[7])
        r = 2;
    if (r == 0 && d->cancel[7])
        r = 1;
    return r;
}

namespace {
// Ends the field's edit (+0x1988 editing, +0x1989 on-screen keyboard) as the field itself
// would; the caret goes away. Returns the UI owner when the field is Find's, else null.
uint8_t* EndFieldEdit()
{
    auto root = static_cast<uint8_t*>(G());
    auto owner = root ? *reinterpret_cast<uint8_t**>(root + 0xd0) : nullptr;
    if (!g_findEditState || !owner || owner != g_findEditOwner || !*reinterpret_cast<void**>(owner))
        return nullptr;
    if (g_findEditState[0x1988]) {
        if (g_findEditState[0x1989] && g_closeKeyboard) {
            g_closeKeyboard(*reinterpret_cast<void**>(g_findEditState),
                *reinterpret_cast<uint32_t*>(g_findEditState + 8));
            g_findEditState[0x1989] = 0;
        }
        g_findEditState[0x1988] = 0;
    }
    return owner;
}
}

void DisableDialogField()
{
    __try { EndFieldEdit(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Native text editor (140d36070) publishes edit-active to UI owner +0x2fe70.
// Stopping dialog draws alone leaves that flag set after an external close.
void ReleaseDialogInput()
{
    __try {
        if (auto owner = EndFieldEdit(); owner && owner[0x2fe70]) {
            Log("dialog: releasing Find text input (shared flag %u)", owner[0x2fe70]);
            owner[0x2fe70] = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("dialog: text input cleanup unavailable"); }
    g_findEditState = nullptr;
    g_findEditOwner = nullptr;
    g_pending.set = false;
}

// Explicit bridge recovery for an already abandoned field in the backpack.
void RecoverDialogInput()
{
    __try {
        auto root = static_cast<uint8_t*>(G());
        auto owner = root ? *reinterpret_cast<uint8_t**>(root + 0xd0) : nullptr;
        if (owner && *reinterpret_cast<void**>(owner)) {
            Log("dialog: explicit recovery, shared text input flag %u", owner[0x2fe70]);
            owner[0x2fe70] = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("dialog: recovery unavailable"); }
}

const char* DialogText(Dialog* d, size_t* len)
{
    *len = d->text.len < sizeof(d->buffer) ? d->text.len : sizeof(d->buffer) - 1;
    return d->text.ptr;
}

void SetDialogText(Dialog* d, const std::string& text)
{
    size_t n = text.size() < sizeof(d->buffer) - 1 ? text.size() : sizeof(d->buffer) - 1;
    memcpy(d->buffer, text.data(), n);
    d->buffer[n] = 0;
    d->text.ptr = d->buffer;
    d->text.len = n;
    g_pending.text.assign(d->buffer, n);
    g_pending.set = true; // into the field's own edit state on the next draw
}

void SetDialogLines(Dialog* d, const std::vector<std::string>& lines)
{
    size_t n = lines.size() < 8 ? lines.size() : 8;
    for (size_t i = 0; i < n; ++i) {
        d->lineStore[i] = lines[i];
        d->lines[i] = MakeText(d->lineStore[i].c_str());
    }
    // The dialog's own list holds 4 inline; it is pointed at these 8 instead, and never needs
    // to grow (count <= capacity).
    *reinterpret_cast<Text**>(d->desc + 0xd0) = d->lines;
    *reinterpret_cast<uint64_t*>(d->desc + 0xd8) = n;
    *reinterpret_cast<uint64_t*>(d->desc + 0xe0) = 8;
}

void SetDialogTints(Dialog* d, const std::vector<uint32_t>& tints)
{
    for (size_t i = 0; i < 8; ++i)
        d->tints[i] = i < tints.size() ? tints[i] : 0;
}

uint32_t RarityColour(int rarity)
{
    // FbUiGlobalColors (assets + 0xd46c) starts with FbUiRarityColors: undefined, common,
    // uncommon, rare, epic, legendary (UiColor, 8 bytes each; the first 4 the colour).
    uint8_t* assets = Assets();
    if (!assets || rarity < 0 || rarity > 4)
        return 0;
    uint32_t colour = 0;
    hot::Read(assets + 0xd46c + 8 * (1 + rarity), &colour, sizeof(colour));
    return colour;
}

void SetDialogHighlights(Dialog* d, int selectedLine, int hoverLine, int shownLines)
{
    d->selectedLine = selectedLine;
    d->hoverLine = hoverLine;
    d->shownLines = shownLines;
}

void SetDialogScroll(Dialog* d, int first, int visible, int total)
{
    d->scrollFirst = first;
    d->scrollVisible = visible;
    d->scrollTotal = total;
}

namespace {
// A scroll bar right of the list when it holds more than it shows: a dim track and a gold thumb
// (FbUiPrimitives roundedBoxSmall), drawn with the button row's widget (images take absolute
// rects and are not clipped to their widget).
constexpr size_t kAssetRoundedBox = 0xd250 + 0x180;
// Bars behind the selected line (gold) and the line under the mouse (faint), over the width
// of the lines (the area between field and buttons, split evenly between the shown lines).
// Nine-slice boxes take their transparency from the colour's alpha byte (0xAABBGGRR); the float
// after it brightens (the menu's own highlight: 0xa6000000, 1.0).
uint64_t BoxColour(uint32_t rgb, float alpha)
{
    uint32_t a = static_cast<uint32_t>((std::min)(1.0f, (std::max)(0.0f, alpha)) * 255.0f + 0.5f);
    return Colour((a << 24) | (rgb & 0xffffffu), 1.0f);
}

void DrawLineBar(void* widget, const float area[4], int shown, int line, uint32_t colour, float alpha)
{
    uint8_t* assets = Assets();
    float row[4];
    if (line < 0 || line >= shown || !assets || !LineRow(area, shown, line, row))
        return;
    float bar[4] = {row[0] + 20.0f, row[1] + 2.0f, row[2] - 40.0f, row[3] - 4.0f};
    float b = (std::min)(bar[3] / 2, 12.0f);
    float borders[4] = {b, b, b, b};
    g_nineSlice(widget, bar, assets + kAssetRoundedBox, borders, BoxColour(colour, alpha));
}

void DrawHighlights(Dialog* d, void* widget)
{
    float area[4];
    if (!g_nineSlice || d->shownLines <= 0 || !DialogListArea(d, area))
        return;
    __try {
        for (int i = 0; i < d->shownLines && i < 8; ++i)
            if (d->tints[i])
                DrawLineBar(widget, area, d->shownLines, i, d->tints[i] & 0xffffffu, (d->tints[i] >> 24) / 255.0f);
        if (d->hoverLine != d->selectedLine)
            DrawLineBar(widget, area, d->shownLines, d->hoverLine, 0xffffffu, 0.10f);
        DrawLineBar(widget, area, d->shownLines, d->selectedLine, 0x3cc8ffu, 0.22f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        d->shownLines = 0;
    }
}

void DrawScroll(Dialog* d, void* widget)
{
    uint8_t* assets = Assets();
    float area[4];
    if (!assets || !g_nineSlice || d->scrollTotal <= d->scrollVisible || d->scrollVisible <= 0 ||
        !DialogListArea(d, area))
        return;
    float w = 6.0f, x = area[0] + area[2] + 14.0f, y = area[1] + 6.0f, h = area[3] - 12.0f;
    if (h < 20.0f)
        return;
    float track[4] = {x, y, w, h};
    float borders[4] = {w / 2, w / 2, w / 2, w / 2};
    float thumbH = (std::max)(h * d->scrollVisible / d->scrollTotal, 2 * w);
    float thumbY = y + (h - thumbH) * d->scrollFirst / (std::max)(1, d->scrollTotal - d->scrollVisible);
    float thumb[4] = {x, thumbY, w, thumbH};
    __try {
        g_nineSlice(widget, track, assets + kAssetRoundedBox, borders, BoxColour(0xffffffu, 0.15f));
        g_nineSlice(widget, thumb, assets + kAssetRoundedBox, borders, BoxColour(0x3cc8ffu, 0.85f));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        d->scrollTotal = 0;
    }
}
} // namespace

// The row of one shown line inside the list area. The game leaves more room above the first
// line and below the last than between lines; measured against the field's height (which scales
// with the UI the same way): pitch 0.73, above 0.475, below 0.25 of it. When those do not fit
// the area (another layout), the area is split evenly.
bool LineRow(const float area[4], int shown, int line, float out[4])
{
    if (shown <= 0 || line < 0 || line >= shown)
        return false;
    float fh = g_fieldRect[3];
    float above = 0.475f * fh, below = 0.25f * fh;
    float pitch = (area[3] - above - below) / shown;
    if (!(fh > 0.0f) || pitch < 0.4f * fh || pitch > 1.2f * fh) {
        above = 0.0f;
        pitch = area[3] / shown;
    }
    out[0] = area[0];
    out[1] = area[1] + above + pitch * line;
    out[2] = area[2];
    out[3] = pitch;
    return true;
}

int DialogLineAt(Dialog* d, int shown, float x, float y)
{
    float area[4], row[4];
    if (!DialogListArea(d, area) || x < area[0] || x >= area[0] + area[2])
        return -1;
    for (int i = 0; i < shown; ++i)
        if (LineRow(area, shown, i, row) && y >= row[1] && y < row[1] + row[3])
            return i;
    return -1;
}

bool DialogListArea(Dialog*, float out[4])
{
    float top = g_fieldRect[1] + g_fieldRect[3], bottom = g_rowRect[1];
    if (!(g_fieldRect[2] > 0 && g_rowRect[2] > 0 && bottom > top + 10.0f))
        return false;
    float left = g_rowRect[0], right = g_rowRect[0] + g_rowRect[2];
    out[0] = left;
    out[1] = top;
    out[2] = right - left;
    out[3] = bottom - top;
    return true;
}

bool CursorInUi(float& x, float& y)
{
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground)
        GetWindowThreadProcessId(foreground, &pid);
    if (pid != GetCurrentProcessId())
        return false;
    POINT cursor{};
    RECT client{};
    if (!GetCursorPos(&cursor) || !ScreenToClient(foreground, &cursor) || !GetClientRect(foreground, &client) ||
        client.right <= 0 || client.bottom <= 0)
        return false;
    // The UI is laid out 1080 units high, as wide as the window's aspect makes it (Auto Loot
    // measured root 3840 x 1080 at 5120 x 1440 pixels). No widget is begun here: this runs after
    // the game's UI frame, where a widget without a parent faults.
    float scale = 1080.0f / static_cast<float>(client.bottom);
    x = cursor.x * scale;
    y = cursor.y * scale;
    return true;
}

void SetDialogHint(Dialog* d, const std::string& hint)
{
    if (d->hintStore == hint)
        return;
    d->hintStore = hint;
    d->hint = MakeText(d->hintStore.c_str());
}

void ClearDialog(Dialog* d)
{
    SetDialogLines(d, {});
    d->text.len = 0;
    g_pending = {};
    g_drawing = nullptr;
    d->confirm[7] = 0;
    d->cancel[7] = 0;
}

} // namespace ui
