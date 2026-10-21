#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
// Versioned C/POD boundary. Both sides use /MT; never transfer CRT-owned objects.
struct HotHost {
    uint32_t size, version;
    void* token;
    bool (*enter)(void*);
    void (*leave)();
    uint8_t* (*find)(const char*, void*);
    void (*read)(const void*, void*, size_t);
    void* (*hook)(uint8_t*, void*, size_t, const uint8_t*, size_t);
    void (*poll)();
};
struct HotRuntime {
    uint32_t size, version;
    bool (*prepare)();
    bool (*ready)();
    void (*resume)();
    bool (*search)(const char*);
    bool (*status)(void*);
};
using HotInit = bool (*)(const HotHost*, HotRuntime*);
namespace hot {
inline const HotHost* host = nullptr;
struct Lease {
    bool active;
    Lease() : active(!host || host->enter(host->token)) {}
    ~Lease() { if (host) host->leave(); }
    explicit operator bool() const { return active; }
    Lease(const Lease&) = delete;
};
inline void Read(const void* p, void* out, size_t n) { if (host) host->read(p,out,n); else memcpy(out,p,n); }
inline bool Equal(const void* p, const void* expected, size_t n) {
    unsigned char bytes[64]; if(n>sizeof(bytes)) return false; Read(p,bytes,n); return memcmp(bytes,expected,n)==0;
}
}
