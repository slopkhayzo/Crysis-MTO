// System snapshot: Phase 2 of research/docs/cpu-multithreading-plan.md, after the skinning poses (details in
// research/docs/system-snapshot.md). x64 only.
//
// Most of what the flush still calls outside the renderer are CrySystem's getters: ISystem::GetI3DEngine before each
// of the 3D engine's getters (850 calls per frame on village), the timer's frame time, time scale and the frame's
// time (about 180), the ITimer pointer, the editor flags, the view camera. They read fields CrySystem sets before the
// flush: the timer in UpdateOnFrameStart, the view camera in SetViewCamera. A render thread can't read them while the
// main thread starts the next frame.
//   - on: right before the flush (last: after the environment snapshot's capture) each getter is called once (the
//     per-timer ones for both timers, the view camera copied), and the CSystem and CTimer objects' vtable pointers are
//     swapped for counting copies (vtable_counter.h) whose getter slots answer from that capture until the flush
//     returns. The real-time clock (GetAsyncCurTime: the timer's tick source, less a base, over the frequency) is
//     answered with the base and frequency the capture took, its tick source called at the call: the timer's
//     UpdateOnFrameStart moves that base every frame, which on a render thread would be the next frame's while the
//     flush still reads it. Calls from other threads, GetAsyncTime (which only CrySystem's own code calls), a timer
//     outside the timers' range, and every call after one of the setters ran inside the flush (the timer's frame
//     start, time scale, pause, enabling and reset; SetViewCamera; the system's update: their slots set a marker) go to
//     CrySystem.
//   - verify: as on, but each call it would answer also goes to CrySystem, whose result is compared and returned.
// Each getter's code is checked by pattern at the first flush (the whole function, or IsEditorMode's start); one whose
// code differs is left to CrySystem.
//
// [ShadowPrepass] System = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_SYSTEM)

#include "sys_snapshot.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>

#include "calltrace.h"
#include "config.h"
#include "diag.h"
#include "flush_thread.h"
#include "log.h"
#include "scan.h"
#include "vtable_counter.h"

