#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Find on the Skills page (HUD screen SkillTree): the skill tree's nodes with their names and
// descriptions in the game's language, a gold pulse on found nodes and centring the view.
namespace skills {

constexpr int kScreen = 10; // keen::HudScreenType SkillTree

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
