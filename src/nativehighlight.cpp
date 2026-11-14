#include "hotapi.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>
#include "nativehighlight.h"
#include "nativeimage.h"
#include "proxy_math.h"
#include "silhouette.h"
#include "inventory.h"
#include "scan.h"
#include "log.h"
#include "ui.h"

namespace nativehighlight {
namespace {
constexpr unsigned kMax = 256;
// Through walls: a copy of each lit model's render instance (the renderer's own create / bind / transform / delete)
// moved toward the camera along the view ray and scaled by the same factor, so it covers the same pixels but stands in
// front of walls; it shares the source's animation binding (+0x130), so an NPC's copy moves as the NPC does, and it gets
// Find's outline. The HUD silhouette is drawn only where no copy could be made. Placed right before the frame stages its
// render instances, with this frame's camera (the client's view): placed when the model systems run, they trailed a
// turning camera by a frame (Find's first try, given up in 2026-10; the map mod's G G found the staging call, 2026-11-11).
constexpr double kCopyNear = 1.5;   // metres from the eye at the least...
constexpr double kCopyBeforePlayer = 1.0; // ...and this far on the eye's side of the player
constexpr uint8_t kGroup = 7; // Group4; keep interaction/validity colors separate.
struct Span { const void* data; uint64_t count; };
struct Field { const char* text; uint64_t length; };
struct System {
    uintptr_t begin, end;
    unsigned reads, writes, modelSlot;
    const char* name;
};
struct Applied {
    void* renderer;
    void* registry;
    void* instance;
    uint32_t handle;
    uint8_t before, last;
    uint32_t proxy;
    void* proxyInstance;
    silhouette::Snapshot silhouette;
    bool haveSilhouette;
    // the source where the model systems left it, for placing the copy before staging
    int64_t sourcePos[3];
    float sourceRot[4], sourceScale[3], radius;
};
using NextFn = bool (*)(void*, void*, uint32_t);
using SetFn = void (*)(void*, const Span*, const Span*);
using InstanceFn = uint8_t* (*)(void*, uint32_t, bool);
using ColorFn = float* (*)(float*, uint32_t);
using detail::Spatial;
using CreateFn = uint32_t (*)(void*, const void*, const Spatial*);
using TransformFn = void (*)(void*, const Span*, const Span*, bool);
using DeleteFn = void (*)(void*, const Span*);
CreateFn g_create;
TransformFn g_transform;
DeleteFn g_delete;
using BindFn = void (*)(void*,const Span*,uint32_t);
BindFn g_bind;
using UploadFn=void (*)(void*);
UploadFn g_upload;
UploadFn g_stageNext; // the frame's call staging the render instances (same signature as the upload)
volatile LONG g_copyCount;
std::atomic<uint8_t*> g_client{nullptr};
float g_player[3];
volatile LONG g_havePlayer;
NextFn g_next;
SetFn g_set;
InstanceFn g_instance;
ColorFn g_color;
System g_systems[2]{};
void* g_paletteReturns[2]{};
SRWLOCK g_lock = SRWLOCK_INIT;
Target g_targets[kMax]{};
unsigned g_count;
ULONGLONG g_published;
Applied g_applied[kMax]{};
unsigned g_appliedCount;
bool g_ready;
volatile LONG g_pulsing;
thread_local bool g_inHook;

template<class T> T Read(const void* p, size_t off = 0) {
    T value;
    memcpy(&value, static_cast<const uint8_t*>(p) + off, sizeof(value));
    return value;
}

bool InImage(const void* p, size_t length) {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto at = reinterpret_cast<uintptr_t>(p);
    return at >= base && length <= nt->OptionalHeader.SizeOfImage &&
           at - base <= nt->OptionalHeader.SizeOfImage - length;
}

bool Named(const Field& f, const char* text) {
    size_t n = strlen(text);
    return f.length == n && InImage(f.text, n) && memcmp(f.text, text, n) == 0;
}

bool FunctionRange(void* fn, uintptr_t& begin, uintptr_t& end) {
    return detail::FunctionRange(GetModuleHandleW(nullptr), fn, begin, end);
}

// Use the game's own named system definition, not build-specific absolute addresses.
bool DiscoverSystem(const char* name, System& result) {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    const uint8_t* found = nullptr;
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (memcmp(sec->Name, ".rdata", 6) != 0) continue;
        for (size_t off = 0; off + 0x180 <= sec->Misc.VirtualSize; off += 8) {
            auto p = base + sec->VirtualAddress + off;
            if (!Named(Read<Field>(p), name)) continue;
            void* fn = Read<void*>(p, 0x10);
            uintptr_t begin, end;
            if (!FunctionRange(fn, begin, end)) continue;
            if (found) return false;
            found = p;
            result.begin = begin;
            result.end = end;
        }
    }
    if (!found) return false;
    result.name = name;
    unsigned slot = 0;
    bool model = false;
    for (unsigned list = 0; list < 4; ++list) {
        auto span = Read<Span>(found, 0x38 + 0x10 * list);
        if (span.count > 32 || (span.count && !InImage(span.data, span.count * sizeof(Field)))) return false;
        for (unsigned i = 0; i < span.count; ++i, ++slot) {
            auto field = Read<Field>(span.data, i * sizeof(Field));
            if (Named(field, "ModelComponent")) {
                if (model || list != 2) return false;
                model = true;
                result.modelSlot = slot;
            }
        }
        if (list < 2) result.reads += static_cast<unsigned>(span.count);
        else result.writes += static_cast<unsigned>(span.count);
    }
    // Both systems have a single global resource, RenderScene, at the end of their view.
    // Verify the final view resource at runtime against the model resolver as well.
    return model;
}

void Set(void* renderer, uint32_t handle, uint8_t color) {
    Span handles{&handle, 1}, colors{&color, 1};
    g_set(renderer, &handles, &colors);
}

void RemoveProxy(Applied& a);
void Restore(unsigned i) {
    Applied a = g_applied[i];
    RemoveProxy(a);
    auto current = g_instance(a.renderer, a.handle, true);
    // A group another mod set (Quick Deposit's 6, the map mod's 7) is not put back: that mod
    // restores its own, and once both had lit the chest it stayed lit with neither.
    if (current == a.instance && current && current[0x78] == a.last)
        Set(a.renderer, a.handle, a.before == 6 || a.before == 7 ? 0 : a.before);
    g_applied[i] = g_applied[--g_appliedCount];
}

void RemoveProxy(Applied& a) {
    if (!a.proxy) return;
    if (g_instance(a.renderer,a.proxy,true)==a.proxyInstance) {
        Span handles{&a.proxy,1}; g_delete(a.renderer,&handles);
    }
    a.proxy=0; a.proxyInstance=nullptr;
    InterlockedDecrement(&g_copyCount);
}

volatile LONG g_stageCalls, g_viewEyes, g_placed; // dev: what the staging hook did
// This frame's eye: the client's view matrix, else the renderer's camera.
bool Eye(void* renderer, double eye[3], double forward[3]) {
    uint8_t* client = g_client.load(std::memory_order_relaxed);
    float view[16];
    if (client) {
        memcpy(view, client + 0x1d20, sizeof(view));
        if (detail::EyeFromView(view, eye, forward)) { InterlockedIncrement(&g_viewEyes); return true; }
    }
    auto camera = Read<uint8_t*>(renderer, 0x40);
    return camera && detail::EyeFromCamera(Read<Spatial>(camera, 0x8d0), eye, forward);
}

double Nearest(const double eye[3]) {
    double nearest = kCopyNear;
    if (InterlockedCompareExchange(&g_havePlayer, 0, 0)) {
        double d2 = 0;
        for (int j = 0; j < 3; ++j) {
            double d = g_player[j] + (j == 1 ? 1.0 : 0.0) - eye[j];
            d2 += d * d;
        }
        nearest = (std::max)(nearest, (std::min)(std::sqrt(d2) - kCopyBeforePlayer, 12.0));
    }
    return nearest;
}

// The lit model's copy where the model is now (created on the first call): 1 there is one, 0 none needed (the model
// is right before the eye, in view), -1 none could be made (the silhouette shows it instead).
int UpdateProxy(Applied& a, uint8_t* source) {
    if (!g_create || !g_transform || !g_delete || !g_bind) return -1;
    auto resource = Read<uint8_t*>(source, 0x108);
    auto asset = resource ? Read<uint8_t*>(resource, 0x38) : nullptr;
    if (!asset) { RemoveProxy(a); return -1; }
    Spatial transform = Read<Spatial>(source, 0x38);
    memcpy(a.sourcePos, transform.position, sizeof(a.sourcePos));
    memcpy(a.sourceRot, transform.rotation, sizeof(a.sourceRot));
    memcpy(a.sourceScale, transform.scale, sizeof(a.sourceScale));
    a.radius = Read<float>(asset, 0x58);
    double eye[3], forward[3];
    if (!Eye(a.renderer, eye, forward)) { RemoveProxy(a); return -1; }
    if (!detail::PerspectiveCopy(eye, forward, Nearest(eye), transform, a.radius)) {
        RemoveProxy(a); // right before the eye (or behind it): nothing to show through
        return 0;
    }
    if (a.proxy && g_instance(a.renderer,a.proxy,true)!=a.proxyInstance) { // gone with its renderer (a world left)
        a.proxy=0; a.proxyInstance=nullptr;
        InterlockedDecrement(&g_copyCount);
    }
    const bool created = !a.proxy;
    if (created) {
        a.proxy=g_create(a.renderer,resource+0x18,&transform);
        if (!a.proxy) return -1;
        a.proxyInstance=g_instance(a.renderer,a.proxy,true);
        InterlockedIncrement(&g_copyCount);
        DevLog("copy %08x of %08x (resource kind %u, binding %u)", a.proxy, a.handle, Read<uint8_t>(resource, 0x14),
            Read<uint32_t>(source, 0x130));
    }
    Span handles{&a.proxy,1}, transforms{&transform,1};
    if (uint32_t binding=Read<uint32_t>(source,0x130)) g_bind(a.renderer,&handles,binding);
    if (created || !g_stageNext) g_transform(a.renderer,&handles,&transforms,created);
    Set(a.renderer,a.proxy,kGroup);
    return 1;
}

// Called only at the END of the native model system's iteration, on its own thread.
void Apply(void* iter, void* view, const System& system) {
    auto state = Read<uint8_t*>(iter);
    static volatile LONG observed[2]{};
    unsigned which = system.reads == 6 ? 0 : 1;
    if (state && !InterlockedExchange(&observed[which], 1)) {
        DevLog("native: %s state %p registry %p counts %u/%u expected %u/%u", system.name, state,
            Read<void*>(state, 0x30), Read<uint16_t>(state, 0x4b8), Read<uint16_t>(state, 0x4ba), system.reads, system.writes);
        for (unsigned slot = 0; slot < system.reads + system.writes; ++slot) {
            auto r = state + 0x1c8 + slot * 0x10;
            DevLog("native: slot %u data %p stride %u type %u flags %04x", slot, Read<void*>(r), Read<uint16_t>(r, 8), Read<uint16_t>(r, 10), Read<uint16_t>(r, 12));
        }
    }
    if (!state || Read<uint16_t>(state, 0x4b8) != system.reads ||
        Read<uint16_t>(state, 0x4ba) != system.writes) return;
    auto registry = Read<void*>(state, 0x30);
    auto record = state + 0x1c8 + system.modelSlot * 0x10;
    uint16_t type = Read<uint16_t>(record, 0xa);
    if (type >= 1024 || Read<uint16_t>(record, 8) != 4) return;
    // ViewInit puts global resources last. These two verified view layouts are 0x58/0xb8.
    size_t viewSize = system.reads == 6 && system.writes == 2 ? 0xb8 :
                      system.reads == 2 && system.writes == 1 ? 0x58 : 0;
    if (!viewSize) return;
    auto scene = Read<void*>(view, viewSize - 8);
    if (!scene) return;
    auto renderer = Read<void*>(scene, 0x98);
    if (!renderer) return;
    // Render jobs can use column views with no registry in state+0x30. The
    // client render registry is separate from the authoritative inventory one.
    void* registries[16]{};
    int registryCount = inv::Registries(registries, 16);

    AcquireSRWLockExclusive(&g_lock);
    // The outer SEH wrapper must release this lock on a memory-layout mismatch.
    __try {
        ULONGLONG now = GetTickCount64();
        bool active = g_count && now - g_published < 2000;
        uint32_t handles[kMax]{};
        float alphas[kMax]{};
        unsigned count = 0;
        if (!registry && active) {
            for (int r = 0; r < registryCount && !registry; ++r) {
                for (unsigned i = 0; i < g_count; ++i) {
                    auto model = static_cast<const uint32_t*>(inv::Data(registries[r], type, g_targets[i].entity));
                    auto pos = static_cast<const int64_t*>(inv::Data(registries[r], 133, g_targets[i].entity));
                    if (!model || !pos || !*model || !g_instance(renderer, *model, true)) continue;
                    bool same = true;
                    for (int j = 0; j < 3; ++j)
                        same &= std::fabs(static_cast<float>(pos[j] / 4294967296.0) - g_targets[i].position[j]) < 0.15f;
                    if (same) { registry = registries[r]; break; }
                }
            }
        }
        if (active && registry) for (unsigned i = 0; i < g_count; ++i) {
            const Target& t = g_targets[i];
            if (t.alpha <= 0.01f) continue;
            auto model = static_cast<const uint32_t*>(inv::Data(registry, type, t.entity));
            auto pos = static_cast<const int64_t*>(inv::Data(registry, 133, t.entity));
            if (!model || !pos) continue;
            bool same = true;
            for (int j = 0; j < 3; ++j)
                same &= std::fabs(static_cast<float>(pos[j] / 4294967296.0) - t.position[j]) < 0.15f;
            if (!same || !*model || !g_instance(renderer, *model, true)) continue;
            auto found=std::find(handles,handles+count,*model);
            if(found==handles+count) { alphas[count]=t.alpha;handles[count++]=*model; }
            else alphas[found-handles]=(std::max)(alphas[found-handles],t.alpha);
        }
        static ULONGLONG reportAt[2]{};
        if (active && now - reportAt[which] > 5000) {
            reportAt[which] = now;
            DevLog("native: %s targets %u resolved %u type %u scene %p renderer %p", system.name, g_count, count, type, scene, renderer);
        }
        for (unsigned i = 0; i < g_appliedCount;) {
            const Applied& a = g_applied[i];
            if (a.renderer == renderer && (!registry || a.registry == registry) &&
                std::find(handles, handles + count, a.handle) == handles + count) Restore(i);
            else ++i;
        }
        for (unsigned i = 0; i < count; ++i) {
            uint8_t* instance = g_instance(renderer, handles[i], true);
            if (!instance) continue;
            unsigned j = 0;
            while (j < g_appliedCount && !(g_applied[j].renderer == renderer && g_applied[j].handle == handles[i])) ++j;
            if (j == g_appliedCount) {
                if (j == kMax) continue;
                g_applied[g_appliedCount++] = {renderer, registry, instance, handles[i], instance[0x78], kGroup,0,nullptr};
                DevLog("native: %s model type %u, handle %08x, previous highlight %u", system.name, type, handles[i], instance[0x78]);
            }
            Applied& a = g_applied[j];
            if (a.instance != instance) continue;
            // Let interaction/placement highlights take precedence over Find.
            if (instance[0x78] != 0 && instance[0x78] != a.last) continue;
            Set(renderer, handles[i], kGroup);
            a.haveSilhouette=false;
            if (UpdateProxy(a,instance) >= 0) continue; // its copy shows it through walls (or it's right in view): no silhouette
            auto resource=Read<uint8_t*>(instance,0x108);
            auto asset=resource?Read<uint8_t*>(resource,0x38):nullptr;
            if(asset && Read<uint32_t>(asset,0x70)>0 && Read<uint32_t>(asset,0x70)<256) {
                auto mesh=asset+0x6c+Read<int32_t>(asset,0x6c);
                memcpy(a.silhouette.asset,asset,0x160);
                memcpy(a.silhouette.mesh,mesh,56);
                a.silhouette.transform=Read<Spatial>(instance,0x38);
                a.silhouette.alpha=alphas[i];
                a.haveSilhouette=true;
            }
        }
        InterlockedExchange(&g_pulsing, g_appliedCount ? 1 : 0);
    } __finally {
        ReleaseSRWLockExclusive(&g_lock);
    }
}

// Which render system is iterating is told by the iterator's read / write counts (6/2 and 2/1), not
// by the return address: other mods (Quick Deposit, the map mod) redirect the same calls, and when
// one of them sits in front of Find, Find's return address is that mod's detour.
const System* Iterating(void* iter)
{
    __try {
        auto state = Read<uint8_t*>(iter);
        if (!state) return nullptr;
        for (const auto& system : g_systems)
            if (Read<uint16_t>(state, 0x4b8) == system.reads && Read<uint16_t>(state, 0x4ba) == system.writes)
                return &system;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}
bool NextHookBody(void* iter, void* view, uint32_t size, const System* system);
bool NextHook(void* iter, void* view, uint32_t size)
{
    const System* system = Iterating(iter);
    bool result;
    { hot::Lease lease; if (!lease) return g_next(iter, view, size);
      result = NextHookBody(iter, view, size, system); }
    if (hot::host && system) hot::host->poll();
    return result;
}
bool NextHookBody(void* iter, void* view, uint32_t size, const System* system)
{
    bool next = g_next(iter, view, size);
    if (next || g_inHook || !system) return next;
    g_inHook = true;
    __try { Apply(iter, view, *system); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_pulsing, 0);
        static volatile LONG logged;
        if (!InterlockedExchange(&logged, 1)) Log("native: model layout guard failed; skipped render update");
    }
    g_inHook = false;
    return next;
}

// Right before the frame stages its render instances (g_lock held): every copy placed with this frame's camera.
void PlaceCopiesInner(void* renderer) {
    double eye[3], forward[3];
    InterlockedIncrement(&g_stageCalls);
    if (!Eye(renderer, eye, forward)) return;
    const double nearest = Nearest(eye);
    for (unsigned i=0;i<g_appliedCount;++i) {
        Applied& a=g_applied[i];
        if (!a.proxy || a.renderer!=renderer || a.radius<=0.0f) continue;
        if (g_instance(a.renderer,a.proxy,true)!=a.proxyInstance) continue;
        Spatial transform{};
        memcpy(transform.position, a.sourcePos, sizeof(a.sourcePos));
        memcpy(transform.rotation, a.sourceRot, sizeof(a.sourceRot));
        memcpy(transform.scale, a.sourceScale, sizeof(a.sourceScale));
        if (!detail::PerspectiveCopy(eye, forward, nearest, transform, a.radius)) continue;
        Span handles{&a.proxy,1},transforms{&transform,1};
        g_transform(a.renderer,&handles,&transforms,false);
        InterlockedIncrement(&g_placed);
    }
    static ULONGLONG at;
    if (GetTickCount64() - at > 3000) {
        at = GetTickCount64();
        DevLog("copies: %ld staging calls, %ld with the client's view, %ld placements in 3 s; eye %.2f %.2f %.2f",
            InterlockedExchange(&g_stageCalls, 0), InterlockedExchange(&g_viewEyes, 0), InterlockedExchange(&g_placed, 0), eye[0], eye[1], eye[2]);
    }
}
void PlaceCopies(void* renderer) {
    if (!InterlockedCompareExchange(&g_copyCount,0,0) || !renderer) return;
    AcquireSRWLockExclusive(&g_lock);
    __try { PlaceCopiesInner(renderer); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        static volatile LONG logged;
        if (!InterlockedExchange(&logged, 1)) Log("native: placing the through-wall copies faulted");
    }
    ReleaseSRWLockExclusive(&g_lock);
}
void StageHook(void* renderer) {
    { hot::Lease lease; if (lease) PlaceCopies(renderer); }
    g_stageNext(renderer);
}
float* ColorHookBody(float* out, uint32_t color, void* returnSite);
float* ColorHook(float* out, uint32_t color)
{
    void* site = _ReturnAddress();
    hot::Lease lease;
    if (!lease) return g_color(out, color);
    return ColorHookBody(out, color, site);
}
float* ColorHookBody(float* out, uint32_t color, void* returnSite)
{
    void* caller = returnSite;
    if (InterlockedCompareExchange(&g_pulsing, 0, 0) &&
        (caller == g_paletteReturns[0] || caller == g_paletteReturns[1])) {
        float phase = static_cast<float>(GetTickCount64() % 1800) / 1800.0f;
        float gain = 0.38f + 0.62f * (0.5f - 0.5f * std::cos(phase * 6.283185307f));
        auto red = static_cast<uint32_t>(255 * gain);
        auto green = static_cast<uint32_t>(205 * gain);
        auto blue = static_cast<uint32_t>(110 * gain);
        color = 0xff000000u | (blue << 16) | (green << 8) | red;
    }
    return g_color(out, color);
}
} // namespace