namespace optiperf {
namespace {

enum class YMode { Off, On, Verify };

YMode ReadMode() {
    const std::wstring v = ConfigString(L"ShadowPrepass", L"System", L"OPTIPERF_SHADOW_PREPASS_SYSTEM", L"off");
    if (_wcsicmp(v.c_str(), L"on") == 0 || v == L"1") return YMode::On;
    if (_wcsicmp(v.c_str(), L"verify") == 0) return YMode::Verify;
    return YMode::Off;
}

#ifdef _WIN64
const char* ModeName(YMode m) { return m == YMode::On ? "on" : m == YMode::Verify ? "verify" : "off"; }

// How a getter returns its answer (x64; `self` is the object).
enum class Kind : uint8_t {
    Bool,        // bool fn(self)
    Ptr,         // void* fn(self)
    Camera,      // const CCamera& fn(self): answered with a copy
    Float,       // float fn(self)
    FloatTimer,  // float fn(self, ETimer): per timer
    TimeTimer,   // CTimeValue fn(self, ETimer): into a hidden pointer, which it returns; per timer
    Clock,       // float fn(self): the real-time clock, (ticks() - base) / frequency
};

struct Def {
    int object;
    int slot;
    const char* name;
    Kind kind;
    const char* code;  // the whole function, or its start
};

// --- patterns (x64 CrySystem.dll, build 6156): the ISystem and ITimer slots (Mod SDK order), CSystem's and CTimer's
// functions there ---------------------------------------------------------------------------------------------------
constexpr Def kDefs[kSysGetters] = {
    {kSysSystem, 23, "IsDedicated", Kind::Bool, "8A 81 FB 05 00 00 C3"},
    // the game framework's editing state, else IsEditor (slot 24)
    {kSysSystem, 25, "IsEditorMode", Kind::Bool,
     "40 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 83 79 08 00 74 53 48 8B 49 08 48 8B 01 FF 50 "
     "48 48 85 C0 74 44 48 8B 03 48 8B CB FF 90 C0 00 00 00"},
    {kSysSystem, 28, "GetI3DEngine", Kind::Ptr, "48 8B 41 50 C3"},
    {kSysSystem, 57, "GetITimer", Kind::Ptr, "48 8B 41 38 C3"},
    {kSysSystem, 80, "GetViewCamera", Kind::Camera, "48 8D 81 D8 03 00 00 C3"},
    // the frame's start, per timer (+0x40, ticks), in seconds
    {kSysTimer, 2, "GetCurrTime", Kind::FloatTimer, "48 63 C2 F3 48 0F 2A 44 C1 40 F3 0F 59 05 ?? ?? ?? ?? C3"},
    {kSysTimer, 3, "GetFrameStartTime", Kind::TimeTimer, "49 63 C0 48 8B 4C C1 40 48 8B C2 48 89 0A C3"},
    // the real-time clock: the tick source (+0x8, a function pointer), less the base (+0x10), over the frequency (+0x28)
    {kSysTimer, 5, "GetAsyncCurTime", Kind::Clock,
     "40 53 48 83 EC 20 48 8B D9 FF 51 08 F2 48 0F 2A 43 28 48 2B 43 10 F2 48 0F 2A C8 F2 0F 5E C8 F2 0F 5A C1 48 83 "
     "C4 20 5B C3"},
    // the frame's length (+0x38), 0 while the timer is off (+0x60) or the game timer paused (+0x270)
    {kSysTimer, 6, "GetFrameTime", Kind::FloatTimer,
     "80 79 60 00 75 04 0F 57 C0 C3 85 D2 75 08 38 91 70 02 00 00 75 F0 F3 0F 10 41 38 C3"},
    {kSysTimer, 7, "GetRealFrameTime", Kind::Float, "80 79 60 00 75 04 0F 57 C0 C3 F3 0F 10 41 3C C3"},
    {kSysTimer, 8, "GetTimeScale", Kind::Float, "F3 0F 10 81 4C 02 00 00 C3"},
};
constexpr int kTimers = 2;  // ETimer: the game's and the UI's (the per-timer fields hold two)
constexpr int kClockTicks = 0x8, kClockBase = 0x10, kClockFrequency = 0x28;  // (fixed by GetAsyncCurTime's pattern)
// Slots whose call inside the flush, on its thread, makes the snapshot out of date (another thread's call, such as the
// main thread's next frame beside a render thread's flush, leaves it): the system's update and SetViewCamera; the timer's
// reset, frame start, time scale, enabling, serialization, pause and SetTimer.
constexpr int kSystemStale[] = {3, 79};
constexpr int kTimerStale[] = {0, 1, 9, 10, 14, 15, 17};
constexpr int kMaxSlots = 256;
constexpr size_t kCameraBytes = 0x214;  // CCamera, as its assignment copies it

const char* const kObjectNames[kSysObjects] = {"ISystem", "ITimer"};
struct SlotName {
    int object, slot;
    const char* name;
};
constexpr SlotName kSlotNames[] = {
    {kSysSystem, 3, "Update"},          {kSysSystem, 23, "IsDedicated"},     {kSysSystem, 24, "IsEditor"},
    {kSysSystem, 25, "IsEditorMode"},   {kSysSystem, 26, "GetIConsole"},     {kSysSystem, 28, "GetI3DEngine"},
    {kSysSystem, 37, "GetIPak"},        {kSysSystem, 43, "GetIProfileSystem"}, {kSysSystem, 55, "GetIRenderer"},
    {kSysSystem, 57, "GetITimer"},      {kSysSystem, 79, "SetViewCamera"},   {kSysSystem, 80, "GetViewCamera"},
    {kSysTimer, 0, "ResetTimer"},       {kSysTimer, 1, "UpdateOnFrameStart"}, {kSysTimer, 2, "GetCurrTime"},
    {kSysTimer, 3, "GetFrameStartTime"}, {kSysTimer, 4, "GetAsyncTime"},     {kSysTimer, 5, "GetAsyncCurTime"},
    {kSysTimer, 6, "GetFrameTime"},     {kSysTimer, 7, "GetRealFrameTime"},  {kSysTimer, 8, "GetTimeScale"},
    {kSysTimer, 9, "SetTimeScale"},     {kSysTimer, 10, "EnableTimer"},      {kSysTimer, 11, "IsTimerEnabled"},
    {kSysTimer, 12, "GetFrameRate"},    {kSysTimer, 15, "PauseTimer"},       {kSysTimer, 16, "IsTimerPaused"},
};

const char* SlotLabel(int object, int slot) {
    for (const SlotName& s : kSlotNames)
        if (s.object == object && s.slot == slot) return s.name;
    return "";
}

struct Getter {
    void* orig = nullptr;  // the vtable's entry: CrySystem's function (or the tracer's thunk in front of it)
    bool hooked = false;
    uint64_t v[kTimers] = {};  // the captured answer(s), as raw bits (per timer for the per-timer ones)
    // main-thread calls inside the flush: answered; to CrySystem as out of date, or for another timer; verify: compared
    uint64_t answered = 0, passed = 0, aside = 0, same = 0, differ = 0;
};

struct YState {
    YMode mode = YMode::Off;
    bool installed = false, setupTried = false, ready = false, verify = false;
    bool swapped = false;  // the vtable copies are in (from the capture to the flush's end)
    bool live = false;     // ... and the snapshot is still up to date
    void** system = nullptr;
    void* object[kSysObjects] = {};
    DWORD mainThread = 0;
    VtableCounter counter[kSysObjects];
    Getter get[kSysGetters];
    uint8_t camera[kCameraBytes] = {};  // GetViewCamera's answer: the view camera at the capture
    struct {
        int64_t (*ticks)() = nullptr;  // the timer's tick source
        int64_t base = 0, frequency = 0;
    } clock;  // GetAsyncCurTime's: at the capture
    std::atomic<uint64_t> otherThread{0};
    uint64_t flushes = 0, captureCycles = 0, stale = 0;
    int logged = 0, loggedStale = 0;
    bool loggedFirst = false;
};
YState g;

inline uint64_t Bits(float f) {
    uint32_t w;
    std::memcpy(&w, &f, 4);
    return w;
}
inline float AsFloat(uint64_t w) {
    const uint32_t u = uint32_t(w);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

void Append(char* line, size_t size, size_t& n, const char* fmt, ...) {
    if (n >= size) return;
    va_list a;
    va_start(a, fmt);
    const int k = vsnprintf(line + n, size - n, fmt, a);
    va_end(a);
    if (k > 0) n = (std::min)(size - 1, n + size_t(k));
}

bool Marked() { return g.counter[kSysSystem].Marked() || g.counter[kSysTimer].Marked(); }

// The snapshot just went out of date: the rest of this flush asks CrySystem.
void GoneStale() {
    g.live = false;
    ++g.stale;
    if (++g.loggedStale <= 10)
        Log("system snapshot: flush %llu: out of date after a call of one of the setters: the rest of the flush asks "
            "CrySystem", g.flushes);
}

// the main thread or the flush's (they differ with FlushThread=worker); other threads' calls go to CrySystem, counted
bool OnMain() {
    const DWORD t = GetCurrentThreadId();
    if (t == g.mainThread || t == FlushThread()) return true;
    if (!g_vtableCountingOff) g.otherThread.fetch_add(1, std::memory_order_relaxed);  // (not in a particle batch)
    return false;
}

// Whether a call is answered from the snapshot: on the flush's thread while it is live (else it goes to CrySystem,
// counted).
bool Answers(Getter& x) {
    if (g.live && Marked()) GoneStale();
    if (g.live && OnFlushThread()) return true;
    ++x.passed;
    return false;
}

// verify: CrySystem's answer against the snapshot's
void Check(Getter& x, int i, uint64_t live, uint64_t snap, int timer, void* from) {
    if (live == snap) {
        ++x.same;
        return;
    }
    ++x.differ;
    if (++g.logged > 20) return;
    char where[160] = "";
    DescribeAddress(from, where, sizeof where);
    char what[32] = "";
    if (timer >= 0) snprintf(what, sizeof what, "(%d)", timer);
    const Kind k = kDefs[i].kind;
    if (k == Kind::Float || k == Kind::FloatTimer)
        Log("system snapshot: VERIFY flush %llu: %s::%s%s differs (called from %s): snapshot %.9g [%08llX], CrySystem "
            "%.9g [%08llX]",
            g.flushes, kObjectNames[kDefs[i].object], kDefs[i].name, what, where, double(AsFloat(snap)), snap,
            double(AsFloat(live)), live);
    else
        Log("system snapshot: VERIFY flush %llu: %s::%s%s differs (called from %s): snapshot 0x%llX, CrySystem 0x%llX",
            g.flushes, kObjectNames[kDefs[i].object], kDefs[i].name, what, where, snap, live);
}

// --- the getters' slots in the vtable copies --------------------------------------------------------------------------
template <int I>
bool BoolHook(void* self) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<bool (*)(void*)>(x.orig);
    if (!OnMain() || !Answers(x)) return fn(self);
    if (g.verify) {
        const bool r = fn(self);
        Check(x, I, r ? 1 : 0, x.v[0], -1, _ReturnAddress());
        return r;
    }
    ++x.answered;
    return x.v[0] != 0;
}

template <int I>
void* PtrHook(void* self) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<void* (*)(void*)>(x.orig);
    if (!OnMain() || !Answers(x)) return fn(self);
    if (g.verify) {
        void* r = fn(self);
        Check(x, I, uint64_t(uintptr_t(r)), x.v[0], -1, _ReturnAddress());
        return r;
    }
    ++x.answered;
    return reinterpret_cast<void*>(uintptr_t(x.v[0]));
}

// the view camera: the copy made at the capture (verify: the live camera's bytes against it)
template <int I>
const void* CameraHook(void* self) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<const void* (*)(void*)>(x.orig);
    if (!OnMain() || !Answers(x)) return fn(self);
    if (g.verify) {
        const void* r = fn(self);
        const bool same = std::memcmp(r, g.camera, kCameraBytes) == 0;
        Check(x, I, same ? 0 : 1, 0, -1, _ReturnAddress());
        return r;
    }
    ++x.answered;
    return g.camera;
}

