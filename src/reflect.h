#pragma once
#include <cstddef>
#include <cstdint>

// The game's own type reflection (keen:: type descriptors in the exe), read once at startup so struct field offsets
// come from the running build instead of numbers taken from one build. A field the game no longer has, or a type
// that can't be found, gives the fallback (the value of the build of 2026-11-04) and is logged.
//
// Descriptor (64-bit): +0x00 name, +0x08 length, +0x10 scoped name, +0x18 length, +0x20 full name ("keen::ItemInfo"),
// +0x28 length, +0x40 u32 size, +0x48 u32 member count, u32 kind, +0x50 hash, +0x58 members. Member records are
// 0x30 bytes: name, length, type descriptor, u64 offset. (re/scripts/pytools/refl.py reads the same.)
namespace reflect {

bool Init();

// Byte offset of a field, "uiValues" or a path through member types, "equipment.placedEntity".
size_t Offset(const char* type, const char* field, size_t fallback);
// A type's size.
size_t Size(const char* type, size_t fallback);

// An engine struct offset (not in the reflection) read from two instructions that use it: each pattern's 32-bit
// displacement at index. Taken only when both are found and agree; otherwise the fallback, logged. The patterns come
// from tools/patchcheck/dispsig.py: every displacement wildcarded, so the code around may shift with the struct.
size_t FromCode(const char* name, const char* pattern1, int index1, const char* pattern2, int index2, size_t fallback);

// How the lookups went, for one log line: found in the game, found at a different offset than the fallback, missing.
struct Stats { int found, moved, missing; };
Stats GetStats();

} // namespace reflect
