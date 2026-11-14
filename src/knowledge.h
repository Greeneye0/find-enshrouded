#pragma once
#include <cstdint>

// What the player has unlocked: the game keeps the player's knowledge as plain arrays of ids, and
// an unlocked recipe's id is among them (an NPC's crafting menu shows exactly those recipes).
namespace knowledge {

// Reads the player's knowledge again (cheap once found). isRecipe tells recipe ids, which is how
// the arrays are recognized. False if none is found; the game may not have received it yet.
bool Refresh(bool (*isRecipe)(uint32_t), bool full = false);
bool Ready();
bool Known(uint32_t id);
// Known, or else known after a full scan (at most one every 30 seconds): the arrays found earlier may be a copy the
// game left behind when the list grew.
bool KnownAfterRescan(uint32_t id, bool (*isRecipe)(uint32_t));

} // namespace knowledge
