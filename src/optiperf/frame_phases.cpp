// Frame phases: see frame_phases.h. The game loop (CryAction) calls ISystem::RenderBegin, Update, Render and RenderEnd
// through CSystem's vtable: those slots (Mod SDK order 3-6) are patched in the class vtable at the second flush's entry.
// Not at the first: the system snapshot copies that vtable then, and its copy ends at the first slot that doesn't lead
// into CrySystem's code. At a flush's entry the object holds its class vtable (the copy is swapped in from the capture
// to the flush's end). On the main thread, each hook moves the frame to its phase on entry and on to the next one on
// exit; the time between two moves goes to the part being left.
//
// RenderWorld itself is not hooked: the reflection part tells its RenderWorld calls in the flush apart by their return
// address, which a hook in the 3D engine's vtable would replace with its own. ISystem::Render is RenderWorld but for
// 3 us per frame (measured with a RenderWorld hook, 2026-10-19).
//
// The time is summed per interval, from one outer flush's start to the next one's: the flush (B), then the rest of
// the main thread's work (C, the present E in it), then the next frame's traversal and optiperf's work before its
// flush (A). Besides the parts' means, the report gives per frame what each interval would take with the flush on a
// render thread, from that interval's own parts (no copies, no waits for the GPU, a free hand-off):
//   - beside the main thread's rest, with a barrier before the next traversal: A + max(B, C); with the present moved to
//     the render thread too: A + max(B + E, C - E);
//   - beside the next traversal as well (the renderer's frame state double-buffered): max(B, A + C), and
//     max(B + E, A + C - E).
// Intervals longer than 250 ms (a load, a hitch) are left out of the means and counted apart; so is the first one
// (the hooks went in just before it).

#include "frame_phases.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>

#include "calltrace.h"
#include "config.h"
#include "log.h"
#include "scan.h"

