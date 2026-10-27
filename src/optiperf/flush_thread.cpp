// The outer flush's thread: see flush_thread.h; why, and what the runs found, in research/docs/render-thread.md §10–§11.
//
// The worker starts at the first outer flush (on the main thread, outside any loader lock), with a larger stack than
// the main thread's (1 MB, from Crysis.exe's header) and the main thread's priority. Each flush, the main thread hands
// it the function with what of the main thread's own state the flush can see, signals it and waits:
//   - MXCSR: rounding, denormals (the particle batch's index reads DAZ) and the exception masks. Copied there and back.
//   - The C runtime's random seed. msvcr80's rand keeps it in the thread's _getptd() block (+0x1C, read from rand's
//     code), and the renderer calls rand (the post effects' noise among others): the flush continues the main thread's
//     sequence, and the main thread continues from where the flush left it, as on one thread. The CRT is the one the
//     renderer imports rand from.
//   - The call tracer's flush mark and frame phase (thread-local in calltrace.cpp).
// What else could be kept per thread: the renderer has no TLS of its own and calls GetCurrentThreadId only for the
// CRT's security cookie; the locale is the process's (Crysis.exe's CRT start-up); errno and the CRT's other per-thread
// state only report. The 3D engine only records thread ids (no branch on them); CrySystem's one TLS user is
// CUNIXConsole::IsInputThread.
// A flush still running after 5 s is logged with where the worker is (its instruction and its stack's game call chain,
// read while it is suspended), and messages sent to the main thread are then let through (a SendMessage from the flush
// to the game's window waits for the main thread, which waits for the flush).
//
// overlap: the main thread doesn't wait. The renderer's gate (render_gate.h, installed with the setting) holds its
// first call into the renderer, joins the flush there (waits for it, then runs the caller's after-flush function) and
// lets the call go on; the next outer flush joins too, if nothing did. MXCSR and the seed go to the worker but don't
// come back: the main thread has gone on with its own. The report gives how long the main thread ran beside the flush,
// how long it then waited, and which calls joined.
//
// FlushThreadPresent=on (with overlap): the frame's end on the worker. At the main thread's EndFrame (the present,
// which the render queue takes beside the flush) the calls taken so far are sealed: the HUD's draws, the text, the
// frame's end. The worker makes them right after the flush, while the main thread goes on into its next update; a
// join waits for them too. If the main thread joins before it seals them (a call that can't be taken), the join makes
// them, as before. What the join compares the flush's end with is copied right after the flush, before the frame's end
// changes it. While the worker makes the frame's end, a join's wait lets messages sent to the main thread through at
// once: a present away from the window's thread may wait for the window's thread to take one (DXGI's). And a join from
// inside the game window's procedure (hooked) waits for it at most 2 s: DXGI's own handling of the window may run the
// procedure holding DXGI's lock, which the worker's present waits for.
#include "flush_thread.h"

#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "allocator.h"
#include "calltrace.h"
#include "config.h"
#include "diag.h"
#include "hooks.h"
#include "log.h"
#include "render_gate.h"
#include "render_queue.h"
#include "scan.h"
#include "shadow_prepass.h"
#include "traversal_record.h"