template <int I>
float FloatHook(void* self) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<float (*)(void*)>(x.orig);
    if (!OnMain() || !Answers(x)) return fn(self);
    if (g.verify) {
        const float r = fn(self);
        Check(x, I, Bits(r), x.v[0], -1, _ReturnAddress());
        return r;
    }
    ++x.answered;
    return AsFloat(x.v[0]);
}

template <int I>
float FloatTimerHook(void* self, int timer) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<float (*)(void*, int)>(x.orig);
    if (!OnMain()) return fn(self, timer);
    if (timer < 0 || timer >= kTimers) {
        ++x.aside;
        return fn(self, timer);
    }
    if (!Answers(x)) return fn(self, timer);
    if (g.verify) {
        const float r = fn(self, timer);
        Check(x, I, Bits(r), x.v[timer], timer, _ReturnAddress());
        return r;
    }
    ++x.answered;
    return AsFloat(x.v[timer]);
}

template <int I>
int64_t* TimeTimerHook(void* self, int64_t* out, int timer) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<int64_t* (*)(void*, int64_t*, int)>(x.orig);
    if (!OnMain()) return fn(self, out, timer);
    if (timer < 0 || timer >= kTimers) {
        ++x.aside;
        return fn(self, out, timer);
    }
    if (!Answers(x)) return fn(self, out, timer);
    if (g.verify) {
        int64_t* r = fn(self, out, timer);
        Check(x, I, uint64_t(*out), x.v[timer], timer, _ReturnAddress());
        return r;
    }
    ++x.answered;
    *out = int64_t(x.v[timer]);
    return out;
}

