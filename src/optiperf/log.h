#pragma once

namespace optiperf {

// Opens (truncates) the log file; a null/empty path disables logging to file (OutputDebugString still works).
// Safe to call under the loader lock (only kernel32 file APIs are used).
void LogInit(const wchar_t* path);
void Log(const char* fmt, ...);
void LogClose();
// The log file's path as LogInit got it (empty if none): diagnostics write their own files next to it.
const wchar_t* LogPath();

}  // namespace optiperf