namespace optiperf {
namespace {

enum class FMode { Main, Worker, Overlap };

#ifdef _WIN64
constexpr SIZE_T kStackBytes = SIZE_T(8) << 20;  // reserved for the worker's stack
constexpr DWORD kStuckMs = 5000;                  // a flush running longer than this is logged
constexpr uint64_t kMaxStuckLogs = 8;
constexpr DWORD kWatchMs = 10000;  // no flush handed off for this long (after the first): the main thread's place logged
constexpr int kMaxMainLogs = 4;
constexpr UINT kHangExitCode = 0x0F7E0001;  // (FlushThreadHangExit: the process's exit code)
// msvcr80!rand: sub rsp,28h; call _getptd; mov ecx,[rax+seed]; imul ecx,ecx,343FDh
constexpr char kRandCode[] = "48 83 EC 28 E8 ?? ?? ?? ?? 8B 48 ?? 69 C9 FD 43 03 00";
constexpr int kRandCall = 4, kRandSeed = 11;
constexpr unsigned kMxcsrFlags = 0x3F;  // MXCSR's sticky exception flags (the rest is control)
// joins not made at a renderer call's thunk: the next flush, the report, the render queue's request
constexpr unsigned kJoinNextFlush = 0xFFFFFFFFu, kJoinReport = 0xFFFFFFFEu, kJoinQueue = 0xFFFFFFFDu,
                   kJoinPin = 0xFFFFFFFCu;  // (the allocator's: a main-thread realloc of a pinned block)
constexpr int kTopReasons = 10;
constexpr uint64_t kObserveEvery = 8;  // FlushThreadObserve: one join in this many observed for the frame's rest
// FlushThreadPresent: the frame's end, open (the main thread takes its calls), sealed (the worker's to make after the
// flush), or joined (the join makes them)
constexpr LONG kFrameEndOpen = 0, kFrameEndSealed = 1, kFrameEndJoined = 2;
// a join asked for from the window's procedure, in a join's wait for the frame's end: how long it waits (taking messages
// sent to the main thread) before its call is made beside the frame's end, the worker taken to be waiting for it
constexpr DWORD kNestedWaitMs = 2000;
#endif

struct Stats {
    uint64_t flushes = 0;                      // on the worker
    uint64_t flushCycles = 0;                  // the flush itself
    uint64_t startCycles = 0, maxStart = 0;    // the signal to the worker's start
    uint64_t backCycles = 0, maxBack = 0;      // the worker's end to the main thread's return (worker)
    uint64_t priority = 0;                     // the main thread's priority changed (the worker's follows)
    uint64_t mxcsrChanged = 0;                 // flushes that came back with other MXCSR control bits
    uint64_t stuck = 0, sent = 0;              // waits past kStuckMs; sent messages let through then
    uint64_t joinsSpun = 0, joinsSlept = 0;    // overlap: joins of a running flush that ended in the spin, or not
    // overlap: the join's work after the wait: the parts' steps after the flush, the pinned blocks' frees, the
    // queue's calls, the recorder's record
    uint64_t joinAfter = 0, joinPins = 0, joinQueue = 0, joinRecord = 0;
    uint64_t observedFrames = 0;  // FlushThreadObserve: the frames whose rest after a join was observed
    // FlushThreadPresent: the frames' ends made on the worker, or by the join (not sealed in time); the seal's time
    // after the hand-off; the worker's waits for it after the flush; the frame's end itself; the messages let through
    // during a join's wait for it, and the joins asked for meanwhile (from the window's procedure)
    uint64_t frameEnds = 0, frameEndsJoined = 0, sealCycles = 0, sealWaits = 0, sealWaitCycles = 0,
             frameEndCycles = 0, sentAtOnce = 0, nestedJoins = 0, nestedMadeBeside = 0;
    uint64_t windowJoins = 0, windowMadeBeside = 0;  // joins from inside the game window's procedure meanwhile
    // overlap
    uint64_t beside = 0;                       // the main thread's time from the hand-off to the join
    uint64_t joinWait = 0;                     // its wait there for the flush's end
    uint64_t joinedRunning = 0;                // joins that found the flush still running
    uint64_t joinsNextFlush = 0, joinsReport = 0, joinsQueue = 0, joinsPin = 0;
    // where the two threads ran (group 0's logical processors): the main thread at each hand-off, the worker as it
    // started the flush; the hand-offs where both were on one physical core; another processor than the time before
    uint64_t placed = 0, sameCore = 0, workerMoved = 0, mainMoved = 0;
    uint64_t mainOn[64] = {}, workerOn[64] = {};
};

struct State {
    FMode mode = FMode::Main;
    bool started = false;  // the first outer flush has come
    char* renderer = nullptr;
    DWORD mainThread = 0;
    volatile DWORD flushThread = 0;
#ifdef _WIN64
    HANDLE thread = nullptr, go = nullptr, done = nullptr;
    // the flush handed over, and the main thread's state that goes with it (MXCSR and the seed come back)
    void (*fn)(void*) = nullptr;
    void* ctx = nullptr;
    unsigned mxcsr = 0, seed = 0;
    int phase = 0;
    void* (*getptd)() = nullptr;  // msvcr80's per-thread block
    int seedAt = -1;              // rand's seed in it; -1: not found (the worker keeps its own)
    int priority = THREAD_PRIORITY_NORMAL;
    volatile ULONG_PTR stackHigh = 0;  // the worker's stack base (its stack is read up to it when stuck)
    HANDLE main = nullptr;             // the main thread (suspended for a moment when it seems hung)
    ULONG_PTR mainStackHigh = 0;
    volatile uint64_t lastHandOff = 0;  // GetTickCount64 at the last hand-off
    uint64_t loggedFor = 0;             // the hand-off whose stall was logged
    int mainLogs = 0;
    bool hangExit = false;              // FlushThreadHangExit=on (tests)
    uint64_t tGo = 0, tStart = 0, tEnd = 0;  // this flush: signalled, started, ended
    // overlap: the flush beside the main thread, its after-flush function, and the calls that joined it
    bool pending = false;
    volatile bool ended = false;  // the worker is done with the flush handed off last (a join won't wait)
    volatile bool flushEnded = false;  // the flush itself has ended (the frame's end may still be the worker's)
    // FlushThreadPresent=on: the frame's end, open until the main thread seals it (the worker's to make) or joins first
    bool present = false;
    volatile LONG frameEnd = 0;
    HANDLE sealed = nullptr;     // set at either
    bool inWait = false;         // the main thread takes a message sent to it, in a join's wait
    volatile bool inFrameEnd = false;  // the worker makes the frame's end
    // the game window's procedure, hooked (FlushThread_WatchWindowProc): the main thread's depth inside it
    bool windowWatched = false;
    void* windowProc = nullptr;  // (the original)
    int windowDepth = 0;
    unsigned joinReason = 0;  // the join waiting now (for a stall's log)
    uint64_t joinStarted = 0;  // when the last join began (its time stamp counter)
    // FlushThreadObserve=on: one join in kObserveEvery keeps the gate on for the rest of the frame (to the next outer
    // flush's entry), each call that would have joined counted per thunk, with its first caller
    bool observe = false, observing = false;
    uint64_t observeJoins = 0;
    std::vector<uint64_t> wouldJoin;
    std::vector<void*> wouldCaller;
    void (*after)(void*) = nullptr;
    void* afterCtx = nullptr;
    std::vector<uint32_t> joins;  // per gate slot
    // the physical cores (each one's logical processors, group 0, and efficiency class: higher is faster), each
    // processor's core (-1: not known) and the last-level caches' processors; where the main thread was at the last
    // hand-off and the two threads the time before; FlushThreadPlacement=apart: the mask the process's other threads
    // are kept to
    std::vector<KAFFINITY> coreMasks;
    std::vector<BYTE> coreClass;
    std::vector<KAFFINITY> l3Masks;
    int coreOf[64] = {};
    volatile unsigned mainCpu = ~0u;
    unsigned lastMain = ~0u, lastWorker = ~0u;
    bool apart = false;
    KAFFINITY othersMask = 0;
#endif
    Stats st;
};
State g;

#ifdef _WIN64
// The function a module imports by name from a DLL (the bound address in its import table), or null.
void* Imported(char* base, const char* dll, const char* name) {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!d.VirtualAddress) return nullptr;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + d.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(base + imp->Name, dll) != 0 || !imp->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
            if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) &&
                std::strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name, name) == 0)
                return reinterpret_cast<void*>(slots->u1.Function);
    }
    return nullptr;
}

// rand's seed in the per-thread block of the CRT the renderer calls rand in: the offset from rand's own code, which
// has to call that CRT's exported _getptd
void FindSeed() {
    char* rand = g.renderer ? static_cast<char*>(Imported(g.renderer, "MSVCR80.dll", "rand")) : nullptr;
    HMODULE crt = nullptr;
    if (rand)
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(rand), &crt);
    char* getptd = crt ? static_cast<char*>(FindExport(reinterpret_cast<char*>(crt), "_getptd")) : nullptr;
    if (!getptd || !IsInCode(reinterpret_cast<char*>(crt), rand, sizeof kRandCode / 3) ||
        !MatchesAt(rand, ParsePattern(kRandCode)) || RelTarget(rand + kRandCall) != getptd)
        return;
    g.getptd = reinterpret_cast<void* (*)()>(getptd);
    g.seedAt = static_cast<uint8_t>(rand[kRandSeed]);
}

unsigned& Seed() { return *reinterpret_cast<unsigned*>(static_cast<char*>(g.getptd()) + g.seedAt); }