// the real-time clock: CrySystem's computation, with the capture's base and frequency (verify: those it would read)
template <int I>
float ClockHook(void* self) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<float (*)(void*)>(x.orig);
    if (!OnMain() || !Answers(x)) return fn(self);
    if (g.verify) {
        const char* t = static_cast<const char*>(self);
        int64_t (*ticks)();
        int64_t base, frequency;
        std::memcpy(&ticks, t + kClockTicks, 8);
        std::memcpy(&base, t + kClockBase, 8);
        std::memcpy(&frequency, t + kClockFrequency, 8);
        const bool same = ticks == g.clock.ticks && base == g.clock.base && frequency == g.clock.frequency;
        Check(x, I, same ? 0 : 1, 0, -1, _ReturnAddress());
        return fn(self);
    }
    ++x.answered;
    return float(double(g.clock.ticks() - g.clock.base) / double(g.clock.frequency));
}

template <int I>
void* HookFor() {
    constexpr Kind k = kDefs[I].kind;
    if constexpr (k == Kind::Bool) return reinterpret_cast<void*>(&BoolHook<I>);
    else if constexpr (k == Kind::Ptr) return reinterpret_cast<void*>(&PtrHook<I>);
    else if constexpr (k == Kind::Camera) return reinterpret_cast<void*>(&CameraHook<I>);
    else if constexpr (k == Kind::Float) return reinterpret_cast<void*>(&FloatHook<I>);
    else if constexpr (k == Kind::FloatTimer) return reinterpret_cast<void*>(&FloatTimerHook<I>);
    else if constexpr (k == Kind::Clock) return reinterpret_cast<void*>(&ClockHook<I>);
    else return reinterpret_cast<void*>(&TimeTimerHook<I>);
}

