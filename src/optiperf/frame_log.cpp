// A frame-rate log for play sessions: see frame_log.h.

#include "frame_log.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>

#include "calltrace.h"
#include "config.h"
#include "hooks.h"
#include "log.h"
#include "scan.h"

namespace optiperf {
namespace {

using UpdateFn = bool (*)(void* self, int updateFlags, int pauseMode);
constexpr unsigned kUpdateSlot = 3;  // ISystem::Update(int updateFlags, int nPauseMode) (Mod SDK; the frame phases' too)
constexpr int kBins = 4000;          // frame times in 0.05 ms bins, to 200 ms; longer ones in the last
constexpr double kBinMs = 0.05;
constexpr double kStallMs = 1000.0;  // a longer frame is a load or the exit: counted apart

struct State {
    bool installed = false;
    UpdateFn update = nullptr;
    DWORD thread = 0;  // the main thread (the first caller)
    double tickMs = 0;
    LARGE_INTEGER start{}, last{}, secondStart{};
    bool started = false;
    // this second
    uint32_t secondFrames = 0, secondPaused = 0;
    double secondWorst = 0;
    // the session's frames, paused ones apart
    uint64_t frames = 0, paused = 0, stalls = 0;
    double totalMs = 0, worstMs = 0, below100Ms = 0, below60Ms = 0;
    uint32_t bins[kBins + 1] = {};
    FILE* csv = nullptr;
    CRITICAL_SECTION lock;  // (the reports read the session's numbers from the reporter thread)
};
State g;

void EndSecond(const LARGE_INTEGER& now) {
    const double seconds = double(now.QuadPart - g.secondStart.QuadPart) * g.tickMs / 1000.0;
    if (g.csv) {
        std::fprintf(g.csv, "%.1f,%u,%.1f,%.2f,%u\n", double(now.QuadPart - g.start.QuadPart) * g.tickMs / 1000.0,
                     g.secondFrames, seconds > 0 ? double(g.secondFrames) / seconds : 0.0, g.secondWorst, g.secondPaused);
        std::fflush(g.csv);
    }
    g.secondStart = now;
    g.secondFrames = g.secondPaused = 0;
    g.secondWorst = 0;
}

void Count(const LARGE_INTEGER& now, bool paused) {
    if (!g.started) {
        g.started = true;
        g.start = g.last = g.secondStart = now;
        return;
    }
    const double ms = double(now.QuadPart - g.last.QuadPart) * g.tickMs;
    g.last = now;
    ++g.secondFrames;
    if (ms > g.secondWorst) g.secondWorst = ms;
    if (paused || ms > kStallMs) {
        if (paused) ++g.secondPaused;
        EnterCriticalSection(&g.lock);
        ++(paused ? g.paused : g.stalls);
        LeaveCriticalSection(&g.lock);
    } else {
        int bin = int(ms / kBinMs);
        if (bin > kBins) bin = kBins;
        EnterCriticalSection(&g.lock);
        ++g.frames;
        g.totalMs += ms;
        if (ms > g.worstMs) g.worstMs = ms;
        if (ms > 10.0) g.below100Ms += ms;
        if (ms > 1000.0 / 60.0) g.below60Ms += ms;
        ++g.bins[bin];
        LeaveCriticalSection(&g.lock);
    }
    if (double(now.QuadPart - g.secondStart.QuadPart) * g.tickMs >= 1000.0) EndSecond(now);
}

bool UpdateHook(void* self, int updateFlags, int pauseMode) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const DWORD me = GetCurrentThreadId();
    if (!g.thread) g.thread = me;
    if (me == g.thread) Count(now, pauseMode != 0);
    return g.update(self, updateFlags, pauseMode);
}

// the frame time at which `share` of the counted frames are shorter (from the bins; under the lock)
double Percentile(double share) {
    const uint64_t want = uint64_t(double(g.frames) * share);
    uint64_t seen = 0;
    for (int i = 0; i <= kBins; ++i) {
        seen += g.bins[i];
        if (seen > want) return (double(i) + 0.5) * kBinMs;
    }
    return g.worstMs;
}

// the function behind a vtable entry of a module that may not be relocated yet (the loader's notification can come first)
char* Entry(char* base, void* entry) {
    char* p = static_cast<char*>(CallTrace_Original(entry));
    if (IsInCode(base, p, 16)) return p;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    p = base + (reinterpret_cast<uintptr_t>(entry) - uintptr_t(nt->OptionalHeader.ImageBase));
    return IsInCode(base, p, 16) ? p : nullptr;
}

}  // namespace

void FrameLog_OnModuleLoaded(const wchar_t* baseName, char* base, size_t) {
    if (_wcsicmp(baseName, L"CrySystem.dll") != 0 || g.installed) return;
    const std::wstring v = ConfigString(L"Debug", L"FrameLog", L"OPTIPERF_FRAME_LOG", L"off");
    if (_wcsicmp(v.c_str(), L"on") != 0 && v != L"1") return;
    g.installed = true;
    void** const vt = FindRttiVtable(base, ".?AVCSystem@@");
    char* const update = vt ? Entry(base, vt[kUpdateSlot]) : nullptr;
    if (!update) {
        Log("CrySystem.dll: frame log off: CSystem::Update not found");
        return;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g.tickMs = 1000.0 / double(f.QuadPart);
    InitializeCriticalSection(&g.lock);
    std::wstring path = LogPath() ? LogPath() : L"";
    const size_t slash = path.find_last_of(L"\\/");
    path = slash == std::wstring::npos ? L"" : path.substr(0, slash + 1) + L"optiperf_frames.csv";
    if (!path.empty() && _wfopen_s(&g.csv, path.c_str(), L"w") == 0 && g.csv)
        std::fprintf(g.csv, "seconds,frames,fps,longest_ms,paused_frames\n");
    if (!InstallHook(update, reinterpret_cast<void*>(&UpdateHook), reinterpret_cast<void**>(&g.update), "CSystem::Update")) {
        Log("CrySystem.dll: frame log off: could not hook CSystem::Update");
        return;
    }
    Log("CrySystem.dll: frame log on: CSystem::Update +0x%X; a line a second in %ls", unsigned(update - base),
        g.csv ? path.c_str() : L"(no file: the log is off)");
}

void FrameLog_Report(const char* when) {
    if (!g.update) return;
    EnterCriticalSection(&g.lock);
    const uint64_t frames = g.frames, paused = g.paused, stalls = g.stalls;
    const double total = g.totalMs, worst = g.worstMs, b100 = g.below100Ms, b60 = g.below60Ms;
    const double p50 = frames ? Percentile(0.5) : 0, p99 = frames ? Percentile(0.99) : 0, p999 = frames ? Percentile(0.999) : 0;
    LeaveCriticalSection(&g.lock);
    if (!frames) return;
    Log("frames [%s]: %llu counted (not counted: %llu paused, %llu over 1 s: loads, the exit), %.1f fps on average | frame "
        "times: median %.2f ms, 99th %.2f ms (1 %% low %.0f fps), 99.9th %.2f ms (0.1 %% low %.0f fps), longest %.1f ms | time "
        "below 100 fps %.1f %%, below 60 fps %.1f %%",
        when, static_cast<unsigned long long>(frames), static_cast<unsigned long long>(paused),
        static_cast<unsigned long long>(stalls), 1000.0 * double(frames) / total,
        p50, p99, 1000.0 / p99, p999, 1000.0 / p999, worst, 100.0 * b100 / total, 100.0 * b60 / total);
}

}  // namespace optiperf
