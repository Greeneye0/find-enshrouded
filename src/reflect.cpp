#include "reflect.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include "log.h"
#include "scan.h"

namespace reflect {
namespace {

std::unordered_map<std::string_view, const uint8_t*> g_types; // full name -> descriptor
uintptr_t g_lo = 0, g_hi = 0;
bool g_ready = false;
Stats g_stats{};

template <class T> T At(const uint8_t* p, size_t off)
{
    T v;
    memcpy(&v, p + off, sizeof(v));
    return v;
}

bool InImage(uint64_t p, uint64_t n) { return p >= g_lo && n < 0x100000 && p + n + 1 <= g_hi; }

// A name in the image of this length, null-terminated, printable.
bool Text(uint64_t p, uint64_t n, std::string_view& out)
{
    if (!n || n >= 300 || !InImage(p, n))
        return false;
    auto* s = reinterpret_cast<const char*>(p);
    if (s[n] != 0)
        return false;
    for (uint64_t i = 0; i < n; ++i)
        if (static_cast<unsigned char>(s[i]) < 32 || static_cast<unsigned char>(s[i]) >= 127)
            return false;
    out = std::string_view(s, n);
    return true;
}

// The member of a descriptor with this name: its offset and type descriptor.
bool Member(const uint8_t* desc, std::string_view name, uint64_t& offset, const uint8_t*& type)
{
    const uint32_t kind = At<uint32_t>(desc, 0x4c) & 0xff;
    if (kind == 0x0c || kind == 0x0d) // enums
        return false;
    const uint32_t count = At<uint32_t>(desc, 0x48);
    const uint64_t members = At<uint64_t>(desc, 0x58);
    if (!count || count > 400 || !InImage(members, 0x30ull * count))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto* m = reinterpret_cast<const uint8_t*>(members) + 0x30ull * i;
        std::string_view n;
        if (!Text(At<uint64_t>(m, 0), At<uint64_t>(m, 8), n) || n != name)
            continue;
        offset = At<uint64_t>(m, 0x18);
        const uint64_t t = At<uint64_t>(m, 0x10);
        type = InImage(t, 0x60) ? reinterpret_cast<const uint8_t*>(t) : nullptr;
        return offset < 0x100000;
    }
    return false;
}

const uint8_t* Find(const char* type)
{
    if (!g_ready)
        return nullptr;
    auto it = g_types.find(type);
    return it == g_types.end() ? nullptr : it->second;
}

} // namespace

bool Init()
{
    if (g_ready)
        return true;
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    g_lo = reinterpret_cast<uintptr_t>(base);
    g_hi = g_lo + nt->OptionalHeader.SizeOfImage;
    const ULONGLONG start = GetTickCount64();
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (memcmp(sec->Name, ".rdata", 6) != 0 && memcmp(sec->Name, ".data\0", 6) != 0)
            continue;
        const uint8_t* p = base + sec->VirtualAddress;
        const size_t size = (std::min)(static_cast<size_t>(sec->Misc.VirtualSize), static_cast<size_t>(sec->SizeOfRawData));
        for (size_t off = 0; off + 0x60 <= size; off += 8) {
            const uint64_t fullLen = At<uint64_t>(p, off + 0x28);
            if (fullLen <= 8 || fullLen >= 300)
                continue;
            std::string_view full;
            if (!Text(At<uint64_t>(p, off + 0x20), fullLen, full) || full.compare(0, 6, "keen::") != 0)
                continue;
            std::string_view name;
            if (!Text(At<uint64_t>(p, off), At<uint64_t>(p, off + 8), name))
                continue;
            g_types.emplace(full, p + off); // the first one; duplicates are the same type
        }
    }
    g_ready = !g_types.empty();
    Log("reflect: %zu game types read in %llu ms", g_types.size(), GetTickCount64() - start);
    return g_ready;
}

size_t Offset(const char* type, const char* field, size_t fallback)
{
    const uint8_t* desc = Find(type);
    uint64_t total = 0;
    bool ok = desc != nullptr;
    for (const char* f = field; ok && *f;) {
        const char* dot = strchr(f, '.');
        std::string_view part(f, dot ? static_cast<size_t>(dot - f) : strlen(f));
        uint64_t off = 0;
        const uint8_t* next = nullptr;
        ok = Member(desc, part, off, next);
        total += off;
        desc = next;
        f = dot ? dot + 1 : f + part.size();
        if (dot && !desc)
            ok = false;
    }
    if (!ok) {
        ++g_stats.missing;
        Log("reflect: %s.%s not in the game's types, using 0x%zx", type, field, fallback);
        return fallback;
    }
    ++g_stats.found;
    if (total != fallback) {
        ++g_stats.moved;
        Log("reflect: %s.%s is at 0x%llx in this build (was 0x%zx)", type, field, total, fallback);
    }
    return static_cast<size_t>(total);
}

size_t Size(const char* type, size_t fallback)
{
    const uint8_t* desc = Find(type);
    if (!desc) {
        ++g_stats.missing;
        Log("reflect: type %s not in the game, size 0x%zx assumed", type, fallback);
        return fallback;
    }
    ++g_stats.found;
    const size_t size = At<uint32_t>(desc, 0x40);
    if (size != fallback) {
        ++g_stats.moved;
        Log("reflect: %s is 0x%zx bytes in this build (was 0x%zx)", type, size, fallback);
    }
    return size;
}

size_t FromCode(const char* name, const char* pattern1, int index1, const char* pattern2, int index2, size_t fallback)
{
    int32_t got[2] = {-1, -1};
    const char* patterns[2] = {pattern1, pattern2};
    const int index[2] = {index1, index2};
    for (int i = 0; i < 2; ++i)
        if (uint8_t* at = FindUnique(patterns[i]))
            memcpy(&got[i], at + index[i], sizeof(got[i]));
    if (got[0] < 0 || got[1] < 0 || got[0] != got[1]) {
        ++g_stats.missing;
        Log("reflect: %s not read from the code (0x%x / 0x%x), using 0x%zx", name, got[0], got[1], fallback);
        return fallback;
    }
    ++g_stats.found;
    if (static_cast<size_t>(got[0]) != fallback) {
        ++g_stats.moved;
        Log("reflect: %s is at 0x%x in this build (was 0x%zx)", name, got[0], fallback);
    }
    return static_cast<size_t>(got[0]);
}

Stats GetStats() { return g_stats; }

} // namespace reflect
