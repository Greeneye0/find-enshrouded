#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <algorithm>
namespace nativehighlight::detail {
template<class T> T Read(const void* p, size_t off) { T value; memcpy(&value, static_cast<const uint8_t*>(p) + off, sizeof(value)); return value; }
inline bool FunctionRange(void* image, void* fn, uintptr_t& begin, uintptr_t& end) {
    auto base = reinterpret_cast<uintptr_t>(image);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto valid = [&](const void* p, size_t n) {
        uintptr_t at = reinterpret_cast<uintptr_t>(p);
        size_t size = nt->OptionalHeader.SizeOfImage;
        return at >= base && n <= size && at - base <= size - n;
    };
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto table = reinterpret_cast<RUNTIME_FUNCTION*>(base + dir.VirtualAddress);
    bool found = false;
    for (unsigned i = 0; i < dir.Size / sizeof(*table); ++i) {
        RUNTIME_FUNCTION root = table[i];
        // MSVC can split a function into chained unwind regions. ViewNext's
        // return address can lie beyond the first region (dynamic: +0x42).
        bool terminated = false;
        for (unsigned depth = 0; depth < 16; ++depth) {
            auto info = reinterpret_cast<const uint8_t*>(base + root.UnwindData);
            if (!valid(info, 4)) return false;
            if (((info[0] >> 3) & UNW_FLAG_CHAININFO) == 0) { terminated = true; break; }
            size_t tail = 4 + ((static_cast<size_t>(info[2]) + 1) & ~size_t(1)) * 2;
            if (!valid(info + tail, sizeof(root))) return false;
            root = Read<RUNTIME_FUNCTION>(info, tail);
        }
        if (!terminated) return false;
        if (base + root.BeginAddress != reinterpret_cast<uintptr_t>(fn)) continue;
        if (!found) { begin = reinterpret_cast<uintptr_t>(fn); end = begin; }
        end = (std::max)(end, base + table[i].EndAddress);
        found = true;
    }
    return found;
}

}