// the worker's name: for diag's crash lines (the debugger convention, which it catches) and for tools
void Name(HANDLE thread, DWORD id) {
    using SetDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (auto set = reinterpret_cast<SetDescriptionFn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))
        set(thread, L"optiperf flush");
    struct {
        DWORD type;
        LPCSTR name;
        DWORD id;
        DWORD flags;
    } info = {0x1000, "optiperf flush", id, 0};
    __try {
        RaiseException(0x406D1388, 0, DWORD(sizeof info / sizeof(ULONG_PTR)), reinterpret_cast<const ULONG_PTR*>(&info));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// the calling thread's logical processor in group 0 (~0u in another group)
unsigned CurrentProcessor() {
    PROCESSOR_NUMBER p = {};
    GetCurrentProcessorNumberEx(&p);
    return p.Group == 0 && p.Number < 64 ? unsigned(p.Number) : ~0u;
}

// the physical cores' logical processors and efficiency classes, and the last-level caches' processors (group 0), for
// where the two threads run
void ReadTopology() {
    for (int& c : g.coreOf) c = -1;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &bytes);
    std::vector<char> buf(bytes);
    if (!bytes || !GetLogicalProcessorInformationEx(
                      RelationAll, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()), &bytes))
        return;
    for (DWORD off = 0; off < bytes;) {
        const auto* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (!e->Size) break;
        if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount && e->Processor.GroupMask[0].Group == 0) {
            const KAFFINITY m = e->Processor.GroupMask[0].Mask;
            for (int i = 0; i < 64; ++i)
                if (m & (KAFFINITY(1) << i)) g.coreOf[i] = int(g.coreMasks.size());
            g.coreMasks.push_back(m);
            g.coreClass.push_back(e->Processor.EfficiencyClass);
        } else if (e->Relationship == RelationCache && e->Cache.Level == 3 && e->Cache.GroupMask.Group == 0) {
            g.l3Masks.push_back(e->Cache.GroupMask.Mask);
        }
        off += e->Size;
    }
}

// on the worker as it starts a flush: where it is, against where the main thread was at the hand-off
void CountPlacement() {
    const unsigned w = CurrentProcessor(), m = g.mainCpu;
    if (w >= 64 || m >= 64) return;
    Stats& s = g.st;
    ++s.placed;
    ++s.mainOn[m];
    ++s.workerOn[w];
    if (g.coreOf[m] >= 0 && g.coreOf[m] == g.coreOf[w]) ++s.sameCore;
    if (g.lastWorker < 64 && w != g.lastWorker) ++s.workerMoved;
    if (g.lastMain < 64 && m != g.lastMain) ++s.mainMoved;
    g.lastWorker = w;
    g.lastMain = m;
}

// "processor:count" for each processor with a count
void ListProcessors(const uint64_t (&on)[64], char* out, size_t size) {
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; i < 64; ++i) {
        if (!on[i]) continue;
        const int w = snprintf(out + used, size - used, "%s%d:%llu", used ? " " : "", i, on[i]);
        if (w < 0 || size_t(w) >= size - used) break;
        used += size_t(w);
    }
}

// FlushThreadPlacement=apart: the process's other threads kept off the two threads' cores (at the start, then at each
// report for the threads made since); how many were set
int KeepOthersOff() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    int set = 0;
    THREADENTRY32 te = {};
    te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId(), worker = g.flushThread;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == g.mainThread || te.th32ThreadID == worker) continue;
        if (HANDLE t = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID)) {
            if (SetThreadAffinityMask(t, g.othersMask)) ++set;
            CloseHandle(t);
        }
    }
    CloseHandle(snap);
    return set;
}

// FlushThreadPlacement=apart, at the start on the main thread: it and the worker each on a physical core of
// their own, the process's other threads on the rest. The two cores, among those whose processors are all in the
// process's mask (a launcher may have narrowed it): of the most performant efficiency class (a hybrid part's
// performance cores), within the last-level cache that holds most of those (one core complex of a part with several),
// not processor 0's while others are left (it takes most interrupts), the last two. At least two cores must be left
// for the other threads, or nothing is set.
void Apart() {
    DWORD_PTR process = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    const KAFFINITY mask = KAFFINITY(process);
    const auto whole = [&](size_t c) { return (g.coreMasks[c] & ~mask) == 0; };
    BYTE best = 0;
    for (size_t c = 0; c < g.coreMasks.size(); ++c)
        if (whole(c)) best = std::max(best, g.coreClass[c]);
    std::vector<size_t> cand;
    for (size_t c = 0; c < g.coreMasks.size(); ++c)
        if (whole(c) && g.coreClass[c] == best) cand.push_back(c);
    KAFFINITY cache = 0;
    size_t most = 0;
    for (const KAFFINITY l3 : g.l3Masks) {
        const auto in = [&](size_t c) { return (g.coreMasks[c] & ~l3) == 0; };
        const size_t n = size_t(std::count_if(cand.begin(), cand.end(), in));
        if (n > most) {
            most = n;
            cache = l3;
        }
    }
    if (most >= 2)
        cand.erase(std::remove_if(cand.begin(), cand.end(), [&](size_t c) { return (g.coreMasks[c] & ~cache) != 0; }),
                   cand.end());
    if (cand.size() > 2 && g.coreOf[0] >= 0)
        cand.erase(std::remove(cand.begin(), cand.end(), size_t(g.coreOf[0])), cand.end());
    const KAFFINITY mainMask = cand.size() >= 2 ? g.coreMasks[cand[cand.size() - 1]] : 0;
    const KAFFINITY workerMask = cand.size() >= 2 ? g.coreMasks[cand[cand.size() - 2]] : 0;
    g.othersMask = mask & ~(mainMask | workerMask);
    int otherCores = 0;
    for (const KAFFINITY m : g.coreMasks)
        if (m & g.othersMask) ++otherCores;
    if (cand.size() < 2 || otherCores < 2) {
        Log("flush thread: placement apart (FlushThreadPlacement) not applied: the process's mask 0x%llX has %zu whole "
            "cores to choose from (efficiency class %u) and would leave %d for the other threads",
            static_cast<unsigned long long>(process), cand.size(), unsigned(best), otherCores);
        g.apart = false;
        return;
    }
    const size_t mainCore = cand[cand.size() - 1], workerCore = cand[cand.size() - 2];
    const bool mainSet = SetThreadAffinityMask(GetCurrentThread(), mainMask) != 0;
    const bool workerSet = SetThreadAffinityMask(g.thread, workerMask) != 0;
    const int others = KeepOthersOff();
    Log("flush thread: placement apart (FlushThreadPlacement): the main thread on core %zu (processors 0x%llX%s), "
        "the worker on core %zu (0x%llX%s), chosen from %zu cores of efficiency class %u in a last-level cache of "
        "0x%llX; the process's %d other threads on 0x%llX, %d cores (threads made later get the process's mask 0x%llX "
        "until the next report sets theirs)",
        mainCore, static_cast<unsigned long long>(mainMask), mainSet ? "" : ", not set", workerCore,
        static_cast<unsigned long long>(workerMask), workerSet ? "" : ", not set", cand.size(), unsigned(best),
        static_cast<unsigned long long>(cache), others, static_cast<unsigned long long>(g.othersMask), otherCores,
        static_cast<unsigned long long>(process));
}

