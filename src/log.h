#pragma once

// Appends a timestamped line to find.log in the game folder.
void LogInit();
void Log(const char* fmt, ...);

// Diagnostics for development builds only (build-hot.bat defines FIND_DEV).
#ifdef FIND_DEV
#define DevLog Log
#else
#define DevLog(...) do { if constexpr (false) Log(__VA_ARGS__); } while (0) // compiled out
#endif
