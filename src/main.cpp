#include <windows.h>
#include "config.h"
#include "find.h"
#include "hotapi.h"
#include "inventory.h"
#include "log.h"
#include "ui.h"

#define FIND_VERSION "1.1.8"

bool LoadProxyTargets();

namespace find {
bool HotPrepare();
bool HotReady();
void HotResume();
}

// Hooks the game, once per process: the standalone wsock32.dll and the Greeneye Loader's copy
// may both be installed, and the first to start wins (a reload belongs to the first).
static bool StartFind(const char* how, const GreeneyeHost* host)
{
    if (!greeneye::ClaimProcess(L"Find", host))
        return false;
    LogInit();
    const bool reload = hot::host && hot::host->generation > 1;
    Log("Find " FIND_VERSION " loaded (%s%s, thread %lu, %s)", how, reload ? ", reloaded" : "", GetCurrentThreadId(),
        reload ? "while the game runs" : ui::Client() ? "in a world already" : "before the game starts");
    Config config = LoadConfig();
    if (!ui::Init() || !inv::Init() || !find::Init(config)) {
        Log("inactive: this game version is not supported");
        return false;
    }
    Log("ready: %s to find; chests glow %gs at most, %gs after you come within %gm", config.hotkeyText.c_str(),
        config.glow.glowSeconds, config.glow.nearSeconds, config.glow.nearMeters);
    return true;
}

// Greeneye Loader (greeneye\find\find.dll): find.ini and find.log live in that folder; an
// existing find.ini moves there from the game folder once. With the loader's API 2 every hook
// goes through the loader, so Find can be reloaded while the game runs (generation 2, 3...).
GREENEYE_MOD_START
{
    if (!greeneye::HostOk(host) || host->side != kGreeneyeClient)
        return false;
    hot::host = greeneye::HostHasHooks(host) ? host : nullptr;
    greeneye::SetDataDir(host->modDir);
    greeneye::Migrate("find.ini");
    return StartFind("Greeneye Loader", host);
}

// Before a newer Find replaces this one: wait while the search dialog is open, then put the
// chests' highlight back (the render hook restores it) and stop drawing; resume if it fails.
static const GreeneyeHot kHot = {sizeof(GreeneyeHot), &find::HotPrepare, &find::HotReady, &find::HotResume};
GREENEYE_MOD_HOT
{
    return &kHot;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(instance);
    // Under any other name this is the Greeneye Loader's copy, started by GreeneyeModStart.
    if (!greeneye::LoadedAs(instance, L"wsock32.dll"))
        return TRUE;
    if (!LoadProxyTargets())
        return FALSE;

    // Only act inside the game itself, not tools that happen to load wsock32.dll from here.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!wcsstr(_wcslwr(exe), L"\\enshrouded.exe"))
        return TRUE;

    greeneye::UseGameDirForData();
    StartFind("standalone", nullptr);
    return TRUE;
}
