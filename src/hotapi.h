#pragma once
// Find's view of the Greeneye Loader's hook API (common/greeneye.h, API 2): under the loader,
// every hook goes through the loader (host->hook), every detour holds a hot::Lease, and pattern
// scans read the code as it was before the loader's hooks (host->find, hot::Read). That is what
// lets the loader replace Find while the game runs (hot reload). Standalone (wsock32.dll) host is
// null and Find patches the game itself.
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "../../common/greeneye.h"

using HotHost = GreeneyeHost;

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
