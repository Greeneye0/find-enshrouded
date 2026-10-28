#include "recipes.h"
// Recipe registry reader.
//
// The game keeps its resources in one large allocation, written as stored: arrays and strings are
// {int32 offset from the field, uint32 count}. keen::RecipeRegistryResource starts with its
// recipes array; each RecipeInfo (0xa8 bytes, build 1273043) holds
//   + 0x24 workshop id (0: your own crafting menu; an NPC's trade or a station otherwise),
//   + 0x28 inputs  {offset, count}, ItemStack 0x30 bytes each,
//   + 0x30 outputs {offset, count}, ItemStack: item GUID, then + 0x10 the item id, + 0x14 amount,
//   + 0x78 debug name {offset, length}.
// Nothing points at the registry from outside the allocation (the resource system addresses it by
// offset), so it is found by its shape: an array header whose records all look like that.
#include <windows.h>
#include <algorithm>
#include <unordered_map>
#include "log.h"

namespace recipes {
namespace {

constexpr size_t kRecipeSize = 0xa8;
constexpr size_t kWorkshop = 0x24;
constexpr size_t kInputs = 0x28;
constexpr size_t kOutputs = 0x30;
constexpr size_t kDebugName = 0x78;
constexpr size_t kStackSize = 0x30;
constexpr size_t kStackItem = 0x10;

std::unordered_map<uint32_t, std::vector<uint32_t>> g_madeBy; // item -> workshops
std::unordered_map<uint32_t, std::vector<uint32_t>> g_makes;  // workshop -> items
bool g_loaded = false;

struct Span {
    const uint8_t* lo;
    const uint8_t* hi;
    bool Has(const uint8_t* p, size_t n) const { return p >= lo && p <= hi && static_cast<size_t>(hi - p) >= n; }
};

// {offset, count} at field, resolved; null if it leaves the allocation.
const uint8_t* Array(const Span& s, const uint8_t* field, size_t elementSize, uint32_t& count)
{
    int32_t rel = *reinterpret_cast<const int32_t*>(field);
    count = *reinterpret_cast<const uint32_t*>(field + 4);
    if (!count)
        return field;
    const uint8_t* at = field + rel;
    return s.Has(at, static_cast<size_t>(count) * elementSize) ? at : nullptr;
}

bool LooksLikeRecipe(const Span& s, const uint8_t* r)
{
    uint32_t inputs, outputs, nameLen;
    if (!Array(s, r + kInputs, kStackSize, inputs) || inputs > 32)
        return false;
    const uint8_t* out = Array(s, r + kOutputs, kStackSize, outputs); // none: a flame altar upgrade
    if (!out || outputs > 8)
        return false;
    const uint8_t* name = Array(s, r + kDebugName, 1, nameLen);
    if (!name || nameLen == 0 || nameLen > 255)
        return false;
    for (uint32_t i = 0; i < nameLen; ++i)
        if (name[i] < 0x20 || name[i] > 0x7e)
            return false;
    return outputs == 0 || *reinterpret_cast<const uint32_t*>(out + kStackItem) != 0;
}

// A recipes array at header: every record is a recipe. Adds them; returns how many.
int TakeRegistry(const Span& s, const uint8_t* header)
{
    uint32_t count;
    const uint8_t* records = Array(s, header, kRecipeSize, count);
    if (!records || count < 64 || count > 200000 || records <= header)
        return 0;
    // Quick checks first (most candidates fail the first record), then all of them.
    if (!LooksLikeRecipe(s, records) || !LooksLikeRecipe(s, records + (count - 1) * kRecipeSize))
        return 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!LooksLikeRecipe(s, records + i * kRecipeSize))
            return 0;
    int taken = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* r = records + i * kRecipeSize;
        uint32_t workshop = *reinterpret_cast<const uint32_t*>(r + kWorkshop);
        if (!workshop)
            continue;
        uint32_t outputs;
        const uint8_t* out = Array(s, r + kOutputs, kStackSize, outputs);
        for (uint32_t o = 0; o < outputs; ++o) {
            uint32_t item = *reinterpret_cast<const uint32_t*>(out + o * kStackSize + kStackItem);
            auto& by = g_madeBy[item];
            if (std::find(by.begin(), by.end(), workshop) == by.end())
                by.push_back(workshop);
            auto& makes = g_makes[workshop];
            if (std::find(makes.begin(), makes.end(), item) == makes.end())
                makes.push_back(item);
        }
        ++taken;
    }
    return taken;
}

int ScanRegion(const Span& s)
{
    int found = 0;
    __try {
        for (const uint8_t* p = s.lo; p + 8 <= s.hi; p += 4) {
            // header: positive offset to an 8-aligned array of at least 64 records
            int32_t rel = *reinterpret_cast<const int32_t*>(p);
            if (rel <= 0 || (rel & 7) || (reinterpret_cast<uintptr_t>(p + rel) & 7))
                continue;
            uint32_t count = *reinterpret_cast<const uint32_t*>(p + 4);
            if (count < 64 || count > 200000)
                continue;
            if (int n = TakeRegistry(s, p)) {
                Log("recipes: registry at %p, %u recipes, %d made at an NPC or station", p, count, n);
                found += n;
                p += 4;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("recipes: scan stopped by an unreadable page");
    }
    return found;
}

} // namespace

bool Load(const void* anyGameData)
{
    if (g_loaded)
        return true;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!anyGameData || !VirtualQuery(anyGameData, &mbi, sizeof(mbi)) || !mbi.AllocationBase)
        return false;
    ULONGLONG start = GetTickCount64();
    auto* base = static_cast<const uint8_t*>(mbi.AllocationBase);
    size_t total = 0;
    int found = 0;
    // Each committed, readable region of the allocation (the game data is one allocation).
    for (const uint8_t* at = base; VirtualQuery(at, &mbi, sizeof(mbi)) && mbi.AllocationBase == base;
         at = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
        bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
        if (!readable)
            continue;
        Span s{static_cast<const uint8_t*>(mbi.BaseAddress), static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize};
        total += mbi.RegionSize;
        found += ScanRegion(s);
    }
    Log("recipes: %d recipes for %zu items at %zu workshops (%zu MB of game data, %llu ms)", found, g_madeBy.size(),
        g_makes.size(), total >> 20, GetTickCount64() - start);
    g_loaded = found > 0;
    return g_loaded;
}

bool Loaded() { return g_loaded; }

const std::vector<uint32_t>* MadeBy(uint32_t itemId)
{
    auto it = g_madeBy.find(itemId);
    return it == g_madeBy.end() ? nullptr : &it->second;
}

const std::vector<uint32_t>* Makes(uint32_t workshop)
{
    auto it = g_makes.find(workshop);
    return it == g_makes.end() ? nullptr : &it->second;
}

bool IsWorkshop(uint32_t workshop) { return g_makes.count(workshop) != 0; }

} // namespace recipes
