#pragma once
#include <cstdint>
#include <vector>

// Which workshop (an NPC's trade or a crafting station) makes an item, from the game's own recipe
// registry: read once from the loaded game data, then kept for the session.
namespace recipes {

// Finds the recipe registry in the game data that holds anyGameData (an ItemInfo record will do).
// Returns false if it isn't there; Load can be called again later.
bool Load(const void* anyGameData);
bool Loaded();

// Workshops (0 never: that is your own crafting menu) whose recipes make itemId.
const std::vector<uint32_t>* MadeBy(uint32_t itemId);

// The items a workshop's recipes make.
const std::vector<uint32_t>* Makes(uint32_t workshop);

bool IsWorkshop(uint32_t workshop);

// Any recipe's id (also those of your own crafting menu).
bool IsRecipe(uint32_t id);

// The recipes of a workshop that make itemId (their recipe ids).
const std::vector<uint32_t>* RecipeIds(uint32_t workshop, uint32_t itemId);

} // namespace recipes