template <size_t... I>
void HookTable(void** out, std::index_sequence<I...>) {
    ((out[I] = HookFor<int(I)>()), ...);
}

// --- the capture, right before the flush ----------------------------------------------------------------------------
void Capture() {
    const uint64_t t0 = __rdtsc();
    for (int i = 0; i < kSysGetters; ++i) {
        Getter& x = g.get[i];
        if (!x.hooked) continue;
        void* o = g.object[kDefs[i].object];
        void* fn = x.orig;
        switch (kDefs[i].kind) {
        case Kind::Bool: x.v[0] = reinterpret_cast<bool (*)(void*)>(fn)(o) ? 1 : 0; break;
        case Kind::Ptr: x.v[0] = uint64_t(uintptr_t(reinterpret_cast<void* (*)(void*)>(fn)(o))); break;
        case Kind::Camera: std::memcpy(g.camera, reinterpret_cast<const void* (*)(void*)>(fn)(o), kCameraBytes); break;
        case Kind::Float: x.v[0] = Bits(reinterpret_cast<float (*)(void*)>(fn)(o)); break;
        case Kind::FloatTimer:
            for (int t = 0; t < kTimers; ++t) x.v[t] = Bits(reinterpret_cast<float (*)(void*, int)>(fn)(o, t));
            break;
        case Kind::TimeTimer:
            for (int t = 0; t < kTimers; ++t) {
                int64_t v = 0;
                reinterpret_cast<int64_t* (*)(void*, int64_t*, int)>(fn)(o, &v, t);
                x.v[t] = uint64_t(v);
            }
            break;
        case Kind::Clock: {  // (the fields the clock reads: its pattern fixes them)
            const char* t = static_cast<const char*>(o);
            std::memcpy(&g.clock.ticks, t + kClockTicks, 8);
            std::memcpy(&g.clock.base, t + kClockBase, 8);
            std::memcpy(&g.clock.frequency, t + kClockFrequency, 8);
            x.v[0] = uint64_t(g.clock.base);
            x.v[1] = uint64_t(g.clock.frequency);
            break;
        }
        }
    }
    g.captureCycles += __rdtsc() - t0;
    if (g.loggedFirst) return;
    g.loggedFirst = true;
    char line[1024] = "";
    size_t n = 0;
    for (int i = 0; i < kSysGetters; ++i) {
        const Getter& x = g.get[i];
        if (!x.hooked) continue;
        switch (kDefs[i].kind) {
        case Kind::Bool: Append(line, sizeof line, n, " %s %d", kDefs[i].name, int(x.v[0])); break;
        case Kind::Ptr: Append(line, sizeof line, n, " %s %p", kDefs[i].name, reinterpret_cast<void*>(uintptr_t(x.v[0]))); break;
        case Kind::Camera: break;
        case Kind::Float: Append(line, sizeof line, n, " %s %g", kDefs[i].name, double(AsFloat(x.v[0]))); break;
        case Kind::FloatTimer:
            Append(line, sizeof line, n, " %s %g/%g", kDefs[i].name, double(AsFloat(x.v[0])), double(AsFloat(x.v[1])));
            break;
        case Kind::TimeTimer:
            Append(line, sizeof line, n, " %s %lld/%lld", kDefs[i].name, (long long)x.v[0], (long long)x.v[1]);
            break;
        case Kind::Clock:
            Append(line, sizeof line, n, " %s base %lld, frequency %lld", kDefs[i].name, (long long)x.v[0],
                   (long long)x.v[1]);
            break;
        }
    }
    Log("system snapshot: first capture:%s", line);
}