void WatchMain();

// FlushThreadPresent=on, on the worker right after the flush: the frame's end the main thread seals at its EndFrame,
// made here (or none, if the main thread joins first; meanwhile the worker waits, watching the main thread as when idle)
void FrameEnd() {
    RenderQueue_AfterFlush();  // (what the join compares the flush's end with, before the frame's end changes it)
    if (g.frameEnd == kFrameEndOpen) {
        const uint64_t w0 = __rdtsc();
        ++g.st.sealWaits;
        while (g.frameEnd == kFrameEndOpen)
            if (WaitForSingleObject(g.sealed, kWatchMs) == WAIT_TIMEOUT) WatchMain();
        g.st.sealWaitCycles += __rdtsc() - w0;
    }
    if (g.frameEnd != kFrameEndSealed) return;
    const uint64_t t0 = __rdtsc();
    g.inFrameEnd = true;
    RenderQueue_MakeSealed();
    g.inFrameEnd = false;
    g.st.frameEndCycles += __rdtsc() - t0;
    ++g.st.frameEnds;
}

DWORD WINAPI Worker(void*) {
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    g.stackHigh = high;
    for (;;) {
        const DWORD w = WaitForSingleObject(g.go, kWatchMs);
        if (w == WAIT_TIMEOUT) {  // (no flush handed off for a while: a hang of the main thread's is logged)
            WatchMain();
            continue;
        }
        if (w != WAIT_OBJECT_0) return 1;
        g.tStart = __rdtsc();
        CountPlacement();
        _mm_setcsr(g.mxcsr);
        if (g.seedAt >= 0) Seed() = g.seed;
        CallTrace_SetPhase(g.phase);
        CallTrace_SetFlush(true);
        g.fn(g.ctx);
        CallTrace_SetFlush(false);
        if (g.seedAt >= 0) g.seed = Seed();
        g.mxcsr = _mm_getcsr();
        g.tEnd = __rdtsc();
        g.flushEnded = true;
        RenderQueue_OnFlushEnd();  // (its waits beside the flush end too)
        if (g.present) FrameEnd();
        g.ended = true;
        SetEvent(g.done);
    }
}

// where the worker is: its instruction, and the game call chain in a copy of its stack taken while it is suspended
void LogStuck(int seconds, bool sent) {
    static uintptr_t stack[4096];  // (32 KB: not on the main thread's stack)
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL;
    SIZE_T got = 0;
    bool ok = false;
    if (SuspendThread(g.thread) != DWORD(-1)) {
        if (GetThreadContext(g.thread, &c)) {
            ok = true;
            const ULONG_PTR high = g.stackHigh;
            const SIZE_T n = high > c.Rsp ? (std::min)(SIZE_T(high - c.Rsp), sizeof stack) : 0;
            if (n && !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.Rsp), stack, n, &got)) got = 0;
        }
        ResumeThread(g.thread);
    }
    char where[160] = "?", chain[1024] = "(not read)";
    if (ok) DescribeAddress(reinterpret_cast<void*>(c.Rip), where, sizeof where);
    if (got >= sizeof(uintptr_t)) Diag_CallChain(stack, unsigned(got / sizeof(uintptr_t)), chain, sizeof chain);
    Log("flush thread: the flush has run for %d s on the worker, now at %s | game call chain: %s | a message sent to "
        "the main thread: %s",
        seconds, where, chain, sent ? "waiting (let through)" : "none");
    if (!g.pending) return;
    // overlap: where the main thread joined it (what it holds there, the flush may be waiting for)
    char what[128] = "the next flush";
    if (g.joinReason == kJoinReport)
        snprintf(what, sizeof what, "the report");
    else if (g.joinReason == kJoinQueue)
        snprintf(what, sizeof what, "the render queue's request");
    else if (g.joinReason == kJoinPin)
        snprintf(what, sizeof what, "a pinned block's realloc");
    else if (g.joinReason != kJoinNextFlush)
        RenderGate_Describe(g.joinReason, what, sizeof what);
    char mine[1024];
    Diag_CallChain(_AddressOfReturnAddress(), 4096, mine, sizeof mine);
    Log("flush thread: the main thread waits for it at %s | its game call chain: %s", what, mine);
}

// On the worker, with no flush handed off for kWatchMs since the last (the main thread hung outside the flush's waits:
// a join's replay, the game's own code): where the main thread is, from a copy of its stack taken while it is
// suspended, and what the render queue is doing. Once per stall, a few per run; a long load may log one too.
// (For tests, [ShadowPrepass] FlushThreadHangExit=on ends the process when the stall lasts kWatchMs more, so that a
// hung run frees the job queue at once; a level load as long would end it too.)
void WatchMain() {
    const uint64_t last = g.lastHandOff, now = GetTickCount64();
    if (!last || !g.main || now - last < kWatchMs) return;
    if (g.loggedFor == last) {
        if (g.hangExit && now - last >= 2 * kWatchMs) {
            Log("flush thread: still no flush handed off after %llu s: the process is ended (FlushThreadHangExit=on)",
                (now - last) / 1000);
            TerminateProcess(GetCurrentProcess(), kHangExitCode);
        }
        return;
    }
    if (g.mainLogs >= kMaxMainLogs) return;
    g.loggedFor = last;
    ++g.mainLogs;
    static uintptr_t stack[8192];  // (64 KB, the worker's: the main thread's stack copied here)
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL;
    SIZE_T got = 0;
    bool ok = false;
    if (SuspendThread(g.main) != DWORD(-1)) {
        if (GetThreadContext(g.main, &c)) {
            ok = true;
            const ULONG_PTR high = g.mainStackHigh;
            const SIZE_T n = high > c.Rsp ? (std::min)(SIZE_T(high - c.Rsp), sizeof stack) : 0;
            if (n && !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.Rsp), stack, n, &got)) got = 0;
        }
        ResumeThread(g.main);
    }
    char where[160] = "?", chain[1400] = "(not read)", full[2400] = "(not read)", queue[200] = "";
    if (ok) DescribeAddress(reinterpret_cast<void*>(c.Rip), where, sizeof where);
    if (got >= sizeof(uintptr_t)) {
        Diag_CallChain(stack, unsigned(got / sizeof(uintptr_t)), chain, sizeof chain);
        Diag_FullCallChain(stack, unsigned(got / sizeof(uintptr_t)), 16, full, sizeof full);
    }
    RenderQueue_Where(queue, sizeof queue);
    const char* const step = ShadowPrepass_AfterStep();
    Log("flush thread: no flush handed off for %llu s (the last one %s); the main thread is at %s | its game call "
        "chain: %s | the render queue: %s | the parts' step after the flush: %s | every module's return addresses: %s",
        (now - last) / 1000, g.pending ? "still beside it" : "joined", where, chain, queue, step ? step : "none",
        full);
}

