#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace optiperf {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_lock;
bool g_lockInit = false;
LARGE_INTEGER g_t0, g_freq;
wchar_t g_path[MAX_PATH * 2] = L"";

}  // namespace

const wchar_t* LogPath() { return g_path; }

void LogInit(const wchar_t* path) {
    if (!g_lockInit) {
        InitializeCriticalSection(&g_lock);
        g_lockInit = true;
    }
    if (path && *path) wcsncpy_s(g_path, path, _TRUNCATE);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    // write-through: the log is written rarely, and it must survive a hang or a hard reset of the machine
    if (path && *path)
        g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
}

void Log(const char* fmt, ...) {
    if (!g_lockInit) return;
    char buf[2048];
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double t = double(now.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
    int len = sprintf_s(buf, "[%9.3f] ", t);

    va_list args;
    va_start(args, fmt);
    const int body = vsnprintf(buf + len, sizeof(buf) - len - 2, fmt, args);
    va_end(args);
    len = body < 0 ? int(sizeof(buf)) - 3 : len + body;
    if (len > int(sizeof(buf)) - 3) len = int(sizeof(buf)) - 3;
    buf[len++] = '\r';
    buf[len++] = '\n';
    buf[len] = 0;

    EnterCriticalSection(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(g_file, buf, DWORD(len), &written, nullptr);
    }
    LeaveCriticalSection(&g_lock);
    OutputDebugStringA(buf);
}

void LogClose() {
    if (!g_lockInit) return;
    EnterCriticalSection(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_file);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_lock);
}

}  // namespace optiperf