// At the first flush: the system and the timer, their vtables' getters checked, the vtable copies set up.
void Setup() {
    g.setupTried = true;
    void* system = *g.system;
    char* base = reinterpret_cast<char*>(GetModuleHandleW(L"CrySystem.dll"));
    if (!system || !base) {
        Log("system snapshot: off: no %s", system ? "CrySystem.dll" : "ISystem");
        return;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const size_t size = nt->OptionalHeader.SizeOfImage;
    void** svt = *static_cast<void***>(system);
    int sslots = 0;
    while (sslots < kMaxSlots && IsInCode(base, static_cast<const char*>(CallTrace_Original(svt[sslots])), 1)) ++sslots;
    // the timer, through the system's own GetITimer (checked first)
    void* timer = nullptr;
    char* getTimer = sslots > 57 ? static_cast<char*>(CallTrace_Original(svt[57])) : nullptr;
    const Pattern timerCode = ParsePattern(kDefs[3].code);
    if (getTimer && IsInCode(base, getTimer, timerCode.size()) && MatchesAt(getTimer, timerCode))
        timer = reinterpret_cast<void* (*)(void*)>(svt[57])(system);
    if (!timer) {
        Log("system snapshot: off: ISystem::GetITimer is not the expected code");
        return;
    }
    void** tvt = *static_cast<void***>(timer);
    int tslots = 0;
    while (tslots < kMaxSlots && IsInCode(base, static_cast<const char*>(CallTrace_Original(tvt[tslots])), 1)) ++tslots;
    SysGetter found[kSysGetters];
    const int n = SysSnapshot_Locate(base, svt, sslots, tvt, tslots, found);
    if (n <= 0) {
        Log("system snapshot: off: %s", n < 0 ? "a vtable is shorter than expected" : "none of the getters is as expected");
        return;
    }
    g.object[kSysSystem] = system;
    g.object[kSysTimer] = timer;
    const int slots[kSysObjects] = {sslots, tslots};
    for (int o = 0; o < kSysObjects; ++o)
        if (!g.counter[o].Init(g.object[o], base, size, slots[o]) || g.counter[o].Slots() != slots[o]) {
            Log("system snapshot: off: could not copy the %s vtable", kObjectNames[o]);
            return;
        }
    void* hooks[kSysGetters];
    HookTable(hooks, std::make_index_sequence<kSysGetters>{});
    char left[512] = "";
    size_t len = 0;
    for (int i = 0; i < kSysGetters; ++i) {
        if (!found[i].fn) {
            Append(left, sizeof left, len, " %s::%s", kObjectNames[kDefs[i].object], kDefs[i].name);
            continue;
        }
        Getter& x = g.get[i];
        VtableCounter& c = g.counter[kDefs[i].object];
        x.orig = c.Original(kDefs[i].slot);
        x.hooked = true;
        c.SetTarget(kDefs[i].slot, hooks[i]);
    }
    for (int s : kSystemStale) g.counter[kSysSystem].Mark(s);
    for (int s : kTimerStale) g.counter[kSysTimer].Mark(s);
    g.mainThread = GetCurrentThreadId();
    g.ready = true;
    Log("system snapshot: mode=%s: %d of %d getters answered from it (left to CrySystem:%s); vtables: CSystem %d slots "
        "(CrySystem.dll+0x%X), CTimer %d slots (+0x%X)",
        ModeName(g.mode), n, kSysGetters, n == kSysGetters ? " none" : left, sslots,
        unsigned(reinterpret_cast<char*>(svt) - base), tslots, unsigned(reinterpret_cast<char*>(tvt) - base));
}
#endif  // _WIN64

}  // namespace

int SysSnapshot_Locate(char* base, void* const* systemVtable, int systemSlots, void* const* timerVtable, int timerSlots,
                       SysGetter* out) {
#ifndef _WIN64
    (void)base;
    (void)systemVtable;
    (void)systemSlots;
    (void)timerVtable;
    (void)timerSlots;
    (void)out;
    return -1;
#else
    int need[kSysObjects] = {0, 0};
    for (int s : kSystemStale) need[kSysSystem] = (std::max)(need[kSysSystem], s);
    for (int s : kTimerStale) need[kSysTimer] = (std::max)(need[kSysTimer], s);
    for (const Def& d : kDefs) need[d.object] = (std::max)(need[d.object], d.slot);
    if (systemSlots <= need[kSysSystem] || timerSlots <= need[kSysTimer]) return -1;
    void* const* vts[kSysObjects] = {systemVtable, timerVtable};
    int n = 0;
    for (int i = 0; i < kSysGetters; ++i) {
        const Def& d = kDefs[i];
        out[i] = SysGetter{d.object, d.slot, d.name, nullptr};
        char* fn = static_cast<char*>(CallTrace_Original(vts[d.object][d.slot]));
        const Pattern code = ParsePattern(d.code);
        if (!IsInCode(base, fn, code.size()) || !MatchesAt(fn, code)) continue;
        out[i].fn = fn;
        ++n;
    }
    return n;
#endif
}

