#pragma once

namespace optiperf {

// Opens (truncates) the log file; a null/empty path disables logging to file (OutputDebugString still works).
// Safe to call under the loader lock (only kernel32 file APIs are used).
void LogInit(const wchar_t* path);
void Log(const char* fmt, ...);
void LogClose();

}  // namespace optiperf
