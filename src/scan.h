#pragma once
#include <cstdint>

// Finds a unique byte pattern ("48 8B ?? 05", ?? = any byte) in the game's .text section.
// Returns nullptr if the pattern is missing or occurs more than once.
uint8_t* FindUnique(const char* pattern, void* module = nullptr);

// Finds every match of pattern (as FindUnique) and resolves the rip-relative operand whose
// 32-bit displacement starts dispOffset bytes into the match, the instruction ending at
// insEnd. Returns the target only if all matches agree on it.
uint8_t* ResolveRipConsensus(const char* pattern, int dispOffset, int insEnd);

// Resolves the target of a rip-relative "lea r64, [rip+disp32]" (7 bytes, 48/4C 8D) at addr.
uint8_t* ResolveLea(uint8_t* addr);

// Redirects the function at target to detour by overwriting its first length bytes (at least
// 12) with "mov rax, imm64; jmp rax" plus padding. Those bytes must be whole,
// position-independent instructions. Returns a callable trampoline for the original
// function, or nullptr on failure.
void* InstallHook(uint8_t* target, void* detour, size_t length = 12);

// As above, for a prologue that cannot be copied as is (e.g. a rip-relative load): the
// trampoline runs prologue (prologueLength bytes, equivalent to the first length bytes of
// target) and then continues at target + length.
void* InstallHook(uint8_t* target, void* detour, size_t length, const uint8_t* prologue, size_t prologueLength);

// True if target starts with a "mov rax, imm64; jmp rax" patch (another mod's hook).
bool IsAbsJumpPatch(const uint8_t* target);

// InstallHook that coexists with another mod's hook of the same function (such as Quick
// Deposit's): if target already starts with "mov rax, imm64; jmp rax", the jump is pointed at
// detour and its previous destination is returned as the function to call on.
void* InstallHookChained(uint8_t* target, void* detour, size_t length = 12);
