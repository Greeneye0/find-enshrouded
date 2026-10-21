// ECS component type names at runtime (build 1269579).
//
// Type indices are assigned at startup (Inventory is 283, Position 133 in the running game; the
// static component tables in the exe use another order). The registry keeps, somewhere, a
// table from index to the component's reflection descriptor, whose first two qwords are the
// short name and its length ("HighlightColor", 14). It is found once by looking through the
// registry's pointers for an array whose entries 283 and 133 name Inventory and Position.
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "ecsnames.h"
#include "log.h"

namespace ecs {
namespace {

constexpr int kTypes = 640;
uint8_t* g_table = nullptr; // entry i at g_table + i * g_stride, descriptor at + g_field (via g_deref)
size_t g_stride = 0;
size_t g_field = 0;
bool g_deref = false;
size_t g_derefField = 0;
bool g_tried = false;

bool InImage(uint64_t v)
{
    return v >= 0x140000000ull && v < 0x150000000ull;
}

// The descriptor's short name, if p looks like one.
bool DescName(uint64_t desc, char* out, size_t size)
{
    if (!InImage(desc))
        return false;
    __try {
        auto* d = reinterpret_cast<const uint64_t*>(desc);
        uint64_t namePtr = d[0], len = d[1];
        if (!InImage(namePtr) || len == 0 || len >= size || len > 80)
            return false;
        const char* s = reinterpret_cast<const char*>(namePtr);
        for (uint64_t i = 0; i < len; ++i)
            if (s[i] < 0x20 || s[i] > 0x7e)
                return false;
        memcpy(out, s, len);
        out[len] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t Read(const uint8_t* p)
{
    __try {
        return *reinterpret_cast<const uint64_t*>(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint64_t EntryDesc(uint8_t* table, size_t stride, size_t field, bool deref, size_t derefField, int index)
{
    uint64_t v = Read(table + index * stride + field);
    if (!deref)
        return v;
    if (v < 0x10000)
        return 0;
    return Read(reinterpret_cast<uint8_t*>(v) + derefField);
}

bool Matches(uint8_t* table, size_t stride, size_t field, bool deref, size_t derefField)
{
    char a[96], b[96];
    return DescName(EntryDesc(table, stride, field, deref, derefField, 283), a, sizeof(a)) && strcmp(a, "Inventory") == 0 &&
           DescName(EntryDesc(table, stride, field, deref, derefField, 133), b, sizeof(b)) && strcmp(b, "Position") == 0;
}

void Discover(void* registry)
{
    auto* reg = static_cast<uint8_t*>(registry);
    for (size_t off = 0; off < 0x4000; off += 8) {
        uint64_t p = Read(reg + off);
        if (p < 0x10000 || (p >> 47) != 0)
            continue;
        auto* table = reinterpret_cast<uint8_t*>(p);
        for (size_t stride = 8; stride <= 0x80; stride += 8) {
            for (size_t field = 0; field < stride; field += 8) {
                if (Matches(table, stride, field, false, 0)) {
                    g_table = table, g_stride = stride, g_field = field, g_deref = false;
                    Log("ecs: type table at registry+0x%zx, stride 0x%zx, descriptor at +0x%zx", off, stride, field);
                    return;
                }
                for (size_t df = 0; df <= 0x40; df += 8) {
                    if (Matches(table, stride, field, true, df)) {
                        g_table = table, g_stride = stride, g_field = field, g_deref = true, g_derefField = df;
                        Log("ecs: type table at registry+0x%zx, stride 0x%zx, entry +0x%zx -> descriptor at +0x%zx", off,
                            stride, field, df);
                        return;
                    }
                }
            }
        }
    }
    Log("ecs: component type names not found");
}

} // namespace

bool Name(void* registry, uint16_t type, char* out, size_t size)
{
    if (!g_tried) {
        g_tried = true;
        Discover(registry);
    }
    if (!g_table || type >= kTypes)
        return false;
    return DescName(EntryDesc(g_table, g_stride, g_field, g_deref, g_derefField, type), out, size);
}

int Find(void* registry, const char* name)
{
    char buf[96];
    for (int t = 0; t < kTypes; ++t)
        if (Name(registry, static_cast<uint16_t>(t), buf, sizeof(buf)) && strcmp(buf, name) == 0)
            return t;
    return -1;
}

} // namespace ecs
