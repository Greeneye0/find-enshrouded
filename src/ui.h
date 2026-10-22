#pragma once
#include <cstddef>
#include <cstdint>
#include <array>
#include <string>
#include <vector>

// The game's immediate-mode UI (build 1269579), as far as Find needs it.
namespace ui {

// The game's text type: utf8 pointer, length, fnv1a64 of the text.
struct Text {
    const char* utf8;
    size_t len;
    uint64_t hash;
};
Text MakeText(const char* utf8);

// Localized text by its key in the UI text tables (*(G + 200) + offset).
Text Loca(size_t offset);

// The localized text of a 16-byte text key (as ItemInfo + 0x48 holds an item's name).
std::string LocaKey(const uint8_t* key16);

bool Init();

void* G();      // the UI root (DAT_14281b498)
void* Client(); // *(**(G + 0xd0) + 0x38), or null outside a world

// --- world markers, drawn inside the HUD's spatial elements window ---

// Projects a world position (meters) to the screen. False if it is behind the camera.
bool Project(const float world[3], float& x, float& y, float* depth = nullptr);

// One marker in the style of the game's waypoint marker, centered on the screen point.
void DrawMarker(float x, float y, float alpha);
struct SolidRect { float x,y,w,h; };
void DrawSolids(const SolidRect* rects, size_t count, uint32_t colour, float alpha);

// A soft round glow (the game's circleSoftGlow primitive, or ringSoftGlow for a halo) of the
// given radius in pixels, centered on the screen point. colour: 0xAABBGGRR.
void DrawGlow(float x, float y, float radius, uint32_t colour, float alpha, bool ring = false);

// --- item icons that light up (the found items, in an open chest) ---

// The icon references (ItemInfo + 0x56c, 16 bytes) of the items to light up.
void SetIconTargets(const std::vector<std::array<uint8_t, 16>>& refs);
// The items (ids) whose inventory slots get a gold glow box while lighting is on.
void SetSlotTargets(const std::vector<uint32_t>& items);
// Lights those icons wherever they are drawn until called with alpha 0. colour: 0xAABBGGRR.
void LightIcons(uint32_t colour, float alpha);
// How many lit icons were drawn since the last call (for the log).
int TakeIconHits();
// How many of the wanted icons the game has looked up so far (for the log).
int IconIdsLearned();
// Logs what the item slot hook saw since the last call.
void LogSlotDiagnostics();
// The distinct item ids slots showed since the last call (diagnostics).
std::vector<uint32_t> TakeSlotIds();
// Records which code draws images until called with false, then logs it.
void TraceDraws(bool on);

// --- the game's text input dialog (the one used to name chests and enter passwords) ---

struct Dialog;
// title: shown above the field; confirm: the label of the confirm button.
Dialog* NewDialog(const char* title, const char* confirm);
// Draws the dialog for this frame. Returns 0 while open, 1 when cancelled, 2 when confirmed.
int DrawDialog(Dialog* d);
void ReleaseDialogInput();
// Ends the field's edit for this frame (no caret, typing goes nowhere); call after DrawDialog.
void DisableDialogField();
void RecoverDialogInput();
// The text typed so far.
const char* DialogText(Dialog* d, size_t* len);
// Replaces the text in the field.
void SetDialogText(Dialog* d, const std::string& text);
// Lines of text shown between the field and the buttons (at most 8; empty: none).
void SetDialogLines(Dialog* d, const std::vector<std::string>& lines);
// A small, dim line under the buttons (key hints). Empty: none.
void SetDialogHint(Dialog* d, const std::string& hint);
// The area between the field and the buttons where the lines are shown, in UI units
// {x, y, w, h}, as laid out in the last frame. False until known.
bool DialogListArea(Dialog* d, float out[4]);
// Which of the shown lines (0-based) the UI point is on, or -1.
int DialogLineAt(Dialog* d, int shown, float x, float y);
// The row {x, y, w, h} of one shown line inside the list area.
bool LineRow(const float area[4], int shown, int line, float out[4]);
// Shows a scroll bar beside the lines: total entries, the first shown, how many are shown.
void SetDialogScroll(Dialog* d, int first, int visible, int total);
// Bars behind the lines: the selected one (gold) and the one under the mouse. Line numbers
// count the shown lines from 0; -1: none.
void SetDialogHighlights(Dialog* d, int selectedLine, int hoverLine, int shownLines);
// A tinted bar behind each shown line (0xAABBGGRR, the alpha byte its strength; 0: none).
void SetDialogTints(Dialog* d, const std::vector<uint32_t>& tints);
// The game's colour for an item rarity (0 common .. 4 legendary), 0xAABBGGRR; 0 if unknown.
uint32_t RarityColour(int rarity);
// The mouse cursor in UI units. False when the game is not in the foreground.
bool CursorInUi(float& x, float& y);
void ClearDialog(Dialog* d);

} // namespace ui