namespace optiperf {
namespace {

#ifdef _WIN64
constexpr int kSlotUpdate = 3, kSlotRenderBegin = 4, kSlotRender = 5, kSlotRenderEnd = 6;  // ISystem
// the timed parts: the phases, then optiperf's work at the flush's entry, then the flush itself
constexpr int kPrework = kPhases, kFlush = kPhases + 1, kParts = kPhases + 2;
constexpr uint64_t kLongInterval = 750000000;  // cycles: 250 ms
const char* const kNames[kParts] = {"other",          "RenderBegin",     "Update",
                                    "game",           "the traversal",   "Render after the flush",
                                    "after Render",   "RenderEnd",       "after RenderEnd",
                                    "optiperf before the flush",         "flush"};
// the report's order: a frame's
const int kOrder[] = {kPhaseBegin, kPhaseGame, kPhaseUpdate, kPhaseWorld, kPrework, kFlush,
                      kPhaseTail,  kPhasePost, kPhaseEnd,    kPhaseAfter};

using VoidFn = void (*)(void*);
using UpdateFn = bool (*)(void*, int, int);

struct Interval {
    uint64_t cycles[kParts];
    uint64_t frames;
};

struct State {
    bool installed = false, setupTried = false, ready = false;
    int entries = 0;  // outer flushes entered before the hooks went in
    void** system = nullptr;
    DWORD mainThread = 0;
    void* sysOrig[4] = {};  // ISystem slots 3..6 as they were (the function, or the tracer's thunk)
    int part = kPhaseOther, beforeFlush = kPhaseWorld;
    uint64_t enter = 0;
    uint64_t boundaries = 0;  // outer flushes started since the hooks went in
    Interval cur = {};        // the interval running
    Interval sum = {};        // the intervals kept
    uint64_t intervals = 0, longIntervals = 0, longCycles = 0;
    uint64_t serial = 0, overlap = 0, overlapEnd = 0, buffered = 0, bufferedEnd = 0;  // cycles, per the models above
};
State g;

bool OnMain() { return GetCurrentThreadId() == g.mainThread; }

// the main thread moves to part p; its time since the last move goes to the part it leaves
void Enter(int p) {
    const uint64_t now = __rdtsc();
    if (g.enter) g.cur.cycles[g.part] += now - g.enter;
    g.enter = now;
    g.part = p;
    CallTrace_SetPhase(p >= kPhases ? kPhaseWorld : p);  // (the flush has the tracer's own flag)
}

// an interval ends (the next outer flush starts): kept if it is whole and not a load or a hitch
void EndInterval() {
    if (++g.boundaries >= 3) {
        const Interval& c = g.cur;
        uint64_t total = 0;
        for (uint64_t v : c.cycles) total += v;
        if (total > kLongInterval) {
            ++g.longIntervals;
            g.longCycles += total;
        } else {
            for (int p = 0; p < kParts; ++p) g.sum.cycles[p] += c.cycles[p];
            g.sum.frames += c.frames;
            ++g.intervals;
            const uint64_t a = c.cycles[kPhaseWorld] + c.cycles[kPrework], b = c.cycles[kFlush],
                           e = c.cycles[kPhaseEnd], rest = total - a - b;
            g.serial += total;
            g.overlap += a + std::max(b, rest);
            g.overlapEnd += a + std::max(b + e, rest - e);
            g.buffered += std::max(b, a + rest);
            g.bufferedEnd += std::max(b + e, a + rest - e);
        }
    }
    g.cur = {};
}

void RenderBeginHook(void* self) {
    const auto fn = reinterpret_cast<VoidFn>(g.sysOrig[kSlotRenderBegin - kSlotUpdate]);
    if (!OnMain()) return fn(self);
    Enter(kPhaseBegin);
    ++g.cur.frames;
    fn(self);
    Enter(kPhaseGame);
}

bool UpdateHook(void* self, int flags, int pause) {
    const auto fn = reinterpret_cast<UpdateFn>(g.sysOrig[0]);
    if (!OnMain()) return fn(self, flags, pause);
    const int was = g.part;
    Enter(kPhaseUpdate);
    const bool r = fn(self, flags, pause);
    Enter(was);
    return r;
}

void RenderHook(void* self) {
    const auto fn = reinterpret_cast<VoidFn>(g.sysOrig[kSlotRender - kSlotUpdate]);
    if (!OnMain()) return fn(self);
    Enter(kPhaseWorld);
    fn(self);
    Enter(kPhasePost);
}

void RenderEndHook(void* self) {
    const auto fn = reinterpret_cast<VoidFn>(g.sysOrig[kSlotRenderEnd - kSlotUpdate]);
    if (!OnMain()) return fn(self);
    Enter(kPhaseEnd);
    fn(self);
    Enter(kPhaseAfter);
}

bool PatchSlot(void** slot, void* fn, void** old) {
    DWORD protect;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
    *old = *slot;
    InterlockedExchangePointer(slot, fn);
    VirtualProtect(slot, sizeof(void*), protect, &protect);
    return true;
}

// At the second flush's entry: CSystem's vtable, from the object, checked to lead into CrySystem's code, then patched.
void Setup() {
    g.setupTried = true;
    g.mainThread = GetCurrentThreadId();
    void* system = g.system ? *g.system : nullptr;
    void** vt = system ? *static_cast<void***>(system) : nullptr;
    HMODULE m = GetModuleHandleW(L"CrySystem.dll");
    bool ok = vt && m;
    for (int i = kSlotUpdate; ok && i <= kSlotRenderEnd; ++i)
        ok = IsInCode(reinterpret_cast<char*>(m), static_cast<const char*>(CallTrace_Original(vt[i])), 1);
    if (!ok) {
        Log("frame phases: off: CSystem's vtable is not the expected one");
        return;
    }
    void* const hooks[4] = {reinterpret_cast<void*>(&UpdateHook), reinterpret_cast<void*>(&RenderBeginHook),
                            reinterpret_cast<void*>(&RenderHook), reinterpret_cast<void*>(&RenderEndHook)};
    for (int i = 0; i < 4; ++i)
        if (!PatchSlot(&vt[kSlotUpdate + i], hooks[i], &g.sysOrig[i])) {
            for (int j = 0; j < i; ++j) {
                void* ignored;
                PatchSlot(&vt[kSlotUpdate + j], g.sysOrig[j], &ignored);
            }
            Log("frame phases: off: could not write CSystem's vtable");
            return;
        }
    g.ready = true;
    g.part = kPhaseWorld;  // (this flush is inside a Render the hook didn't see begin)
    g.enter = __rdtsc();
    Log("frame phases: on: ISystem::RenderBegin, Update, Render and RenderEnd timed on the main thread");
}

double Ms(uint64_t cycles, double frames) { return double(cycles) / 3.0e6 / frames; }

void Report(const char* when) {
    const double f = double(g.sum.frames ? g.sum.frames : 1);
    char line[1024] = "";
    int n = 0;
    for (int p : kOrder) {
        n += snprintf(line + n, sizeof line - size_t(n), "%s%s %.3f", p == kOrder[0] ? "" : ", ", kNames[p],
                      Ms(g.sum.cycles[p], f));
        if (n >= int(sizeof line)) break;
    }
    const double serial = Ms(g.serial, f);
    Log("frame phases [%s]: %llu frames in %llu intervals kept (%llu over 250 ms left out: %.1f s) | per frame (ms): %s "
        "| total %.3f (%.1f fps) | outer flushes per frame %.2f",
        when, g.sum.frames, g.intervals, g.longIntervals, double(g.longCycles) / 3.0e9, line, serial,
        serial > 0 ? 1000.0 / serial : 0.0, double(g.intervals) / f);
    const auto fps = [](double ms) { return ms > 0 ? 1000.0 / ms : 0.0; };
    const double o = Ms(g.overlap, f), oe = Ms(g.overlapEnd, f), b = Ms(g.buffered, f), be = Ms(g.bufferedEnd, f);
    Log("frame phases [%s]: per frame with the flush on a render thread (estimates from each interval's parts, ms): "
        "beside the rest of the main thread's frame, barrier before the traversal %.3f (%.1f fps), the present there too "
        "%.3f (%.1f fps) | beside the traversal as well %.3f (%.1f fps), the present there too %.3f (%.1f fps)",
        when, o, fps(o), oe, fps(oe), b, fps(b), be, fps(be));
}
#endif  // _WIN64

}  // namespace

void FramePhases_Install(const wchar_t* moduleName, void** system) {
#ifndef _WIN64
    (void)system;
    if (ConfigString(L"Debug", L"FramePhases", L"OPTIPERF_FRAME_PHASES", L"off") != L"off")
        Log("%ls: frame phases not timed: 64-bit game only", moduleName);
#else
    if (g.installed) return;
    const std::wstring v = ConfigString(L"Debug", L"FramePhases", L"OPTIPERF_FRAME_PHASES", L"off");
    if (!(_wcsicmp(v.c_str(), L"on") == 0 || v == L"1")) return;
    if (!system) {
        Log("%ls: frame phases not timed: no ISystem", moduleName);
        return;
    }
    g.system = system;
    g.installed = true;
    Log("%ls: frame phases: on (the hooks go in at the second flush)", moduleName);
#endif
}

void FramePhases_FlushEntry() {
#ifdef _WIN64
    if (!g.installed) return;
    if (!g.setupTried && ++g.entries >= 2) Setup();  // (see the top)
    if (!g.ready || !OnMain()) return;
    g.beforeFlush = g.part;
    Enter(kPrework);
#endif
}

void FramePhases_FlushStart() {
#ifdef _WIN64
    if (!g.ready || !OnMain()) return;
    Enter(kFlush);
    EndInterval();
#endif
}

void FramePhases_FlushEnd() {
#ifdef _WIN64
    if (g.ready && OnMain()) Enter(g.beforeFlush == kPhaseWorld ? kPhaseTail : g.beforeFlush);
#endif
}

void FramePhases_Report(const char* when) {
#ifdef _WIN64
    if (g.ready) Report(when);
#else
    (void)when;
#endif
}

}  // namespace optiperf
