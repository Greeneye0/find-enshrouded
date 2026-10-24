#include "hotapi.h"
#include <windows.h>
#include <cstring>
#include <vector>
#include "scan.h"
#include "jumppatch.h"
#include "log.h"

static bool TextSection(void* module, uint8_t*& start, size_t& size)
{
    auto base = static_cast<uint8_t*>(module ? module : GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            start = base + sec->VirtualAddress;
            size = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

static std::vector<int> ParsePattern(const char* pattern)
{
    std::vector<int> bytes; // -1 = wildcard
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') { bytes.push_back(-1); p += (p[1] == '?') ? 2 : 1; continue; }
        bytes.push_back(static_cast<int>(strtoul(p, nullptr, 16)));
        p += 2;
    }
    return bytes;
}

// Calls onMatch for every match; stops early when it returns false.
template <class F> static bool ForEachMatch(const char* pattern, void* module, F onMatch)
{
    std::vector<int> bytes = ParsePattern(pattern);
    uint8_t* text;
    size_t size;
    if (bytes.empty() || bytes[0] < 0 || !TextSection(module, text, size))
        return false;
    const uint8_t first = static_cast<uint8_t>(bytes[0]);
    const size_t n = bytes.size();
    for (uint8_t* p = text; p + n <= text + size; ++p) {
        p = static_cast<uint8_t*>(memchr(p, first, (text + size) - p - n + 1));
        if (!p)
            break;
        size_t i = 1;
        while (i < n && (bytes[i] < 0 || p[i] == bytes[i]))
            ++i;
        if (i == n && !onMatch(p))
            return true;
    }
    return true;
}

// A function whose first instructions another mod's hook replaced (see jumppatch.h).
static uint8_t* FindBehindHook(const char* pattern, void* module)
{
    std::vector<int> bytes = ParsePattern(pattern);
    uint8_t* text;
    size_t size;
    if (bytes.empty() || !TextSection(module, text, size))
        return nullptr;
    uint8_t* p = jumppatch::FindBehindHook(text, size, bytes.data(), bytes.size());
    if (p)
        Log("found a function behind another mod's hook at %p", p);
    return p;
}

uint8_t* FindUnique(const char* pattern, void* module)
{
    if (hot::host) return hot::host->find(pattern, module);
    uint8_t* found = nullptr;
    bool unique = true;
    ForEachMatch(pattern, module, [&](uint8_t* p) {
        if (found) {
            unique = false;
            return false;
        }
        found = p;
        return true;
    });
    if (!found)
        found = FindBehindHook(pattern, module);
    return unique ? found : nullptr;
}

uint8_t* ResolveRipConsensus(const char* pattern, int dispOffset, int insEnd)
{
    uint8_t* target = nullptr;
    bool agree = true;
    ForEachMatch(pattern, nullptr, [&](uint8_t* p) {
        int32_t disp;
        memcpy(&disp, p + dispOffset, 4);
        uint8_t* t = p + insEnd + disp;
        if (target && t != target) {
            agree = false;
            return false;
        }
        target = t;
        return true;
    });
    return agree ? target : nullptr;
}

uint8_t* ResolveLea(uint8_t* addr)
{
    if ((addr[0] != 0x48 && addr[0] != 0x4C) || addr[1] != 0x8D || (addr[2] & 0xC7) != 0x05)
        return nullptr;
    int32_t disp;
    memcpy(&disp, addr + 3, 4);
    return addr + 7 + disp;
}

static void WriteAbsJump(uint8_t* at, const void* to)
{
    at[0] = 0x48; at[1] = 0xB8; // mov rax, imm64
    memcpy(at + 2, &to, 8);
    at[10] = 0xFF; at[11] = 0xE0; // jmp rax
}

// Jump that leaves every register alone (14 bytes), for the trampoline's way back: the copied
// prologue may already have set rax (e.g. "mov rax, rsp" before "lea rbp, [rax-x]").
static void WriteAbsJumpNoRegs(uint8_t* at, const void* to)
{
    at[0] = 0xFF; at[1] = 0x25; // jmp qword ptr [rip+0]
    memset(at + 2, 0, 4);
    memcpy(at + 6, &to, 8);
}

void* InstallHook(uint8_t* target, void* detour, size_t length)
{
    return InstallHook(target, detour, length, target, length);
}

void* InstallHook(uint8_t* target, void* detour, size_t length, const uint8_t* prologue, size_t prologueLength)
{
    if (hot::host) { auto result=hot::host->hook(target, detour, length, prologue, prologueLength); if(!result) Log("hot: rejected custom hook %p length %zu",target,length); return result; }
    if (length < 12 || length > 32 || prologueLength > 48)
        return nullptr;
    auto tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp)
        return nullptr;
    memcpy(tramp, prologue, prologueLength);
    WriteAbsJumpNoRegs(tramp + prologueLength, target + length);

    DWORD old;
    if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &old))
        return nullptr;
    WriteAbsJump(target, detour);
    memset(target + 12, 0x90, length - 12); // nop out the rest of the overwritten instructions
    VirtualProtect(target, length, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, length);
    return tramp;
}

