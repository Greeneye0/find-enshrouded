// Offline tests: chained hooks (Find hooking a function Quick Deposit already hooked) and the
// hotkey parser. build/run: tools\tests.bat
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "../src/config.h"
#include "../src/scan.h"
#include "../src/jumppatch.h"

void Log(const char*, ...) {}

using Fn = long long (*)(long long);
static Fn g_firstOriginal = nullptr, g_secondOriginal = nullptr;
static int g_first = 0, g_second = 0;

static long long First(long long v)
{
    ++g_first;
    return g_firstOriginal(v);
}

static long long Second(long long v)
{
    ++g_second;
    return g_secondOriginal(v);
}

static bool Check(const char* what, bool ok)
{
    printf("%-46s %s\n", what, ok ? "OK" : "FAIL");
    return ok;
}

int main()
{
    bool ok = true;
    // ViewInit-style prologue: mov [rsp+10h], rbx; push rbp/rsi/rdi; push r12; push r13 (12
    // bytes), then lea rax, [rcx+1]; pop r13; pop r12; pop rdi; pop rsi; pop rbp; mov rbx,...; ret
    static const unsigned char code[] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
        0x48, 0x8D, 0x41, 0x01,                   // lea rax, [rcx+1]
        0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, // pop r13, r12, rdi, rsi, rbp
        0x48, 0x8B, 0x5C, 0x24, 0x10,             // mov rbx, [rsp+10h]
        0xC3,
    };
    auto fn = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    memcpy(fn, code, sizeof(code));
    ok &= Check("unhooked function returns 42", reinterpret_cast<Fn>(fn)(41) == 42);

    g_firstOriginal = reinterpret_cast<Fn>(InstallHook(fn, reinterpret_cast<void*>(&First), 12)); // Quick Deposit
    ok &= Check("first hook is a jump patch", IsAbsJumpPatch(fn));
    g_secondOriginal = reinterpret_cast<Fn>(InstallHookChained(fn, reinterpret_cast<void*>(&Second), 12)); // Find
    ok &= Check("chained hook hands back the first detour", g_secondOriginal == reinterpret_cast<Fn>(&First));
    long long r = reinterpret_cast<Fn>(fn)(41);
    ok &= Check("chained call returns 42", r == 42);
    ok &= Check("both detours ran once", g_first == 1 && g_second == 1);

    // a function nobody hooked yet: chained hooking falls back to a normal hook
    auto fn2 = fn + 0x100;
    memcpy(fn2, code, sizeof(code));
    g_secondOriginal = reinterpret_cast<Fn>(InstallHookChained(fn2, reinterpret_cast<void*>(&Second), 12));
    g_second = 0;
    ok &= Check("plain hook through the chained call", reinterpret_cast<Fn>(fn2)(1) == 2 && g_second == 1);

    // Creative Mode's safetyhook: "jmp rel32" to a near stub, or "jmp [rip+0]; address", with a
    // trampoline that runs the stolen instructions and returns into the function after them.
    auto trampoline = [](unsigned char* f, size_t stolen) {
        auto t = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        memcpy(t, f, stolen);
        t[stolen] = 0xFF; t[stolen + 1] = 0x25; memset(t + stolen + 2, 0, 4);
        void* back = f + stolen;
        memcpy(t + stolen + 6, &back, 8);
        return t;
    };
    for (int form = 0; form < 2; ++form) {
        auto f = fn + 0x200 + form * 0x100;
        memcpy(f, code, sizeof(code));
        size_t stolen = form == 0 ? 12 : 16; // whole instructions covering 5 / 14 bytes
        g_firstOriginal = reinterpret_cast<Fn>(trampoline(f, stolen));
        void* first = reinterpret_cast<void*>(&First);
        if (form == 0) {
            auto stub = jumppatch::AllocNear(f, 16);
            stub[0] = 0xFF; stub[1] = 0x25; memset(stub + 2, 0, 4); memcpy(stub + 6, &first, 8);
            int32_t d = static_cast<int32_t>(stub - (f + 5));
            f[0] = 0xE9; memcpy(f + 1, &d, 4);
        } else {
            f[0] = 0xFF; f[1] = 0x25; memset(f + 2, 0, 4); memcpy(f + 6, &first, 8);
        }
        unsigned char after[16];
        memcpy(after, f + 5, sizeof(after));
        const char* name = form == 0 ? "safetyhook jmp rel32" : "safetyhook jmp [rip]";
        ok &= Check(name, IsHookPatch(f) && !IsAbsJumpPatch(f));
        g_first = g_second = 0;
        g_secondOriginal = reinterpret_cast<Fn>(InstallHookChained(f, reinterpret_cast<void*>(&Second), 18));
        ok &= Check("  chained call returns 42", g_secondOriginal && reinterpret_cast<Fn>(f)(41) == 42);
        ok &= Check("  both detours ran once", g_first == 1 && g_second == 1);
        ok &= Check("  bytes after its jump untouched", form == 1 ? memcmp(after + 9, f + 14, 7) == 0 : memcmp(after, f + 5, sizeof(after)) == 0);
    }

    Hotkey h;
    ok &= Check("Shift+F", ParseHotkey("Shift+F", h) && h.key == 'F' && h.shift && !h.ctrl && !h.alt);
    ok &= Check("ctrl + alt + g (any case, spaces)", ParseHotkey(" ctrl + alt + g ", h) && h.key == 'G' && h.ctrl && h.alt && !h.shift);
    ok &= Check("F6", ParseHotkey("F6", h) && h.key == VK_F6 && !h.shift);
    ok &= Check("Shift+F12", ParseHotkey("Shift+F12", h) && h.key == VK_F12 && h.shift);
    ok &= Check("7", ParseHotkey("7", h) && h.key == '7');
    ok &= Check("rejects Hyper+F", !ParseHotkey("Hyper+F", h));
    ok &= Check("rejects empty and F99", !ParseHotkey("", h) && !ParseHotkey("F99", h));

    HMODULE proxy = LoadLibraryW(L"..\\..\\bin\\wsock32.dll");
    auto inetAddr = proxy ? reinterpret_cast<unsigned long(__stdcall*)(const char*)>(GetProcAddress(proxy, "inet_addr")) : nullptr;
    ok &= Check("proxy forwards inet_addr(\"1.2.3.4\")", inetAddr && inetAddr("1.2.3.4") == 0x04030201u);

    printf("%s\n", ok ? "ALL OK" : "FAILURES");
    return ok ? 0 : 1;
}
