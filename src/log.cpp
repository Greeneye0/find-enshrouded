#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include "log.h"
#include "../../common/greeneye.h"
#include "../../common/async_log.h"

static CRITICAL_SECTION g_logLock;
static bool g_logReady = false;
static async_log::Writer g_writer;

// The log rolls over at 10 MB, kept through restarts: the full one becomes find.old.log (the one before goes)
static const long long kRollBytes = 10LL * 1024 * 1024;

static bool g_rollFailed = false; // another copy has the file open (a hot reload's): tried again next start

static void RollIfFull(FILE*& file)
{
    if (!file || g_rollFailed || _ftelli64(file) <= kRollBytes)
        return;
    fclose(file);
    if (!MoveFileExA(greeneye::DataPath("find.log"), greeneye::DataPath("find.old.log"), MOVEFILE_REPLACE_EXISTING))
        g_rollFailed = true;
    file = _fsopen(greeneye::DataPath("find.log"), "a", _SH_DENYNO);
}

void LogInit()
{
    InitializeCriticalSection(&g_logLock);
    g_logReady = true;
    FILE* file = _fsopen(greeneye::DataPath("find.log"), "a", _SH_DENYNO);
    if (file)
        _fseeki64(file, 0, SEEK_END); // "a" reports 0 until the first write
    RollIfFull(file);
    g_writer.Open(file, &RollIfFull);
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
    // On a thread of its own: a slow disk must not stall the caller.
    {
        const size_t length = strlen(line);
        if (length + 1 < sizeof(line)) {
            line[length] = '\n';
            g_writer.Write(line, length + 1);
            line[length] = 0;
        } else {
            g_writer.Write(line, length);
            g_writer.Write("\n", 1);
        }
    }
    LeaveCriticalSection(&g_logLock);
}