void SysSnapshot_Install(const wchar_t* moduleName, void** system) {
#ifndef _WIN64
    (void)system;
    if (ReadMode() != YMode::Off) Log("%ls: system snapshot not applied: 64-bit game only", moduleName);
#else
    if (g.installed) return;
    g.mode = ReadMode();
    if (g.mode == YMode::Off) return;
    if (!system) {
        Log("%ls: system snapshot not applied: no ISystem", moduleName);
        return;
    }
    g.system = system;
    g.verify = g.mode == YMode::Verify;
    g.installed = true;
    Log("%ls: system snapshot: mode=%s (set up at the first flush)", moduleName, ModeName(g.mode));
#endif
}

void SysSnapshot_BeforeFlush() {
#ifdef _WIN64
    if (!g.installed) return;
    if (!g.setupTried) Setup();
    if (!g.ready || g.swapped || GetCurrentThreadId() != g.mainThread) return;
    ++g.flushes;
    Capture();
    for (VtableCounter& c : g.counter) {
        c.ClearMarker();
        c.SetMarkThread(FlushThread());  // (the flush's own setter calls make it out of date, not another thread's)
    }
    g.live = g.swapped = true;
    for (VtableCounter& c : g.counter) c.Begin();
#endif
}

void SysSnapshot_AfterFlush() {
#ifdef _WIN64
    if (!g.swapped) return;
    for (VtableCounter& c : g.counter) c.End();
    g.live = g.swapped = false;
#endif
}

void SysSnapshot_Report(const char* when) {
#ifndef _WIN64
    (void)when;
#else
    if (!g.installed || !g.ready) return;
    uint64_t answered = 0, passed = 0, aside = 0, same = 0, differ = 0;
    char line[2048] = "";
    size_t n = 0;
    for (int i = 0; i < kSysGetters; ++i) {
        const Getter& x = g.get[i];
        if (!x.hooked) continue;
        answered += x.answered;
        passed += x.passed;
        aside += x.aside;
        same += x.same;
        differ += x.differ;
        if (!(x.answered | x.passed | x.aside | x.same | x.differ)) continue;
        if (g.verify)
            Append(line, sizeof line, n, " %s %llu/%llu/%llu", kDefs[i].name, x.same, x.differ, x.passed + x.aside);
        else
            Append(line, sizeof line, n, " %s %llu/%llu", kDefs[i].name, x.answered, x.passed + x.aside);
    }
    const uint64_t asked = g.verify ? same + differ : answered;
    Log("system [%s]: mode=%s | flushes=%llu, capture %.4f ms each | out of date in %llu | calls %s=%llu; to CrySystem: "
        "out of date %llu, another timer %llu, from other threads %llu",
        when, ModeName(g.mode), g.flushes, double(g.captureCycles) / 3.0e6 / double(g.flushes ? g.flushes : 1), g.stale,
        g.verify ? "compared" : "answered", asked, passed, aside, g.otherThread.load(std::memory_order_relaxed));
    Log("system [%s]: per getter (%s):%s", when, g.verify ? "same/differ/to CrySystem" : "answered/to CrySystem", line);
    if (g.verify) Log("system [%s]: VERIFY calls=%llu: same=%llu differ=%llu", when, same + differ, same, differ);
    // every slot called through the copies (only in place during the flushes): from other modules / from CrySystem
    for (int o = 0; o < kSysObjects; ++o) {
        n = 0;
        line[0] = 0;
        for (int i = 0; i < g.counter[o].Slots(); ++i) {
            const uint64_t out = g.counter[o].Outside(i), in = g.counter[o].Inside(i);
            if (out || in) Append(line, sizeof line, n, " %d %s:%llu/%llu", i, SlotLabel(o, i), out, in);
        }
        Log("system [%s]: %s vtable calls inside the flushes (slot name:from other modules/from CrySystem):%s", when,
            kObjectNames[o], n ? line : " none");
    }
#endif
}

}  // namespace optiperf