void Publish(const Target* targets, unsigned count) {
    if (!g_ready) return;
    AcquireSRWLockExclusive(&g_lock);
    g_count = (std::min)(count, kMax);
    if (g_count) memcpy(g_targets, targets, g_count * sizeof(Target));
    g_published = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lock);
}
void Clear() { Publish(nullptr, 0); }
void SetView(void* client, const float player[3]) {
    g_client.store(static_cast<uint8_t*>(client), std::memory_order_relaxed);
    if (player) {
        memcpy(g_player, player, sizeof(g_player));
        InterlockedExchange(&g_havePlayer, 1);
    }
}
static volatile LONG g_stressTest=0;
void SetStressTest(bool enabled) { InterlockedExchange(&g_stressTest,enabled?1:0); }
void DrawSilhouettes() {
    silhouette::Snapshot snapshots[256];unsigned count=0;
    unsigned limit=InterlockedCompareExchange(&g_stressTest,0,0)?256:16;
    AcquireSRWLockShared(&g_lock);
    if(g_count && GetTickCount64()-g_published<2000)
        for(unsigned i=0;i<g_appliedCount && count<limit;++i)
            if(g_applied[i].haveSilhouette) snapshots[count++]=g_applied[i].silhouette;
    ReleaseSRWLockShared(&g_lock);
    silhouette::Draw(snapshots,count);
}
bool GetStatus(Status* out) {
    if (!out || out->size != sizeof(Status)) return false;
    AcquireSRWLockShared(&g_lock);
    *out = {sizeof(Status), g_ready ? 1u : 0u, g_count, g_appliedCount,
            static_cast<uint32_t>(InterlockedCompareExchange(&g_pulsing, 0, 0))};
    ReleaseSRWLockShared(&g_lock);
    return true;
}