// the main thread waits for the flush (after 5 s: logged, and messages sent to it let through), and for the frame's
// end after it if the worker makes it: then messages sent to the main thread are let through at once (the present may
// wait for one), taken with inWait set (a join asked for meanwhile: BoundedJoin)
// (spun first, up to kJoinSpinCycles: a flush that ends soon is joined without the main thread sleeping, which can
// wake it on another core with its caches cold; the worker sets ended before the event, which stays set until the
// next hand-off)
constexpr uint64_t kJoinSpinCycles = 600000;  // about 200 us (the reports' 3 GHz)
bool TakeSentMessages() {
    MSG m;
    const bool inWait = g.inWait;
    g.inWait = true;
    const BOOL any = PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    g.inWait = inWait;
    return any != FALSE;
}
void Wait() {
    const uint64_t c0 = __rdtsc();
    while (!g.ended && __rdtsc() - c0 < kJoinSpinCycles) YieldProcessor();
    ++(g.ended ? g.st.joinsSpun : g.st.joinsSlept);
    for (int seconds = 0;;) {
        if (g.frameEnd == kFrameEndSealed) {  // (the worker's frame's end: its present, away from the window's thread)
            const DWORD w = MsgWaitForMultipleObjectsEx(1, &g.done, kStuckMs, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
            if (w == WAIT_OBJECT_0) return;
            if (w == WAIT_OBJECT_0 + 1) {
                TakeSentMessages();
                ++g.st.sentAtOnce;
                continue;
            }
        } else if (WaitForSingleObject(g.done, kStuckMs) == WAIT_OBJECT_0) {
            return;
        }
        seconds += int(kStuckMs / 1000);
        const bool sent = (HIWORD(GetQueueStatus(QS_SENDMESSAGE)) & QS_SENDMESSAGE) != 0;
        if (++g.st.stuck <= kMaxStuckLogs) LogStuck(seconds, sent);
        if (sent) {
            MSG m;
            PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            ++g.st.sent;
        }
    }
}

// the flush's times, once it has ended
void CountFlush(uint64_t back) {
    Stats& s = g.st;
    const uint64_t start = g.tStart > g.tGo ? g.tStart - g.tGo : 0, ret = back > g.tEnd ? back - g.tEnd : 0;
    ++s.flushes;
    s.flushCycles += g.tEnd > g.tStart ? g.tEnd - g.tStart : 0;
    s.startCycles += start;
    s.maxStart = (std::max)(s.maxStart, start);
    if (g.mode == FMode::Worker) {
        s.backCycles += ret;
        s.maxBack = (std::max)(s.maxBack, ret);
    }
}

// overlap, on the main thread: the flush beside it ends here (waited for, then the after-flush function); reason is
// the gate's slot that joined, or kJoinNextFlush / kJoinReport
void Finish(unsigned reason, uint64_t t0);
void BoundedJoin(unsigned reason, uint64_t t0, bool fromWindow);
void Join(unsigned reason) {
    if (!g.pending) return;
    if (g.inWait) return BoundedJoin(reason, __rdtsc(), false);
    g.joinReason = reason;
    const uint64_t t0 = __rdtsc();
    g.joinStarted = t0;
    g.st.beside += t0 - g.tGo;
    // (FlushThreadPresent: a frame's end not sealed by now is the join's to make; the worker stops waiting for it)
    if (g.present && InterlockedCompareExchange(&g.frameEnd, kFrameEndJoined, kFrameEndOpen) == kFrameEndOpen) {
        ++g.st.frameEndsJoined;
        SetEvent(g.sealed);
    }
    if (!g.ended) {  // (the gate stays on: a call from the window's procedure, in a message let through, joins nested)
        ++g.st.joinedRunning;
        // (from inside the game window's procedure while the worker makes the frame's end: a bounded wait)
        if (g.frameEnd == kFrameEndSealed && g.windowDepth > 0) return BoundedJoin(reason, t0, true);
        Wait();
        if (!g.pending) return;  // (a nested join has done the rest)
    }
    Finish(reason, t0);
}
// A join asked for from the game window's procedure while the worker makes the frame's end: in a message a join's wait
// let through (nested), or one the main thread takes elsewhere (fromWindow), which may come from inside DXGI's own
// handling of the window (a full-screen switch, holding DXGI's lock, which the worker's present waits for). It waits
// for the worker as a join does, taking further messages sent to the main thread, and then does the join's work (a
// join it came from finds it done). If the worker isn't done within kNestedWaitMs (its present may be waiting for this
// very message, or for DXGI's lock), the call goes on beside the frame's end, logged.
void BoundedJoin(unsigned reason, uint64_t t0, bool fromWindow) {
    ++(fromWindow ? g.st.windowJoins : g.st.nestedJoins);
    const uint64_t ms0 = GetTickCount64();
    while (!g.ended && GetTickCount64() - ms0 < kNestedWaitMs)
        if (MsgWaitForMultipleObjectsEx(1, &g.done, 50, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0 + 1)
            TakeSentMessages();
    if (g.ended) {
        if (g.pending) Finish(reason, t0);
        return;
    }
    const uint64_t beside = ++(fromWindow ? g.st.windowMadeBeside : g.st.nestedMadeBeside);
    if (beside > kMaxStuckLogs) return;
    char what[128] = "the render queue's request";
    if (reason < g.joins.size()) RenderGate_Describe(reason, what, sizeof what);
    char mine[1024];
    Diag_CallChain(_AddressOfReturnAddress(), 4096, mine, sizeof mine);
    Log("flush thread: %s, from %s while the worker makes the frame's end, waited %lu ms for it: made beside it | the "
        "main thread's game call chain: %s",
        what, fromWindow ? "the game window's procedure" : "a message a join's wait let through", kNestedWaitMs, mine);
}

// FlushThreadPresent: the game window's procedure, hooked (FlushThread_WatchWindowProc): the main thread's depth in it
using WindowProcFn = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK WindowProcHook(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    const bool main = GetCurrentThreadId() == g.mainThread;
    if (main) ++g.windowDepth;
    const LRESULT r = reinterpret_cast<WindowProcFn>(g.windowProc)(window, message, wparam, lparam);
    if (main) --g.windowDepth;
    return r;
}
void Finish(unsigned reason, uint64_t t0) {
    RenderGate_Set(0);  // (the after-flush function's own renderer calls go straight on)
    const uint64_t t1 = __rdtsc();
    g.st.joinWait += t1 - t0;
    g.pending = false;
    CountFlush(t1);
    if (reason == kJoinNextFlush)
        ++g.st.joinsNextFlush;
    else if (reason == kJoinReport)
        ++g.st.joinsReport;
    else if (reason == kJoinQueue)
        ++g.st.joinsQueue;
    else if (reason == kJoinPin)
        ++g.st.joinsPin;
    else if (reason < g.joins.size())
        ++g.joins[reason];
    if (g.after) g.after(g.afterCtx);
    const uint64_t t2 = __rdtsc();
    Allocator_EndPins();   // (the parts' steps have written: their blocks unpinned, the frees kept meanwhile made)
    const uint64_t t3 = __rdtsc();
    RenderQueue_Replay();  // (the calls taken beside the flush, after the work vanilla's order puts first)
    const uint64_t t4 = __rdtsc();
    // (a traversal the recorder was answering beside the flush: its record made now, its calls made from here on)
    TraversalRecord_OnJoined(reason < g.joins.size() ? reason : ~0u, reason == kJoinNextFlush);
    g.st.joinAfter += t2 - t1;
    g.st.joinPins += t3 - t2;
    g.st.joinQueue += t4 - t3;
    g.st.joinRecord += __rdtsc() - t4;
    // (FlushThreadObserve: one join in kObserveEvery, but the next flush's, keeps the gate on to the frame's end)
    if (g.observe && reason != kJoinNextFlush && ++g.observeJoins % kObserveEvery == 0) {
        g.observing = true;
        ++g.st.observedFrames;
        RenderGate_Set(g.mainThread);
    }
}

// the allocator's callback at a main-thread realloc of a pinned block: the flush beside it joined first
void PinJoin() {
    if (g.pending && GetCurrentThreadId() == g.mainThread) Join(kJoinPin);
}

// the gate's callback: the main thread's call into the renderer while the flush runs beside it (the render queue's
// calls go on without joining: the queue takes them)
// (the gate's slow path calls it with its frame: 5 registers pushed and 0x60 bytes below them, so the gated call's
// return address, into its caller, lies 144 bytes past this function's own)
constexpr size_t kGatedCallerAt = 8 + 0x60 + 5 * 8;
void OnGated(unsigned index) {
    if (RenderQueue_Passes(index)) return;
    if (g.pending) {
        Join(index);
    } else {  // (the gate kept on after a join: this observation, or the recorder's)
        void* const caller = *reinterpret_cast<void* const*>(static_cast<char*>(_AddressOfReturnAddress()) +
                                                              kGatedCallerAt);
        if (!g.observing) return TraversalRecord_OnWouldJoin(index, caller);
        if (GetCurrentThreadId() != g.mainThread) return;
        if (g.wouldJoin.size() <= index) {
            g.wouldJoin.resize(size_t(index) + 1, 0);
            g.wouldCaller.resize(size_t(index) + 1, nullptr);
        }
        if (!g.wouldJoin[index]++) g.wouldCaller[index] = caller;
    }
}
#endif

// At the first outer flush, on the main thread: the threads, and the worker if it is on.
void Start() {
    g.started = true;
    g.mainThread = GetCurrentThreadId();
    g.flushThread = g.mainThread;
#ifdef _WIN64
    if (g.mode == FMode::Main) return;
    FindSeed();
    ULONG_PTR stackLow = 0, stackHigh = 0;  // (the main thread's, for the worker's watch: WatchMain)
    GetCurrentThreadStackLimits(&stackLow, &stackHigh);
    g.mainStackHigh = stackHigh;
    g.main = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g.mainThread);
    g.go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.sealed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.sealed) g.present = false;
    DWORD id = 0;
    if (g.go && g.done)
        g.thread = CreateThread(nullptr, kStackBytes, &Worker, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &id);
    if (!g.thread) {
        Log("flush thread: the worker could not be started (error %lu): the flush stays on the main thread",
            GetLastError());
        return;
    }
    g.priority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(g.thread, g.priority);
    Name(g.thread, id);
    g.flushThread = id;
    Log("flush thread: the outer flush runs on a worker (thread %lu, %u MB of stack) while the main thread (%lu, "
        "priority %d, MXCSR 0x%04X) %s; the C runtime's random seed %s",
        id, unsigned(kStackBytes >> 20), g.mainThread, g.priority, _mm_getcsr(),
        g.mode == FMode::Overlap ? "goes on until its first renderer call" : "waits",
        g.seedAt >= 0 ? "goes with the flush (msvcr80's per-thread block)"
                      : "stays the worker's own (the renderer's rand is not the expected code)");
    ReadTopology();
    if (g.apart) Apart();
#endif
}

}  // namespace

