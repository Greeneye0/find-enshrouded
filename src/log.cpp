#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include "log.h"
#include "../../common/greeneye.h"

static CRITICAL_SECTION g_logLock;
static bool g_logReady = false;
static FILE* g_logFile = nullptr;

// The log rolls over at 10 MB, kept through restarts: the full one becomes find.old.log (the one before goes)
static const long long kRollBytes = 10LL * 1024 * 1024;

static bool g_rollFailed = false; // another copy has the file open (a hot reload's): tried again next start

static void RollIfFull()
{
    if (!g_logFile || g_rollFailed || _ftelli64(g_logFile) <= kRollBytes)
        return;
    fclose(g_logFile);
    if (!MoveFileExA(greeneye::DataPath("find.log"), greeneye::DataPath("find.old.log"), MOVEFILE_REPLACE_EXISTING))
        g_rollFailed = true;
    g_logFile = _fsopen(greeneye::DataPath("find.log"), "a", _SH_DENYNO);
}

void LogInit()
{
    InitializeCriticalSection(&g_logLock);
    g_logReady = true;
    g_logFile = _fsopen(greeneye::DataPath("find.log"), "a", _SH_DENYNO);
    if (g_logFile)
        _fseeki64(g_logFile, 0, SEEK_END); // "a" reports 0 until the first write
    RollIfFull();
}

void Log(const char* fmt, ...)
{
    if (!g_logReady)
        return;
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(line, sizeof(line), "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, args);
    va_end(args);

    EnterCriticalSection(&g_logLock);
    if (g_logFile) {
        fputs(line, g_logFile);
        fputc('\n', g_logFile);
        fflush(g_logFile);
        RollIfFull();
    }
    LeaveCriticalSection(&g_logLock);
}
