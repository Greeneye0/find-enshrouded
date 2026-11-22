#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Find on the Skills page (HUD screen SkillTree): the skill tree's nodes with their names and
// descriptions in the game's language, a gold pulse on found nodes and centring the view.
namespace skills {

// keen::HudScreenType SkillTree (10 in the build of 2026-11-04) and SkillTreeRespec (11, the same page while
// skills are being reset): read from the exe's reflection table in Init, so a patch renumbering them can't break it.
constexpr int kScreen = 10;
bool IsScreen(int screen);
bool IsReskillScreen(int screen); // SkillTreeRespec
// The skill tree is showing without a hint bar of its own: Reskill.
bool NoOwnHintBar(int screen);
// Runs draw inside a UI layer of Find's own (the game's widget calls need an open layer); false without one.
// With a size, the layer's root is placed at the top left with that size (full-screen UI units), for aligned children.
bool InOwnLayer(const char* name, void (*draw)(void*), void* arg, float width = 0, float height = 0);

// One skill as the search lists it: every node of that name (attribute nodes repeat).
struct Skill {
    std::string name;  // as the game shows it
    std::string desc;  // its description, markup removed
    std::string lowerName, lowerDesc; // filled by the caller
    std::vector<uint32_t> nodes; // node ids
};

// The tree's class labels (Warrior, Guardian, Battlemage...), each with the nodes in its part of
// the tree and their middle (canvas units).
struct Category {
    std::string name;
    std::vector<uint32_t> nodes;
    float centre[2] = {};
};

bool Init();
std::vector<Category> Categories();

// The skills of the open skill tree, grouped by name. Empty (with why) if the page or its data
// can't be read.
std::vector<Skill> List(std::string& why);

// Gold pulse on these nodes for ms milliseconds; with exactly one node the view glides to it,
// otherwise to centre (canvas units) if given.
void Highlight(const std::vector<uint32_t>& nodes, unsigned ms, const float* centre = nullptr);
void Clear();

// Each frame (game thread): draws the pulse while the Skills page is open, on a layer of its
// own unless the page draws it (UsePageDrawing).
void Draw(int screen);
// The page's hint bar call is Find's: DrawInPage, called inside the page, draws this frame's pulse
// at the nodes' depth (under the card of the node under the cursor).
void UsePageDrawing(bool on);
void DrawInPage();

#ifdef FIND_DEV
void DevDump();
#endif

} // namespace skills