void FlushThread_Install(const wchar_t* moduleName, char* rendererBase, char* endEf3D) {
    const std::wstring v =
        ConfigString(L"ShadowPrepass", L"FlushThread", L"OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD", L"main");
    g.mode = _wcsicmp(v.c_str(), L"worker") == 0    ? FMode::Worker
             : _wcsicmp(v.c_str(), L"overlap") == 0 ? FMode::Overlap
                                                    : FMode::Main;
    g.renderer = rendererBase;
#ifdef _WIN64
    g.hangExit = _wcsicmp(ConfigString(L"ShadowPrepass", L"FlushThreadHangExit",
                                       L"OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD_HANG_EXIT", L"off")
                              .c_str(),
                          L"on") == 0;
    g.observe = g.mode == FMode::Overlap &&
                _wcsicmp(ConfigString(L"ShadowPrepass", L"FlushThreadObserve",
                                      L"OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD_OBSERVE", L"off")
                             .c_str(),
                         L"on") == 0;
    g.present = g.mode == FMode::Overlap &&
                _wcsicmp(ConfigString(L"ShadowPrepass", L"FlushThreadPresent",
                                      L"OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD_PRESENT", L"off")
                             .c_str(),
                         L"on") == 0;
    g.apart = g.mode != FMode::Main &&
              _wcsicmp(ConfigString(L"ShadowPrepass", L"FlushThreadPlacement",
                                    L"OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD_PLACEMENT", L"default")
                           .c_str(),
                       L"apart") == 0;
#endif
#ifdef _WIN64
    if (g.mode == FMode::Overlap) {
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(rendererBase +
                                                       reinterpret_cast<IMAGE_DOS_HEADER*>(rendererBase)->e_lfanew);
        const std::vector<RttiVtable> vts =
            FindRttiVtables(rendererBase, nt->OptionalHeader.SizeOfImage, &CallTrace_Original);
        if (!RenderGate_Install(vts, &OnGated)) {
            Log("%ls: flush thread: overlap needs the renderer's gate, which could not be installed (%zu vtables): the "
                "main thread waits for the flush (worker)",
                moduleName, vts.size());
            g.mode = FMode::Worker;
            g.present = false;
        } else {
            g.joins.assign(RenderGate_Slots(), 0);
            Log("%ls: flush thread: overlap (it starts at the first outer flush); the renderer's gate on %u vtables, "
                "%u slots",
                moduleName, RenderGate_Vtables(), RenderGate_Slots());
            RenderQueue_Install(moduleName, rendererBase, endEf3D);
            Allocator_EnablePins(&PinJoin);  // (the parts' steps after the flush run at the join: what they write, kept)
            if (g.present)
                Log("%ls: flush thread: the frame's end on the worker (FlushThreadPresent=on): the calls taken up to the "
                    "present are made after the flush there",
                    moduleName);
        }
    }
    if (g.mode == FMode::Worker) Log("%ls: flush thread: worker (it starts at the first outer flush)", moduleName);
#else
    (void)endEf3D;
    if (g.mode != FMode::Main)
        Log("%ls: flush thread: the worker is x64 only: the flush stays on the main thread", moduleName);
    g.mode = FMode::Main;
#endif
}

