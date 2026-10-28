#pragma once
#include <cstdint>

// Read-only view of the client's inventories: your backpack and the base's chests.
//
// An inventory owner (player, chest) has an InventorySetup component that links up to 16
// "page" entities, each with an Inventory component of 8 ItemStacks. Items are described by
// ItemInfo records from the client's item database.
namespace inv {

constexpr int kSlotsPerPage = 8;
constexpr int kMaxPages = 16;

// InventoryCategory of a page link
enum class PageKind : uint8_t { Invalid = 0, Customization = 1, Equipment = 2, Currency = 3, Generic = 4, Virtual = 5 };

struct Stack {
    uint32_t itemId;
    uint32_t count;
    uint32_t pide;
};

struct Page {
    uint32_t entity;
    PageKind kind;
    Stack slots[kSlotsPerPage];
};

struct Owner {
    uint32_t entity;
    int pageCount;
    Page pages[kMaxPages + 1];
};

struct ItemDesc {
    bool known;
    uint8_t category;       // keen::ItemCategory
    uint8_t weaponType[16]; // ItemInfo.weaponCategoryReference, weapons only
    uint8_t ammoType;       // ItemInfo.ammunitionType: 1 arrow, 2 spell, 3 fishing bait
    uint16_t maxStack;
    char name[64];  // readable: "Wand Epic Fire"
    char raw[96];   // internal: "Weapon_T1_1H_WandEpic_01_Fire"
};

// Hooks ViewInit (to learn the client's ECS registry) and the item lookup (to learn the item
// database). Call before the game's code runs.
bool Init();

// True once the client's registry and the InventorySetup type are known. Picks the registry
// that knows the local player and one of the probes (the base's chests), else the one holding
// the player's own inventories.
bool Ready(uint32_t playerId, const uint32_t* probes, int probeCount);

// Forgets the registries when the client's world changes (you left it, or joined another), so
// the next world's are learned and identified afresh.
void ForgetOnWorldChange(void* world);

// Reads an inventory owner. Returns false if the client doesn't know the entity.
bool ReadOwner(uint32_t entity, Owner& out);

// True for the player's hotbar pages, which vanilla "Deposit Stacks" never touches either.
// The game keeps their ids at +0xa0/+0xa4 of one player component; that component type is
// detected on first use. When it can't be found, every page counts as hotbar (safe side).
bool IsHotbarPage(const Owner& player, uint32_t pageEntity);

// Every container (entity with an inventory setup, not a player) whose position lies inside
// the box. Walks the client registry's entity table once. Returns how many were written.
int FindContainers(const float lo[3], const float hi[3], uint32_t* out, int max);

// Storage chests inside the box: every inventory there whose pages are all storage. For a base
// that lists no crafting-storage chests (a small flame altar). Needs Ready() first.
int FindPlainChests(const float lo[3], const float hi[3], uint32_t* out, int max);

// World position of an entity in meters. Returns false if unknown.
bool Position(uint32_t entity, float out[3]);

// The name a player gave the entity in game (CustomPlayerString, resolved through the
// client's custom string store at client + 0xf0). Returns false if it has none.
bool CustomName(void* client, uint32_t entity, char* out, size_t size);

// Crafting stations inside the box (seedbed, kiln, loom and the like) that are not among the chests:
// inventories of two or more storage pages. Bushes (one page), NPCs and mannequins stay out. Needs
// Ready() first. Returns how many were written.
int FindStations(const float lo[3], const float hi[3], const uint32_t* chests, int chestCount, uint32_t* out, int max);

// Keeps only storage chests among candidates: entities with every component type that all
// the base's magic (crafting-storage) chests share, except the magic-only ones. Crafting
// stations and similar inventories drop out. Returns the new count.
int KeepStorageChests(const uint32_t* magic, int magicCount, uint32_t* candidates, int n);
// Drops plants from a list of containers. The flame altar's crafting stock lists planted bushes
// too, since their produce counts toward crafting (a hazelnut bush holds its nuts in one page
// of its own). Chests can be renamed, so they have CustomPlayerString and plants don't; once a
// named chest has shown which type that is, it decides. Until then a single page marks a plant.
// (Component type numbers change between game sessions.) Needs Ready() first. Returns the new
// count.
int DropPlants(void* client, uint32_t* ids, int n);

ItemDesc Describe(uint32_t itemId);

// The item's ItemInfo record (name key at +0x48), or null if the item databases don't know it.
const uint8_t* ItemInfo(uint32_t itemId);

// --- inspection, for the dev bridge ---

// The client registry, also outside a base: the one identified by Ready() or, before that,
// the registry that knows playerId and holds the most entities.
void* InspectRegistry(uint32_t playerId);

// Every ECS registry seen so far (the client has several); returns how many were written.
int Registries(void** out, int max);

// Component type indices of an entity (-1: unknown entity).
int Types(void* registry, uint32_t entity, uint16_t* out, int max);

// Raw component data, or null.
void* Data(void* registry, uint16_t type, uint32_t entity);

// Non-player entities with a position within radius meters of center, nearest first.
int EntitiesNear(void* registry, const float center[3], float radius, uint32_t* out, float* dist, int max);

const char* CategoryName(uint8_t category);

// NPCs and crafting stations in the box and the workshop each crafts at (isWorkshop: a workshop
// the recipes know).
struct Crafter {
    uint32_t entity;
    uint32_t workshop;
};
int FindCrafters(const float lo[3], const float hi[3], bool (*isWorkshop)(uint32_t), Crafter* out, int max);

#ifdef FIND_DEV
struct Hit { uint32_t entity; uint16_t type; uint16_t offset; };
int ScanValue(void* registry, uint32_t value, Hit* out, int max);
#endif

} // namespace inv