// Only the two render systems' calls to the iterator go through NextHook: the iterator itself and
// its many other callers stay as they are (XHL's Area Harvest disassembles the iterator and
// backs out when its first bytes are a hook). They all call the same function, so the first
// redirect's answer serves them all.
bool HookNext(uint8_t* next)
{
    std::vector<uint8_t*> all(4096);
    int found = CallsTo(next, all.data(), static_cast<int>(all.size())), n = 0, redirected = 0;
    for (int i = 0; i < found; ++i) {
        auto call = reinterpret_cast<uintptr_t>(all[i]);
        bool mine = false;
        for (const auto& system : g_systems)
            mine |= call >= system.begin && call < system.end;
        if (!mine) continue;
        ++n;
        void* previous = RedirectCall(all[i], reinterpret_cast<void*>(&NextHook));
        if (previous && !g_next) g_next = reinterpret_cast<NextFn>(previous);
        redirected += previous != nullptr;
    }
    Log("native: %d of the render systems' %d iterator calls redirected (%d calls in all)", redirected, n, found);
    return g_next != nullptr && n > 0 && redirected == n;
}

bool Init() {
    auto next = FindUnique("40 57 48 83 EC 20 4C 8B 01 48 8B FA 8B 41 08 4C 8B C9 49 3B 80 ?? 08 00 00");
    auto setter = FindUnique("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 33 DB 4D 8B F0 48 8B FA 48 8B F1 48 39 5A 08 76 46 66 66 0F 1F 84 00 00 00 00 00 48 8B 07 41 B0 01 48 8B CE 8B 14 98 E8 ?? ?? ?? ?? 48 85 C0 74 1D 49 8B 0E 0F B6 14 0B 38 50 78");
    auto instance = FindUnique("85 D2 74 29 48 8B 81 C8 01 00 00 44 8B C2 41 81 E0 FF FF FF 00 C1 EA 18");
    auto color = FindUnique("40 53 48 83 EC 70 F3 0F 10 05 ?? ?? ?? ?? 48 8B D9 0F 29 74 24 60 0F 29 7C 24 50");
    auto palette1 = FindUnique("8B 53 64 E8 ?? ?? ?? ?? 0F 28 C6 0F 28 CE 0F 28 D6 0F 10 18");
    auto palette2 = FindUnique("8B 53 64 E8 ?? ?? ?? ?? 0F 28 C6 48 8D 8D ?? ?? ?? ?? 0F 28 CE 0F 28 D6 48 8B D3");
    if (!next || !setter || !instance || !color || !palette1 || !palette2 ||
        !DiscoverSystem("dynamic_render_model", g_systems[0]) ||
        !DiscoverSystem("static_render_model", g_systems[1])) {
        Log("native: discovery failed (next %p, set %p, instance %p, color %p, palettes %p/%p)",next,setter,instance,color,palette1,palette2);
        return false;
    }
    // Validate both identified palette calls resolve to the identified conversion function.
    // The map mod redirects the first one to a hook of its own that calls on, so after a hot reload of Find (the map
    // loaded since) the live call goes there: the loader's canonical bytes still show the game's call. Another mod's
    // hook in between is fine; the patterns are specific enough to be the right calls.
    for (auto p : {palette1, palette2}) {
        int32_t original = 0;
        hot::Read(p + 4, &original, 4);
        if (p + 8 + original != color && p + 8 + Read<int32_t>(p, 4) != color)
            Log("native: the palette call at %p goes to %p, not straight to the colour converter %p (another mod's hook)",
                p, p + 8 + Read<int32_t>(p, 4), color);
    }
    g_create=reinterpret_cast<CreateFn>(FindUnique("40 55 57 41 54 41 56 41 57 48 83 EC 30"));
    g_transform=reinterpret_cast<TransformFn>(FindUnique("48 8B C4 44 88 48 20 4C 89 40 18 55"));
    g_delete=reinterpret_cast<DeleteFn>(FindUnique("48 89 6C 24 20 57 41 54 41 56 48 83 EC 30"));
    g_bind=reinterpret_cast<BindFn>(FindUnique("48 89 5C 24 18 48 89 6C 24 20 56 41 54 41 57 48 83 EC 20 45 33 E4"));
    g_upload=reinterpret_cast<UploadFn>(FindUnique("48 8B C4 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 88 48 81 EC 78 01 00 00 4C 8B 71 18"));
    if (!g_create || !g_transform || !g_delete || !g_bind || !g_upload) {
        Log("native: render functions missing (create %p, transform %p, delete %p, bind %p, upload %p)",
            reinterpret_cast<void*>(g_create), reinterpret_cast<void*>(g_transform), reinterpret_cast<void*>(g_delete),
            reinterpret_cast<void*>(g_bind), reinterpret_cast<void*>(g_upload));
        return false;
    }
    g_set = reinterpret_cast<SetFn>(setter);
    g_instance = reinterpret_cast<InstanceFn>(instance);
    g_paletteReturns[0] = palette1 + 8;
    g_paletteReturns[1] = palette2 + 8;
    // Relocate the RIP-relative scalar load in the color converter's 14-byte prologue.
    uint8_t prologue[20] = {0x40,0x53,0x48,0x83,0xec,0x70,0x48,0xb8};
    int32_t displacement; hot::Read(color + 10, &displacement, 4);
    auto constant = color + 14 + displacement;
    memcpy(prologue + 8, &constant, 8);
    const uint8_t load[] = {0xf3,0x0f,0x10,0x00};
    memcpy(prologue + 16, load, 4);
    g_color = reinterpret_cast<ColorFn>(InstallHook(color, reinterpret_cast<void*>(&ColorHook), 14, prologue, sizeof(prologue)));
    if (!g_color) {
        Log("native: could not hook the colour converter at %p", color);
        return false;
    }
    // The frame function's call staging the render instances (mov rcx, r12; call; cmp qword [r12 + 0xa38], 0): the
    // copies are placed right before it. The map mod redirects it too; RedirectCall chains on whatever is there.
    if (uint8_t* stage = FindUnique("49 8B CC E8 ?? ?? ?? ?? 49 83 BC 24 38 0A 00 00 00 74"))
        g_stageNext = reinterpret_cast<UploadFn>(RedirectCall(stage + 3, reinterpret_cast<void*>(&StageHook)));
    Log("native: through-wall copies %s", g_stageNext ? "placed before staging" : "placed with the model systems (staging call not found)");
    g_ready = HookNext(next);
    Log("native: %s; dynamic %p, static %p",g_ready ? "renderer pulse ready" : "hook failed",reinterpret_cast<void*>(g_systems[0].begin),reinterpret_cast<void*>(g_systems[1].begin));
    return g_ready;
}
} // namespace nativehighlight