void FlushThread_OnEntry() {
#ifdef _WIN64
    if (g.pending) Join(kJoinNextFlush);
    if (g.observing) {  // (the observed frame's end: the gate off until the hand-off, the parts' entry work not counted)
        g.observing = false;
        RenderGate_Set(0);
    }
#endif
    if (!g.started) Start();
}

unsigned long FlushThread() { return g.flushThread; }

bool OnFlushThread() { return GetCurrentThreadId() == g.flushThread; }

void FlushThread_JoinNow() {
#ifdef _WIN64
    if (g.pending && GetCurrentThreadId() == g.mainThread) Join(kJoinQueue);
#endif
}

bool FlushThread_Beside() {
#ifdef _WIN64
    return g.pending && GetCurrentThreadId() == g.mainThread;
#else
    return false;
#endif
}

bool FlushThread_Ended() {
#ifdef _WIN64
    // (or the flush has ended and the worker only waits for a frame's end not sealed: a join takes that over at once)
    return g.ended || (g.flushEnded && g.frameEnd == kFrameEndOpen);
#else
    return true;
#endif
}

bool FlushThread_FlushEnded() {
#ifdef _WIN64
    return g.flushEnded;
#else
    return true;
#endif
}

bool FlushThread_SealOpen() {
#ifdef _WIN64
    return g.present && g.pending && g.frameEnd == kFrameEndOpen && GetCurrentThreadId() == g.mainThread;
#else
    return false;
#endif
}

void FlushThread_WatchWindowProc(void* proc) {
#ifdef _WIN64
    if (!g.present || g.windowWatched || !proc) return;
    g.windowWatched = true;
    // (one of the game's modules: not a subclass of the system's, DXGI's say)
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = L"", windows[MAX_PATH] = L"";
    const bool known = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                          static_cast<LPCWSTR>(proc), &module) &&
                       GetModuleFileNameW(module, path, MAX_PATH) && GetSystemWindowsDirectoryW(windows, MAX_PATH);
    if (!known || _wcsnicmp(path, windows, wcslen(windows)) == 0 ||
        !IsInCode(reinterpret_cast<char*>(module), static_cast<const char*>(proc), 16)) {
        Log("flush thread: the game window's procedure isn't watched (%ls): a join from inside it waits for the frame's "
            "end as any",
            known ? path : L"not in a module");
        return;
    }
    if (!InstallHook(proc, reinterpret_cast<void*>(&WindowProcHook), &g.windowProc, "the game window's procedure")) {
        g.windowProc = nullptr;
        return;
    }
    char where[160];
    Log("flush thread: the game window's procedure (%s) watched: a join from inside it while the worker makes the "
        "frame's end waits for it at most %lu ms",
        DescribeAddress(proc, where, sizeof where), kNestedWaitMs);
#else
    (void)proc;
#endif
}

bool FlushThread_Seal() {
#ifdef _WIN64
    if (!g.present || !g.pending || GetCurrentThreadId() != g.mainThread ||
        InterlockedCompareExchange(&g.frameEnd, kFrameEndSealed, kFrameEndOpen) != kFrameEndOpen)
        return false;
    g.st.sealCycles += __rdtsc() - g.tGo;
    SetEvent(g.sealed);
    return true;
#else
    return false;
#endif
}

unsigned long long FlushThread_JoinStartedAt() {
#ifdef _WIN64
    return g.joinStarted;
#else
    return 0;
#endif
}

void FlushThread_Run(void (*fn)(void*), void* ctx, void (*after)(void*), void* afterCtx) {
    if (!g.started) Start();
#ifdef _WIN64
    if (g.thread) {
        if (g.pending) Join(kJoinNextFlush);  // (OnEntry has joined it)
        const int priority = GetThreadPriority(GetCurrentThread());
        if (priority != g.priority && priority != THREAD_PRIORITY_ERROR_RETURN && SetThreadPriority(g.thread, priority)) {
            g.priority = priority;
            ++g.st.priority;
        }
        g.fn = fn;
        g.ctx = ctx;
        const unsigned mxcsr = _mm_getcsr();
        g.mxcsr = mxcsr;
        g.phase = CallTrace_Phase();
        if (g.seedAt >= 0) g.seed = Seed();
        g.tGo = __rdtsc();
        g.mainCpu = CurrentProcessor();
        g.lastHandOff = GetTickCount64();
        g.ended = false;
        g.flushEnded = false;
        g.frameEnd = kFrameEndOpen;
        ResetEvent(g.done);  // (set when the worker is done, until the next hand-off: a nested join's wait sees it too)
        if (g.mode == FMode::Overlap) {
            g.after = after;
            g.afterCtx = afterCtx;
            RenderQueue_OnHandOff();  // (the last window's calls are all made by now)
            g.pending = true;
            RenderGate_Set(g.mainThread);  // (before the worker can end it: a join waits on the event)
            SetEvent(g.go);
            return;
        }
        SetEvent(g.go);
        Wait();
        const uint64_t back = __rdtsc();
        _mm_setcsr(g.mxcsr);
        if (g.seedAt >= 0) Seed() = g.seed;
        if ((g.mxcsr ^ mxcsr) & ~kMxcsrFlags) ++g.st.mxcsrChanged;
        CountFlush(back);
        if (after) after(afterCtx);
        return;
    }
#endif
    CallTrace_SetFlush(true);
    fn(ctx);
    CallTrace_SetFlush(false);
    if (after) after(afterCtx);
}

