#include <windows.h>
#include <vector>
#include <string>
#include <memory>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "../src/hotapi.h"
bool LoadProxyTargets();
namespace {
struct Slot { uint8_t* target; size_t length; uint8_t saved[32], patch[32]; uint8_t* thunk; void* base; void* pending; bool seen; };
struct Generation { HMODULE module{}; HotHost host{}; HotRuntime api{sizeof(HotRuntime),1}; bool initialized{}; unsigned number{}; };
SRWLOCK gate = SRWLOCK_INIT;
thread_local unsigned depth;
std::vector<std::unique_ptr<Slot>> slots;
std::vector<std::unique_ptr<Generation>> retained;
Generation* active; Generation* staging; Generation* pending;
bool initial = true, failed = false, preparing = false;
wchar_t folder[MAX_PATH];
unsigned serial;
std::string state = "starting";
SRWLOCK statusLock = SRWLOCK_INIT;
void Status(const std::string& s) {
    AcquireSRWLockExclusive(&statusLock); state=s;
    std::wstring path=std::wstring(folder)+L"Find.hotload.status";
    if(FILE* f=_wfopen(path.c_str(),L"w")) { fprintf(f,"%s\n",s.c_str()); fclose(f); }
    ReleaseSRWLockExclusive(&statusLock);
}
bool Enter(void* token) { if(!depth++) AcquireSRWLockShared(&gate); return active==token; }
void Leave() { if(!--depth) ReleaseSRWLockShared(&gate); }
void Read(const void* p,void* out,size_t n) {
    memcpy(out,p,n);
    auto at=reinterpret_cast<uintptr_t>(p);
    for(auto& slot:slots) {
        auto a=reinterpret_cast<uintptr_t>(slot->target);
        auto lo=(std::max)(at,a), hi=(std::min)(at+n,a+slot->length);
        if(lo<hi) memcpy(static_cast<uint8_t*>(out)+lo-at,slot->saved+lo-a,hi-lo);
    }
}
uint8_t* Find(const char* pattern,void* module) {
    auto base=static_cast<uint8_t*>(module?module:GetModuleHandleW(nullptr));
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    auto sec=IMAGE_FIRST_SECTION(nt);
    std::vector<int> pat;
    for(const char* p=pattern;*p;) { if(*p==' '){++p;continue;} if(*p=='?'){pat.push_back(-1);p+=p[1]=='?'?2:1;} else {pat.push_back(strtoul(p,nullptr,16));p+=2;} }
    uint8_t* found=nullptr;
    if(pat.empty())return nullptr;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i,++sec) {
        if(memcmp(sec->Name,".text",5))continue;
        std::vector<uint8_t> bytes(sec->Misc.VirtualSize); Read(base+sec->VirtualAddress,bytes.data(),bytes.size());
        for(size_t j=0;j+pat.size()<=bytes.size();++j) {
            if(pat[0]>=0 && bytes[j]!=pat[0])continue;
            size_t k=1; while(k<pat.size() && (pat[k]<0 || bytes[j+k]==pat[k]))++k;
            if(k==pat.size()) {if(found)return nullptr;found=base+sec->VirtualAddress+j;}
        }
    }
    return found;
}
void Jump(uint8_t* p,const void* to) { p[0]=0xff;p[1]=0x25;memset(p+2,0,4);memcpy(p+6,&to,8); }
void* Hook(uint8_t* target,void* detour,size_t n,const uint8_t* code,size_t codeSize) {
    if(!staging || n<12 || n>32 || codeSize>48){failed=true;return nullptr;}
    for(auto& s:slots) if(s->target==target) {
        if(s->length!=n || memcmp(target,s->patch,n)){failed=true;return nullptr;}
        s->pending=detour;s->seen=true;return s->base;
    }
    // Runtime additions require a restart. Never patch executing game instructions.
    if(!initial){failed=true;return nullptr;}
    auto s=std::make_unique<Slot>(); s->target=target;s->length=n;memcpy(s->saved,target,n);
    bool chained=target[0]==0x48 && target[1]==0xb8 && target[10]==0xff && target[11]==0xe0;
    if(chained)memcpy(&s->base,target+2,8);
    else {
        auto t=static_cast<uint8_t*>(VirtualAlloc(nullptr,64,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
        if(!t){failed=true;return nullptr;} memcpy(t,code,codeSize);Jump(t+codeSize,target+n);s->base=t;
        FlushInstructionCache(GetCurrentProcess(),t,64);
    }
    // Aligned destination lives outside executable bytes; swaps are atomic.
    s->thunk=static_cast<uint8_t*>(VirtualAlloc(nullptr,32,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if(!s->thunk){failed=true;return nullptr;}
    s->thunk[0]=0xff;s->thunk[1]=0x25;int32_t displacement=10;memcpy(s->thunk+2,&displacement,4);
    memcpy(s->thunk+16,&s->base,8);FlushInstructionCache(GetCurrentProcess(),s->thunk,32);
    DWORD old;
    if(!VirtualProtect(target,n,PAGE_EXECUTE_READWRITE,&old)){failed=true;return nullptr;}
    target[0]=0x48;target[1]=0xb8;memcpy(target+2,&s->thunk,8);target[10]=0xff;target[11]=0xe0;
    memset(target+12,0x90,n-12);VirtualProtect(target,n,old,&old);FlushInstructionCache(GetCurrentProcess(),target,n);
    memcpy(s->patch,target,n);s->pending=detour;s->seen=true;
    auto result=s->base;slots.push_back(std::move(s));return result;
}
void Poll();
Generation* Load() {
    wchar_t name[100];swprintf_s(name,L"Find.loaded.%lu.%u.dll",GetCurrentProcessId(),++serial);
    auto source=std::wstring(folder)+L"Find.runtime.dll", destination=std::wstring(folder)+name;
    if(!CopyFileW(source.c_str(),destination.c_str(),TRUE)) {Status("error: cannot copy Find.runtime.dll");return nullptr;}
    auto g=std::make_unique<Generation>(); g->module=LoadLibraryExW(destination.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!g->module){Status("error: runtime could not load");return nullptr;}
    // Never unload any generation, including rejected candidates.
    auto ptr=g.get(); g->number=serial;
    AcquireSRWLockExclusive(&gate); retained.push_back(std::move(g)); ReleaseSRWLockExclusive(&gate);
    if(!GetProcAddress(ptr->module,"FindRuntimeInit")){Status("error: candidate has no FindRuntimeInit export");return nullptr;}
    ptr->host={sizeof(HotHost),1,ptr,Enter,Leave,Find,Read,Hook,Poll};return ptr;
}
bool InitializeCandidate(HotInit init, const HotHost* host, HotRuntime* api) {
    __try { return init(host, api); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Activate(Generation* g) {
    failed=false; staging=g;
    for(auto& s:slots){s->pending=s->base;s->seen=false;}
    auto init=reinterpret_cast<HotInit>(GetProcAddress(g->module,"FindRuntimeInit"));
    bool ok=InitializeCandidate(init,&g->host,&g->api);staging=nullptr;
    if(!ok || failed || g->api.version!=1 || g->api.size!=sizeof(HotRuntime) || !g->api.prepare || !g->api.ready || !g->api.resume || !g->api.search || !g->api.status) {
        if(active)active->api.resume();
        Status("error: candidate rejected; previous generation retained (new hook sites require restart)");return false;
    }
    active=g;g->initialized=true;
    for(auto& s:slots) InterlockedExchangePointer(reinterpret_cast<void* volatile*>(s->thunk+16),s->pending);
    Status("active generation "+std::to_string(g->number)+"; retained="+std::to_string(retained.size()));return true;
}
void Poll() {
    if(depth || !TryAcquireSRWLockExclusive(&gate))return;
    if(pending) {
        // Refuse until the Find dialog closes. Restore models using the old render hook.
        if(!preparing) preparing=!active || active->api.prepare();
        if(preparing && (!active || active->api.ready())) {
            auto g=pending;pending=nullptr;preparing=false;Activate(g);
        }
    }
    ReleaseSRWLockExclusive(&gate);
}
DWORD WINAPI Watch(void*) {
    auto request=std::wstring(folder)+L"Find.reload";
    for(;;) {
        if(GetFileAttributesW(request.c_str())!=INVALID_FILE_ATTRIBUTES && DeleteFileW(request.c_str())) {
            // LoadLibrary must run outside the game/render callback and outside the gate.
            auto g=Load();
            if(g) {AcquireSRWLockExclusive(&gate);if(!pending){pending=g;preparing=false;Status("pending: waiting for render safe point and closed Find dialog");}else Status("busy: another reload is pending");ReleaseSRWLockExclusive(&gate);}
        }
        Sleep(250);
    }
}
}
extern "C" __declspec(dllexport) bool FindQueueSearch(const char* text) {
    Enter(nullptr); bool result=active && active->api.search(text); Leave();return result;
}
extern "C" __declspec(dllexport) bool FindNativeStatus(void* out) {
    Enter(nullptr);bool result=active && active->api.status(out);Leave();return result;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason!=DLL_PROCESS_ATTACH)return TRUE;
    DisableThreadLibraryCalls(instance);if(!LoadProxyTargets())return FALSE;
    wchar_t exe[MAX_PATH];GetModuleFileNameW(nullptr,exe,MAX_PATH);
    if(!wcsstr(_wcslwr(exe),L"\\enshrouded.exe"))return TRUE;
    GetModuleFileNameW(instance,folder,MAX_PATH);auto slash=wcsrchr(folder,L'\\');if(!slash)return TRUE;slash[1]=0;
    if(auto g=Load())Activate(g);initial=false;
    if(HANDLE t=CreateThread(nullptr,0,Watch,nullptr,0,nullptr))CloseHandle(t);
    return TRUE;
}
