#ifdef FIND_HOT_RUNTIME
#include <windows.h>
#include "hotapi.h"
#include "config.h"
#include "find.h"
#include "ui.h"
#include "inventory.h"
#include "nativehighlight.h"
#include "log.h"
namespace find { bool QueueSearch(const char*); bool HotPrepare(); bool HotReady(); void HotResume(); }
extern "C" __declspec(dllexport) bool FindRuntimeInit(const HotHost* host, HotRuntime* api) {
    if (!host || host->size != sizeof(HotHost) || host->version != 1 || !api || api->size != sizeof(HotRuntime)) return false;
    hot::host = host;
    LogInit();
    Log("Find runtime initializing (hot ABI 1; build %s %s)", __DATE__, __TIME__);
    Config config = LoadConfig();
    if (!ui::Init() || !inv::Init() || !find::Init(config.hotkey)) return false;
    nativehighlight::Status status{sizeof(status)};
    if (!nativehighlight::GetStatus(&status) || !status.ready) { Log("hot runtime requires the render safe-point hook"); return false; }
    *api = {sizeof(HotRuntime),1,find::HotPrepare,find::HotReady,find::HotResume,
        find::QueueSearch,[](void* p){ return nativehighlight::GetStatus(static_cast<nativehighlight::Status*>(p)); }};
    Log("Find runtime ready");
    return true;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}

#endif