void FlushThread_Report(const char* when) {
    if (g.mode == FMode::Main) return;
#ifdef _WIN64
    if (g.pending && GetCurrentThreadId() == g.mainThread) Join(kJoinReport);
#endif
    const Stats& s = g.st;
    const double n = double(s.flushes ? s.flushes : 1);
    Log("flush thread [%s]: %s | flushes on the worker %llu: the flush %.3f ms, its start %.1f us after the signal (most "
        "%.1f), the main thread back %.1f us after its end (most %.1f) | the main thread's priority changed %llu times, "
        "MXCSR control changed in the flush %llu times | waits past 5 s: %llu (sent messages let through: %llu)",
        when,
#ifdef _WIN64
        g.thread    ? (g.mode == FMode::Overlap ? "the worker, beside the main thread" : "the worker")
        : g.started ? "the main thread (the worker did not start)"
                    : "not started yet (no outer flush so far)",
#else
        "the main thread",
#endif
        s.flushes, double(s.flushCycles) / 3.0e6 / n, double(s.startCycles) / 3.0e3 / n, double(s.maxStart) / 3.0e3,
        double(s.backCycles) / 3.0e3 / n, double(s.maxBack) / 3.0e3, s.priority, s.mxcsrChanged, s.stuck, s.sent);
#ifdef _WIN64
    if (g.thread && s.placed) {  // (where the two threads ran at the hand-offs)
        char mains[600] = "", workers[600] = "";
        ListProcessors(s.mainOn, mains, sizeof mains);
        ListProcessors(s.workerOn, workers, sizeof workers);
        DWORD_PTR process = 0, system = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
        Log("flush thread [%s]: placement %s | %zu physical cores, the process's mask 0x%llX | the worker started on the "
            "main thread's core at %llu of %llu hand-offs (%.1f %%), on another processor than the time before %llu "
            "times (the main thread %llu) | the main thread's processors at the hand-offs (processor:count) %s; the "
            "worker's %s",
            when, g.apart ? "apart" : "default", g.coreMasks.size(), static_cast<unsigned long long>(process),
            s.sameCore, s.placed, 100.0 * double(s.sameCore) / double(s.placed), s.workerMoved, s.mainMoved, mains,
            workers);
        if (g.apart) KeepOthersOff();  // (threads made since the start, or the last report)
    }
    if (g.mode != FMode::Overlap) return;
    // the calls that joined, most first
    std::vector<std::pair<uint32_t, unsigned>> top;
    for (unsigned i = 0; i < g.joins.size(); ++i)
        if (g.joins[i]) top.push_back({g.joins[i], i});
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    char reasons[1200] = "";
    size_t used = 0;
    for (int k = 0; k < kTopReasons && k < int(top.size()); ++k) {
        char what[128];
        RenderGate_Describe(top[size_t(k)].second, what, sizeof what);
        const int w = snprintf(reasons + used, sizeof reasons - used, "%s%s: %u", k ? ", " : "", what,
                               top[size_t(k)].first);
        if (w < 0 || size_t(w) >= sizeof reasons - used) break;
        used += size_t(w);
    }
    Log("flush thread [%s]: overlap | the main thread beside the flush %.3f ms per flush, then waiting for it %.3f ms "
        "(it was still running at %llu of the %llu joins: it ended in the spin at %llu, not at %llu) | joined by: %s%s; "
        "the next flush %llu, the report %llu, the render queue %llu, a pinned block's realloc %llu",
        when, double(s.beside) / 3.0e6 / n, double(s.joinWait) / 3.0e6 / n, s.joinedRunning, s.flushes, s.joinsSpun,
        s.joinsSlept, used ? reasons : "(no renderer call)", top.size() > size_t(kTopReasons) ? ", ..." : "",
        s.joinsNextFlush, s.joinsReport, s.joinsQueue, s.joinsPin);
    Log("flush thread [%s]: overlap | the join's work on the main thread, ms per flush: the parts' steps after the "
        "flush %.3f, the pinned blocks' frees %.3f, the render queue's calls %.3f, the traversal's record %.3f",
        when, double(s.joinAfter) / 3.0e6 / n, double(s.joinPins) / 3.0e6 / n, double(s.joinQueue) / 3.0e6 / n,
        double(s.joinRecord) / 3.0e6 / n);
    if (g.present) {
        const double f = double(s.frameEnds ? s.frameEnds : 1);  // (each seal is a frame's end on the worker)
        Log("flush thread [%s]: overlap | the frame's end on the worker (FlushThreadPresent): %llu of the %llu flushes, "
            "made in %.3f ms each (made by the join, not sealed in time: %llu), sealed %.3f ms after the hand-off on "
            "average; the worker waited for the seal after %llu flushes, %.3f ms each | messages sent to the main thread "
            "let through in a join's wait for it: %llu; joins asked for from them %llu, made beside it %llu; joins "
            "asked for from inside the game window's procedure%s: %llu, made beside it %llu",
            when, s.frameEnds, s.flushes, double(s.frameEndCycles) / 3.0e6 / f, s.frameEndsJoined,
            double(s.sealCycles) / 3.0e6 / f, s.sealWaits,
            double(s.sealWaitCycles) / 3.0e6 / double(s.sealWaits ? s.sealWaits : 1), s.sentAtOnce, s.nestedJoins,
            s.nestedMadeBeside, g.windowWatched && g.windowProc ? "" : " (not watched)", s.windowJoins,
            s.windowMadeBeside);
    }
    if (s.observedFrames) {  // (FlushThreadObserve: what else the observed frames would have joined at, most first)
        std::vector<std::pair<uint64_t, unsigned>> seen;
        for (unsigned i = 0; i < g.wouldJoin.size(); ++i)
            if (g.wouldJoin[i]) seen.push_back({g.wouldJoin[i], i});
        std::sort(seen.begin(), seen.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        Log("flush thread [%s]: overlap | the rest of %llu frames after a join (one join in %llu), the calls that "
            "would have joined too, per observed frame, most first:",
            when, s.observedFrames, kObserveEvery);
        char line[1600] = "";  // (a log line holds 2 KB: a few lines of them)
        size_t lu = 0;
        for (size_t k = 0; k < seen.size() && k < 96; ++k) {
            char what[128], from[128], one[320];
            RenderGate_Describe(seen[k].second, what, sizeof what);
            DescribeAddress(g.wouldCaller[seen[k].second], from, sizeof from);
            const int w = snprintf(one, sizeof one, "%s %.1f (first from %s)", what,
                                   double(seen[k].first) / double(s.observedFrames), from);
            if (w < 0) continue;
            if (lu + size_t(w) + 3 > sizeof line) {
                Log("flush thread [%s]:   %s", when, line);
                lu = 0;
                line[0] = 0;
            }
            lu += size_t(snprintf(line + lu, sizeof line - lu, "%s%s", lu ? ", " : "", one));
        }
        if (lu) Log("flush thread [%s]:   %s", when, line);
    }
    RenderQueue_Report(when);
#endif
}

}  // namespace optiperf
