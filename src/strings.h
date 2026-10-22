#pragma once
#include <cstdint>
#include <string>

// Find's own texts in the game's language. Names of items, perks, item types, categories and
// rarities come from the game itself; these are the few texts the game has no words for.
namespace strings {

enum Id {
    Title,          // the dialog's title and its confirm button: "Find"
    TypePrompt,     // shown before anything is typed
    NotInBase,      // outside a base
    NoMatch,        // %s: what was typed
    Hint,           // key hints under the buttons
    HintClose,      // the same outside a base
    KindCategory,   // labels before a kind
    KindRarity,
    KindPerk,
    KindItemType,
    KindChest,
    Instruments,    // categories the game has no category text for
    FishingRods,
    AnimalFood,
    WeaponGems,
    NoChests,       // inside a base without storage chests
    Count
};

const char* Get(Id id);

// "996 in 1 chest", "12 items in 3 chests": counts with the right plural forms.
std::string ItemCount(uint32_t amount, int chests);
std::string KindCount(uint32_t items, int chests);

// Picks the language from the game's own "Cancel" text. Call once the UI texts are loaded.
void Detect();
const char* Language();

} // namespace strings