void* RedirectCall(uint8_t* call, void* detour)
{
    if (hot::host) { auto result=hot::host->redirectCall(call, detour); if(!result) Log("hot: rejected call redirect %p",call); return result; }
    return jumppatch::RedirectCall(call, detour);
}

// The start of the function that contains code (one of its chunks), from the exe's unwind
// table: chained chunks lead back to the function's first chunk.
uint8_t* FunctionStart(const uint8_t* code)
{
    DWORD64 image = 0;
    const RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(code), &image, nullptr);
    for (int guard = 0; fn && guard < 16; ++guard) {
        auto* unwind = reinterpret_cast<const uint8_t*>(image + fn->UnwindInfoAddress);
        if (!(unwind[0] >> 3 & 0x4)) // UNW_FLAG_CHAININFO
            return reinterpret_cast<uint8_t*>(image + fn->BeginAddress);
        uint8_t codes = unwind[2];
        size_t at = 4 + ((codes + 1) & ~1u) * 2; // the chained RUNTIME_FUNCTION after the codes
        fn = reinterpret_cast<const RUNTIME_FUNCTION*>(unwind + at);
    }
    return nullptr;
}

int CallsTo(const uint8_t* target, uint8_t** out, int max)
{
    uint8_t* text;
    size_t size;
    if (!TextSection(nullptr, text, size) || size < 5)
        return 0;
    std::vector<uint8_t> code(size); // as it was before the loader's hooks
    hot::Read(text, code.data(), size);
    int n = 0;
    for (size_t i = 0; i + 5 <= size && n < max; ++i) {
        if (code[i] != 0xE8)
            continue;
        int32_t d;
        memcpy(&d, &code[i + 1], 4);
        if (text + i + 5 + d == target)
            out[n++] = text + i;
    }
    return n;
}

bool IsAbsJumpPatch(const uint8_t* target)
{
    uint8_t canonical[12];
    if (hot::host) { hot::Read(target, canonical, sizeof(canonical)); target = canonical; }
    return target[0] == 0x48 && target[1] == 0xB8 && target[10] == 0xFF && target[11] == 0xE0;
}

bool IsHookPatch(const uint8_t* target)
{
    uint8_t canonical[12];
    if (hot::host) { hot::Read(target, canonical, sizeof(canonical)); target = canonical; }
    return jumppatch::KindOf(target) != jumppatch::None;
}

void* InstallHookChained(uint8_t* target, void* detour, size_t length)
{
    if (hot::host) { auto result=hot::host->hook(target, detour, length, target, length); if(!result) Log("hot: rejected chained hook %p length %zu",target,length); return result; }
    if (!IsHookPatch(target))
        return InstallHook(target, detour, length);
    // Another mod's jump is already here: point it at detour and hand back its previous
    // destination, which leads (through that mod) to the original code.
    void* previous = jumppatch::Destination(target);
    if (!previous || !jumppatch::Redirect(target, detour))
        return nullptr;
    return previous;
}

uint8_t* SystemFunction(const char* name)
{
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    uint8_t* text;
    size_t textSize;
    if (!TextSection(nullptr, text, textSize))
        return nullptr;
    const size_t n = strlen(name), image = nt->OptionalHeader.SizeOfImage;
    uint8_t* found = nullptr;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (memcmp(sec->Name, ".rdata", 6) != 0)
            continue;
        for (size_t off = 0; off + 0x18 <= sec->Misc.VirtualSize; off += 8) {
            uint8_t* p = base + sec->VirtualAddress + off;
            const char* str;
            uint64_t length;
            uint8_t* fn;
            memcpy(&length, p + 8, 8);
            if (length != n)
                continue;
            memcpy(&str, p, 8);
            auto at = reinterpret_cast<uintptr_t>(str), from = reinterpret_cast<uintptr_t>(base);
            if (at < from || at - from > image - n || memcmp(str, name, n) != 0)
                continue;
            memcpy(&fn, p + 0x10, 8);
            if (fn < text || fn >= text + textSize)
                continue;
            if (found && found != fn)
                return nullptr;
            found = fn;
        }
    }
    return found;
}

void* RedirectSystemCalls(const uint8_t* target, const char* const* names, int count, void* detour,
                          int& calls, int& systems)
{
    calls = systems = 0;
    std::vector<uint8_t*> fns;
    for (int i = 0; i < count; ++i)
        if (uint8_t* fn = SystemFunction(names[i])) fns.push_back(fn);
        else Log("system not found: %s", names[i]);
    systems = static_cast<int>(fns.size());
    std::vector<uint8_t*> all(4096);
    int found = CallsTo(target, all.data(), static_cast<int>(all.size())), redirected = 0;
    void* original = nullptr;
    for (int i = 0; i < found; ++i) {
        uint8_t* fn = FunctionStart(all[i]);
        bool mine = false;
        for (uint8_t* f : fns) mine |= f == fn;
        if (!mine) continue;
        ++calls;
        void* previous = RedirectCall(all[i], detour);
        if (previous && !original) original = previous;
        redirected += previous != nullptr;
    }
    return calls > 0 && redirected == calls ? original : nullptr;
}
