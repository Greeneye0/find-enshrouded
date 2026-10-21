#ifndef FIND_HOT_RUNTIME
#include <windows.h>
#include "config.h"
#include "find.h"
#include "inventory.h"
#include "log.h"
#include "ui.h"

#define FIND_VERSION "1.0.0"

bool LoadProxyTargets();

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(instance);
    if (!LoadProxyTargets())
        return FALSE;

    // Only act inside the game itself, not tools that happen to load wsock32.dll from here.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!wcsstr(_wcslwr(exe), L"\\enshrouded.exe"))
        return TRUE;

    LogInit();
    Log("Find " FIND_VERSION " loaded (thread %lu, %s)", GetCurrentThreadId(),
        ui::Client() ? "in a world already" : "before the game starts");
    Config config = LoadConfig();
    if (!ui::Init() || !inv::Init() || !find::Init(config.hotkey)) {
        Log("inactive: this game version is not supported");
        return TRUE;
    }
    Log("ready: %s to find", config.hotkeyText.c_str());
    return TRUE;
}

#endif
