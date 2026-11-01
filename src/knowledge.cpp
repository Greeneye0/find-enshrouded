#include "knowledge.h"
// Player knowledge reader.
//
// The client keeps the knowledge ids the server sent it (KnowledgeServerMessage knowledgeIds) as
// u32 arrays in the game's fixed-address pool at 0x10000000000 (build 1273043): a run of nonzero
// ids between zeros, about 1,600 for a mid-game player, a few copies of it. No pointer leads to
// them from outside the pool's own block headers, so they are found by content: long runs whose
// first entries include recipe ids. Knowledge only grows, so the union of the copies is current.
#include <windows.h>
#include <unordered_set>
#include <vector>
#include "log.h"

namespace knowledge {
namespace {

constexpr uintptr_t kPool = 0x10000000000;
constexpr size_t kMinRun = 256;       // ids in a knowledge array, at least
constexpr size_t kMaxRun = 200000;
constexpr size_t kHead = 128;         // checked for recipe ids...
constexpr int kMinRecipesInHead = 8;  // ...of which a knowledge array has about 30
constexpr ULONGLONG kRescanMs = 60000; // full scans when none is found, at most this often

std::vector<uintptr_t> g_runs; // where the arrays were found
std::unordered_set<uint32_t> g_known;
bool g_ready = false;
ULONGLONG g_lastScan = 0;

// The run of nonzero ids at p, if it looks like knowledge: its length, else 0.
size_t RunAt(const uint32_t* p, const uint32_t* end, bool (*isRecipe)(uint32_t))
{
    size_t n = 0;
    while (p + n < end && p[n] && n < kMaxRun)
        ++n;
    if (n < kMinRun || n >= kMaxRun)
        return 0;
    int recipes = 0;
    for (size_t i = 0; i < kHead; ++i)
        recipes += isRecipe(p[i]) ? 1 : 0;
    return recipes >= kMinRecipesInHead ? n : 0;
}

// One committed region: appends the start of every knowledge run.
void ScanRegion(const uint32_t* lo, const uint32_t* hi, bool (*isRecipe)(uint32_t), std::vector<uintptr_t>& out)
{
    __try {
        const uint32_t* p = lo;
        while (p < hi) {
            if (!*p) {
                ++p;
                continue;
            }
            const uint32_t* s = p;
            while (p < hi && *p)
                ++p;
            if (static_cast<size_t>(p - s) >= kMinRun && (s == lo || !s[-1]) && RunAt(s, p, isRecipe))
                out.push_back(reinterpret_cast<uintptr_t>(s));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ScanPool(bool (*isRecipe)(uint32_t), std::vector<uintptr_t>& out)
{
    MEMORY_BASIC_INFORMATION mbi{};
    auto* base = reinterpret_cast<const uint8_t*>(kPool);
    for (const uint8_t* at = base; VirtualQuery(at, &mbi, sizeof(mbi)) && mbi.AllocationBase == base;
         at = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE)))
            continue;
        auto* lo = static_cast<const uint32_t*>(mbi.BaseAddress);
        ScanRegion(lo, lo + mbi.RegionSize / 4, isRecipe, out);
    }
}

// Adds the run at start if it is still knowledge; false if it no longer is.
bool Take(uintptr_t start, bool (*isRecipe)(uint32_t))
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(start), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE)) || (mbi.Protect & PAGE_GUARD))
        return false;
    auto* p = reinterpret_cast<const uint32_t*>(start);
    auto* end = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize);
    __try {
        size_t n = RunAt(p, end, isRecipe);
        if (!n)
            return false;
        for (size_t i = 0; i < n; ++i)
            g_known.insert(p[i]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool Refresh(bool (*isRecipe)(uint32_t))
{
    g_known.clear();
    std::vector<uintptr_t> kept;
    for (uintptr_t r : g_runs)
        if (Take(r, isRecipe))
            kept.push_back(r);
    if (kept.empty() && (!g_lastScan || GetTickCount64() - g_lastScan >= kRescanMs)) {
        ULONGLONG start = GetTickCount64();
        g_lastScan = start;
        std::vector<uintptr_t> found;
        ScanPool(isRecipe, found);
        for (uintptr_t r : found)
            if (Take(r, isRecipe))
                kept.push_back(r);
        Log("knowledge: %zu arrays, %zu ids (%llu ms)", kept.size(), g_known.size(), GetTickCount64() - start);
    }
    g_runs = kept;
    g_ready = !g_runs.empty();
    return g_ready;
}

bool Ready() { return g_ready; }

bool Known(uint32_t id) { return g_known.count(id) != 0; }

} // namespace knowledge
