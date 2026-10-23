#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include "log.h"
#include "../../common/greeneye.h"

static CRITICAL_SECTION g_logLock;
static bool g_logReady = false;
static FILE* g_logFile = nullptr;

void LogInit()
{
    InitializeCriticalSection(&g_logLock);
    g_logReady = true;
    g_logFile = _fsopen(greeneye::DataPath("find.log"), "a", _SH_DENYNO);
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
    }
    LeaveCriticalSection(&g_logLock);
}
