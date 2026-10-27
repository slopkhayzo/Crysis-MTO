// Call tracer (developer tool, off by default): counts calls through the vtables of engine classes per method, caller
// and thread, to map the interfaces between modules. Phase 2 of research/docs/cpu-multithreading-plan.md needs to know
// which renderer methods the rest of the engine calls each frame, from which threads, and which callbacks the renderer
// makes into other modules.
//
// Every vtable described by a traced module's MSVC RTTI gets each slot redirected to a small thunk:
//   - calls from the module itself (return address inside it) only bump a per-slot counter and jump on;
//   - calls from anywhere else are counted per thread and per call site, and on x86 also timed: the return address
//     is swapped for a stub (TracePost) that takes the time and returns to the caller. x86 exception dispatch does not
//     walk return addresses, so this is safe there; on x64 it would break unwinding, so x64 only counts.
// Once a second the changes are appended to optiperf_trace_series.tsv next to the log; research/profiling/
// trace_report.py names the slots (RTTI class + Mod SDK interface layouts) and builds the per-frame tables.
//
// [Trace] Modules  = (empty: off) | renderer | all | comma list of DLL names          (env OPTIPERF_TRACE)
//         Timing   = 1 (default on x86; x64 counts only)                              (env OPTIPERF_TRACE_TIMING)
//         MarkFile = file whose size is logged with every snapshot; run_bench.ps1 passes the timedemo log, so the
//                    report can tell the timedemo runs apart                          (env OPTIPERF_TRACE_MARK)
//
// Output files (TSV, next to optiperf.log):
//   optiperf_trace_slots.tsv    one row per hooked slot: id, module, base, class (mangled), subobject offset,
//                               vtable RVA, slot index, original target RVA
//   optiperf_trace_series.tsv   per snapshot: rows (t, mark, thread, slot, calls, incl, excl) with the counts and
//                               cycles added since the previous snapshot. thread = index into the threads file;
//                               -1 = calls from inside the module (all threads); -2 = clock row (calls = TSC ticks,
//                               incl = microseconds since the previous snapshot)
//   optiperf_trace_sites.tsv    cumulative calls per (thread, slot, return address); on x64 a slot with bit 31 set
//                               counts the calls made inside the renderer's outer flush (CallTrace_SetFlush, set by
//                               the shadow pre-pass's EF_EndEf3D hook: needs [ShadowPrepass] Mode). x86 also lists calls made while
//                               another traced call was running, with ret = that outermost call's slot + 1 (< 0x10000)
//   optiperf_trace_threads.tsv  thread index, id, start address, name
//   optiperf_trace_modules.tsv  loaded modules (base, size, name), to resolve return addresses

#include "calltrace.h"

#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "diag.h"
#include "log.h"
#include "render_gate.h"
#include "scan.h"

extern "C" {
#ifdef _WIN64
void TraceEnter64();  // calltrace_x64.asm
void* TraceOnEnter64(uint32_t slot, uintptr_t ret);
#else
void TraceEnter();
void TracePost();
void* __stdcall TraceOnEnter(uint32_t slot, void** retSlot);
void* __stdcall TraceOnLeave(char* espAtLeave);
#endif
}

namespace optiperf {
namespace {

#ifdef _WIN64
constexpr uint32_t kMaxSlots = 1u << 18;
constexpr size_t kThunkSize = 80, kOrigAt = 56, kCommonAt = 64;  // code: 49 bytes
constexpr uint32_t kSiteBits = 16;
#else
constexpr uint32_t kMaxSlots = 1u << 15;
constexpr size_t kThunkSize = 64, kOrigAt = 48;  // code: 45 bytes
constexpr uint32_t kSiteBits = 14;
#endif
constexpr uint32_t kChunk = 1024, kChunks = kMaxSlots / kChunk, kSiteMask = (1u << kSiteBits) - 1;
// One reservation: thunk code first, then the per-slot counters of calls from inside the module on their own pages.
// The counters must not share cache lines with code: every store there would flush the pipeline (self-modifying code).
// x64 thunks reach them RIP-relative, so they stay within the same reservation.
constexpr size_t kCodeBytes = (size_t(kMaxSlots) * kThunkSize + 0xFFFF) & ~size_t(0xFFFF);
constexpr size_t kCounterBytes = size_t(kMaxSlots) * sizeof(uint64_t);
constexpr int kMaxThreads = 256, kMaxDepth = 128, kSiteProbes = 32;

struct Counter {
    uint64_t calls, incl, excl;
};
struct Site {
    uintptr_t ret;
    uint32_t slot;
    uint64_t count;
};
struct Frame {
    void* ret;
    char* retSlot;
    uint64_t t0, child;
    uint32_t slot;
};

struct ThreadState {
    int index;
    DWORD tid;
    uintptr_t start;
    uint64_t siteOverflow, untimed, stale;
    int depth;
    Site* sites;
    Counter* chunks[kChunks];
    Counter* seen[kChunks];  // snapshot thread only: values already written
    Frame frames[kMaxDepth];
};

using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

struct State {
    bool enabled = false, all = false, renderer = false, timing = false;
    NtQueryInformationThreadFn queryThread = nullptr;
    std::vector<std::wstring> modules;
    std::wstring dir, mark;
    char* thunks = nullptr;
    size_t committed = 0, committedCounters = 0;
    volatile LONG slotCount = 0;
    ThreadState* threads[kMaxThreads] = {};
    volatile LONG threadCount = 0;
    CRITICAL_SECTION hookLock, snapLock;
    FILE* slotsFile = nullptr;
    FILE* seriesFile = nullptr;
    uint64_t* internalSeen = nullptr;
    LARGE_INTEGER qpcFreq{}, qpc0{}, qpcLast{};
    uint64_t tscLast = 0;
    int snapshots = 0;
    bool finished = false;  // after the final snapshot: files closed, the snapshot thread only sleeps
    uint32_t vtables = 0;
};
State g;

thread_local ThreadState* t_state = nullptr;  // implicit TLS: unlike TlsGetValue it leaves GetLastError alone
thread_local bool t_inFlush = false;          // CallTrace_SetFlush
thread_local uint32_t t_phase = 0;            // CallTrace_SetPhase
constexpr uint32_t kFlushBit = 0x80000000u;   // in a site's slot: the call was made inside the renderer's outer flush
constexpr int kPhaseShift = 27;               // ... and bits 27-30: the frame phase it was made in (frame_phases.h)

char* Thunk(uint32_t slot) { return g.thunks + size_t(slot) * kThunkSize; }
void* OrigOf(uint32_t slot) { return *reinterpret_cast<void**>(Thunk(slot) + kOrigAt); }
volatile uint64_t* InternalOf(uint32_t slot) {
    return reinterpret_cast<volatile uint64_t*>(g.thunks + kCodeBytes) + slot;
}

// 64-bit counters written with add/adc (low half first) by their owner thread: read the halves until the high half
// is stable. A read that catches a carry in flight comes out too small, which callers handle (counters only grow).
uint64_t Read64(const volatile uint64_t* p) {
#ifdef _WIN64
    return *p;
#else
    auto* h = reinterpret_cast<const volatile uint32_t*>(p);
    for (;;) {
        const uint32_t hi = h[1], lo = h[0];
        if (h[1] == hi) return uint64_t(hi) << 32 | lo;
    }
#endif
}

template <class T>
T* Zeroed(size_t n) {
    return static_cast<T*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * sizeof(T)));
}

ThreadState* NewThreadState() {
    const DWORD err = GetLastError();
    const LONG index = InterlockedIncrement(&g.threadCount) - 1;
    ThreadState* ts = nullptr;
    if (index < kMaxThreads && (ts = Zeroed<ThreadState>(1)) != nullptr) {
        ts->index = index;
        ts->tid = GetCurrentThreadId();
        if (g.queryThread)
            g.queryThread(GetCurrentThread(), 9 /*ThreadQuerySetWin32StartAddress*/, &ts->start, sizeof ts->start, nullptr);
        ts->sites = Zeroed<Site>(size_t(kSiteMask) + 1);
        MemoryBarrier();
        g.threads[index] = ts;
    }
    t_state = ts ? ts : reinterpret_cast<ThreadState*>(1);  // 1 = no state (out of slots): don't try again
    SetLastError(err);
    return ts;
}

inline ThreadState* Current() {
    ThreadState* ts = t_state;
    if (!ts) ts = NewThreadState();
    return reinterpret_cast<uintptr_t>(ts) == 1 ? nullptr : ts;
}

inline Counter* CounterOf(ThreadState* ts, uint32_t slot) {
    Counter*& c = ts->chunks[slot / kChunk];
    if (!c) {
        Counter* fresh = Zeroed<Counter>(kChunk);
        if (!fresh) return nullptr;
        MemoryBarrier();
        c = fresh;
    }
    return &c[slot % kChunk];
}

inline void AddSite(ThreadState* ts, uintptr_t ret, uint32_t slot) {
    uint32_t h = uint32_t((ret >> 1) * 0x9E3779B1u ^ slot * 0x85EBCA77u) & kSiteMask;
    for (int probe = 0; probe < kSiteProbes; ++probe, h = (h + 1) & kSiteMask) {
        Site& s = ts->sites[h];
        if (s.ret == ret && s.slot == slot) {
            ++s.count;
            return;
        }
        if (s.ret == 0) {
            s.slot = slot;
            s.count = 1;
            _WriteBarrier();
            s.ret = ret;  // published last: the snapshot thread skips entries without it
            return;
        }
    }
    ++ts->siteOverflow;
}

}  // namespace
}  // namespace optiperf

using namespace optiperf;

#ifdef _WIN64
extern "C" void* TraceOnEnter64(uint32_t slot, uintptr_t ret) {
    if (ThreadState* ts = Current()) {
        if (Counter* c = CounterOf(ts, slot)) ++c->calls;
        AddSite(ts, ret, (t_inFlush ? slot | kFlushBit : slot) | t_phase << kPhaseShift);
    }
    return OrigOf(slot);
}
#else
// Per-slot thunks jump here for calls from other modules: [esp] = slot, [esp+4] = return address, then the arguments;
// ecx (this) and edx are preserved. Continues into the original function with [esp] = the (possibly swapped) return
// address.
extern "C" __declspec(naked) void TraceEnter() {
    __asm {
        push edx
        push ecx
        lea eax, [esp + 12]  // &return address
        push eax
        push dword ptr [esp + 12]  // slot
        call TraceOnEnter
        pop ecx
        pop edx
        mov [esp], eax
        ret
    }
}

// Timed calls return here instead of to their caller: eax/edx (and st0) still hold the return value.
extern "C" __declspec(naked) void TracePost() {
    __asm {
        push eax  // becomes the caller's return address
        push eax
        push edx
        lea ecx, [esp + 12]  // esp when the hooked function returned
        push ecx
        call TraceOnLeave
        mov [esp + 8], eax
        pop edx
        pop eax
        ret
    }
}

// Timed calls keep a frame per thread. Frames go stale when an exception unwinds past a timed call (its stub is never
// reached). Two rules keep the right frames:
//   - on entry, a frame is still live only if its return slot lies above this call's and still holds TracePost
//     (a live call's slot is above the stack pointer, so nothing overwrites it); everything else on top is dropped;
//   - on return, the frame is the outermost one whose return slot lies below the stack pointer at return: live outer
//     calls lie above it, and frames left above it can only be stale ones from calls nested in it.
extern "C" void* __stdcall TraceOnEnter(uint32_t slot, void** retSlot) {
    void* orig = OrigOf(slot);
    ThreadState* ts = Current();
    if (!ts) return orig;
    void* const ret = *retSlot;
    char* const r = reinterpret_cast<char*>(retSlot);
    if (ret == reinterpret_cast<void*>(&TracePost)) {
        // a timed call jumped here instead of returning (tail call): this runs as part of that call, so it counts as
        // a call from inside the module; frames nested in the jumping call are over
        while (ts->depth > 0 && ts->frames[ts->depth - 1].retSlot < r) --ts->depth, ++ts->stale;
        ++*InternalOf(slot);
        return orig;
    }
    if (Counter* c = CounterOf(ts, slot)) ++c->calls;
    AddSite(ts, uintptr_t(ret), slot);
    if (!g.timing) return orig;
    while (ts->depth > 0) {
        const Frame& top = ts->frames[ts->depth - 1];
        if (top.retSlot > r && *reinterpret_cast<void**>(top.retSlot) == reinterpret_cast<void*>(&TracePost)) break;
        --ts->depth;
        ++ts->stale;
    }
    // a call made while another traced call from outside is running (through a callback): also count it under the
    // outermost such call. Stored as a site whose "return address" is that call's slot + 1: below 64 KB, where no
    // code can be, so it cannot collide with a real call site.
    if (ts->depth > 0) AddSite(ts, uintptr_t(ts->frames[0].slot) + 1, slot);
    if (ts->depth >= kMaxDepth) {
        ++ts->untimed;
        return orig;
    }
    Frame& f = ts->frames[ts->depth++];
    f.ret = ret;
    f.retSlot = r;
    f.slot = slot;
    f.child = 0;
    *retSlot = reinterpret_cast<void*>(&TracePost);
    f.t0 = __rdtsc();
    return orig;
}

// Pops the frame of the call that just returned (see the rules above) and accounts its time.
extern "C" void* __stdcall TraceOnLeave(char* espAtLeave) {
    const uint64_t t = __rdtsc();
    ThreadState* ts = t_state;
    int i = ts->depth - 1;
    if (i < 0) {  // only if the stack was corrupted: there is no caller to return to
        Log("call trace: FATAL: a timed call returned without a frame on thread %lu", GetCurrentThreadId());
        TerminateProcess(GetCurrentProcess(), 0xC0DE0001);
    }
    while (i > 0 && ts->frames[i - 1].retSlot < espAtLeave) --i;
    Frame& f = ts->frames[i];
    ts->depth = i;
    const uint64_t dt = t - f.t0;
    if (Counter* c = CounterOf(ts, f.slot)) {
        c->incl += dt;
        c->excl += dt - (f.child < dt ? f.child : dt);
    }
    if (i > 0) ts->frames[i - 1].child += dt;
    return f.ret;
}
#endif

namespace optiperf {
namespace {

// --- thunks ------------------------------------------------------------------------------------------------------------
void Put(char*& p, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) *p++ = char(b);
}
void Put32(char*& p, uint32_t v) {
    std::memcpy(p, &v, 4);
    p += 4;
}

void WriteThunk(uint32_t slot, void* orig, char* modBase, uint32_t modSize) {
    char* t = Thunk(slot);
    std::memset(t, 0xCC, kThunkSize);
    char* p = t;
#ifdef _WIN64
    Put(p, {0x48, 0x8B, 0x04, 0x24});  // mov rax, [rsp]
    Put(p, {0x49, 0xBB});              // mov r11, module base
    std::memcpy(p, &modBase, 8);
    p += 8;
    Put(p, {0x4C, 0x29, 0xD8});  // sub rax, r11
    Put(p, {0x48, 0x3D});        // cmp rax, module size
    Put32(p, modSize);
    Put(p, {0x73, 0x00});  // jae external
    char* jae = p - 1;
    Put(p, {0x48, 0xFF, 0x05});  // inc qword [internal counter]
    Put32(p, uint32_t(reinterpret_cast<const volatile char*>(InternalOf(slot)) - (p + 4)));
    Put(p, {0xFF, 0x25});  // jmp [original]
    Put32(p, uint32_t(t + kOrigAt - (p + 4)));
    *jae = char(p - (jae + 1));
    Put(p, {0xB8});  // external: mov eax, slot
    Put32(p, slot);
    Put(p, {0xFF, 0x25});  // jmp [TraceEnter64]
    Put32(p, uint32_t(t + kCommonAt - (p + 4)));
    *reinterpret_cast<void**>(t + kCommonAt) = reinterpret_cast<void*>(&TraceEnter64);
#else
    Put(p, {0x8B, 0x04, 0x24});  // mov eax, [esp]
    Put(p, {0x2D});              // sub eax, module base
    Put32(p, uint32_t(modBase));
    Put(p, {0x3D});  // cmp eax, module size
    Put32(p, modSize);
    Put(p, {0x73, 0x00});  // jae external
    char* jae = p - 1;
    const uint32_t counter = uint32_t(reinterpret_cast<uintptr_t>(InternalOf(slot)));
    Put(p, {0x83, 0x05});  // add dword [counter], 1
    Put32(p, counter);
    Put(p, {0x01});
    Put(p, {0x83, 0x15});  // adc dword [counter + 4], 0
    Put32(p, counter + 4);
    Put(p, {0x00});
    Put(p, {0xFF, 0x25});  // jmp [original]
    Put32(p, uint32_t(t + kOrigAt));
    *jae = char(p - (jae + 1));
    Put(p, {0x68});  // external: push slot
    Put32(p, slot);
    Put(p, {0xE9});  // jmp TraceEnter
    Put32(p, uint32_t(reinterpret_cast<char*>(&TraceEnter) - (p + 4)));
#endif
    if (p > t + kOrigAt) {  // the code would run into the data (only if the encodings above change)
        Log("call trace: BUG: thunk code is %d bytes, data starts at %d", int(p - t), int(kOrigAt));
        TerminateProcess(GetCurrentProcess(), 0xC0DE0002);
    }
    *reinterpret_cast<void**>(t + kOrigAt) = orig;
    *InternalOf(slot) = 0;
}

// --- settings and files ------------------------------------------------------------------------------------------------
bool Wanted(const wchar_t* name) {
    if (g.all) {  // the engine's Cry<Name>.dll (case-sensitive: not Windows' CRYPT32.dll, cryptnet.dll, ...)
        const size_t n = wcslen(name);
        return wcsncmp(name, L"Cry", 3) == 0 && n > 7 && (iswupper(name[3]) || iswdigit(name[3])) &&
               _wcsicmp(name + n - 4, L".dll") == 0;
    }
    if (g.renderer && (_wcsicmp(name, L"CryRenderD3D9.dll") == 0 || _wcsicmp(name, L"CryRenderD3D10.dll") == 0))
        return true;
    for (const std::wstring& m : g.modules)
        if (_wcsicmp(name, m.c_str()) == 0) return true;
    return false;
}

FILE* Open(const wchar_t* file, const char* header) {
    FILE* f = _wfopen((g.dir + L"\\" + file).c_str(), L"wb");
    if (f && header) fputs(header, f);
    return f;
}

long long MarkSize() {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (g.mark.empty() || !GetFileAttributesExW(g.mark.c_str(), GetFileExInfoStandard, &a)) return -1;
    return (long long)a.nFileSizeHigh << 32 | a.nFileSizeLow;
}

void WriteModules() {
    FILE* f = Open(L"optiperf_trace_modules.tsv", "base\tsize\tname\r\n");
    if (!f) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    MODULEENTRY32W me{sizeof me};
    if (snap != INVALID_HANDLE_VALUE) {
        for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
            fprintf(f, "0x%llX\t0x%lX\t%ls\r\n", (unsigned long long)uintptr_t(me.modBaseAddr), me.modBaseSize, me.szModule);
        CloseHandle(snap);
    }
    fclose(f);
}

void WriteThreadsAndSites() {
    FILE* th = Open(L"optiperf_trace_threads.tsv", "thread\ttid\tstart\tname\tsite_overflow\tuntimed\tstale\r\n");
    FILE* si = Open(L"optiperf_trace_sites.tsv", "thread\tslot\tret\tcalls\r\n");
    const LONG n = (std::min)(LONG(kMaxThreads), LONG(g.threadCount));
    for (LONG i = 0; i < n; ++i) {
        ThreadState* ts = g.threads[i];
        if (!ts) continue;
        if (th) {
            char name[64] = "";
            Diag_ThreadName(ts->tid, name, sizeof name);
            char start[160];
            DescribeAddress(reinterpret_cast<void*>(ts->start), start, sizeof start);
            fprintf(th, "%ld\t%lu\t%s\t%s\t%llu\t%llu\t%llu\r\n", i, ts->tid, start, name, ts->siteOverflow, ts->untimed,
                    ts->stale);
        }
        if (si) {
            for (uint32_t k = 0; k <= kSiteMask; ++k) {
                const Site& s = ts->sites[k];
                const uintptr_t ret = s.ret;
                if (!ret) continue;
                _ReadBarrier();
                fprintf(si, "%ld\t%u\t0x%llX\t%llu\r\n", i, s.slot, (unsigned long long)ret,
                        (unsigned long long)Read64(&s.count));
            }
        }
    }
    if (th) fclose(th);
    if (si) fclose(si);
}

void Snapshot(bool final) {
    if (!final) {
        EnterCriticalSection(&g.snapLock);
    } else {
        // at exit the snapshot thread may have been stopped while holding the lock: wait a little, then give up
        for (int tries = 0; !TryEnterCriticalSection(&g.snapLock); ++tries) {
            if (tries == 150) return;
            Sleep(10);
        }
    }
    if (g.finished) {
        LeaveCriticalSection(&g.snapLock);
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint64_t tsc = __rdtsc();
    const double t = double(now.QuadPart - g.qpc0.QuadPart) / double(g.qpcFreq.QuadPart);
    const long long mark = MarkSize();
    FILE* f = g.seriesFile;
    if (f) {
        fprintf(f, "%.3f\t%lld\t-2\t0\t%llu\t%llu\t0\r\n", t, mark, (unsigned long long)(tsc - g.tscLast),
                (unsigned long long)((now.QuadPart - g.qpcLast.QuadPart) * 1000000 / g.qpcFreq.QuadPart));
        const LONG nThreads = (std::min)(LONG(kMaxThreads), LONG(g.threadCount));
        for (LONG i = 0; i < nThreads; ++i) {
            ThreadState* ts = g.threads[i];
            if (!ts) continue;
            for (uint32_t c = 0; c < kChunks; ++c) {
                Counter* cur = ts->chunks[c];
                if (!cur) continue;
                Counter*& seen = ts->seen[c];
                if (!seen && !(seen = Zeroed<Counter>(kChunk))) continue;
                for (uint32_t k = 0; k < kChunk; ++k) {
                    const uint64_t calls = Read64(&cur[k].calls);
                    if (calls <= seen[k].calls) continue;
                    const uint64_t incl = (std::max)(Read64(&cur[k].incl), seen[k].incl);
                    const uint64_t excl = (std::max)(Read64(&cur[k].excl), seen[k].excl);
                    fprintf(f, "%.3f\t%lld\t%ld\t%u\t%llu\t%llu\t%llu\r\n", t, mark, i, c * kChunk + k,
                            calls - seen[k].calls, incl - seen[k].incl, excl - seen[k].excl);
                    seen[k] = Counter{calls, incl, excl};
                }
            }
        }
        const LONG nSlots = g.slotCount;
        for (LONG s = 0; s < nSlots; ++s) {
            const uint64_t v = Read64(InternalOf(uint32_t(s)));
            if (v <= g.internalSeen[s]) continue;
            fprintf(f, "%.3f\t%lld\t-1\t%ld\t%llu\t0\t0\r\n", t, mark, s, v - g.internalSeen[s]);
            g.internalSeen[s] = v;
        }
        fflush(f);
    }
    g.qpcLast = now;
    g.tscLast = tsc;
    if (final || ++g.snapshots % 10 == 0) {
        WriteThreadsAndSites();
        WriteModules();
    }
    if (final) {
        g.finished = true;
        if (g.seriesFile) fclose(g.seriesFile);
        g.seriesFile = nullptr;
    }
    LeaveCriticalSection(&g.snapLock);
}

DWORD WINAPI Snapshotter(void*) {
    for (;;) {
        Sleep(1000);
        Snapshot(false);
    }
}

void HookModule(const wchar_t* name, char* base) {
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint32_t size = nt->OptionalHeader.SizeOfImage;
    const std::vector<RttiVtable> vts = FindRttiVtables(base, size, &CallTrace_Original);  // (past the gate's)
    if (vts.empty()) {
        Log("%ls: call trace: no RTTI vtables found (module built without RTTI, or not relocated yet)", name);
        return;
    }
    EnterCriticalSection(&g.hookLock);
    uint32_t hooked = 0, slots = 0;
    for (const RttiVtable& v : vts) {
        const uint32_t first = uint32_t(g.slotCount);
        if (first + v.slots > kMaxSlots) {
            Log("%ls: call trace: slot capacity (%u) reached; remaining vtables not hooked", name, kMaxSlots);
            break;
        }
        const size_t need = size_t(first + v.slots) * kThunkSize, needCounters = size_t(first + v.slots) * sizeof(uint64_t);
        bool ok = true;
        while (ok && g.committed < need) {
            ok = VirtualAlloc(g.thunks + g.committed, 1 << 16, MEM_COMMIT, PAGE_EXECUTE_READWRITE) != nullptr;
            if (ok) g.committed += 1 << 16;
        }
        while (ok && g.committedCounters < needCounters) {
            ok = VirtualAlloc(g.thunks + kCodeBytes + g.committedCounters, 1 << 16, MEM_COMMIT, PAGE_READWRITE) != nullptr;
            if (ok) g.committedCounters += 1 << 16;
        }
        DWORD old;
        if (!ok || !VirtualProtect(v.vtable, v.slots * sizeof(void*), PAGE_READWRITE, &old)) continue;
        for (uint32_t k = 0; k < v.slots; ++k) {
            const uint32_t id = first + k;
            WriteThunk(id, v.vtable[k], base, size);
            if (g.slotsFile)
                fprintf(g.slotsFile, "%u\t%ls\t0x%llX\t%s\t%u\t0x%X\t%u\t0x%X\r\n", id, name,
                        (unsigned long long)uintptr_t(base), v.mangled, v.offset,
                        unsigned(reinterpret_cast<char*>(v.vtable) - base), k, unsigned(v.vtable[k] - base));
        }
        FlushInstructionCache(GetCurrentProcess(), Thunk(first), v.slots * kThunkSize);
        MemoryBarrier();
        InterlockedExchange(&g.slotCount, LONG(first + v.slots));
        for (uint32_t k = 0; k < v.slots; ++k) v.vtable[k] = Thunk(first + k);  // pointer-sized: atomic for callers
        VirtualProtect(v.vtable, v.slots * sizeof(void*), old, &old);
        ++hooked;
        slots += v.slots;
    }
    if (g.slotsFile) fflush(g.slotsFile);
    g.vtables += hooked;
    LeaveCriticalSection(&g.hookLock);
    Log("%ls: call trace: %u of %zu vtables hooked (%u slots)", name, hooked, vts.size(), slots);
}

}  // namespace

void CallTrace_Init() {
    const std::wstring mods = ConfigString(L"Trace", L"Modules", L"OPTIPERF_TRACE", L"");
    if (mods.empty() || mods == L"0" || _wcsicmp(mods.c_str(), L"off") == 0) return;
    for (size_t s = 0; s <= mods.size();) {
        size_t e = mods.find(L',', s);
        if (e == std::wstring::npos) e = mods.size();
        std::wstring m = mods.substr(s, e - s);
        while (!m.empty() && m.front() == L' ') m.erase(m.begin());
        while (!m.empty() && m.back() == L' ') m.pop_back();
        if (_wcsicmp(m.c_str(), L"all") == 0)
            g.all = true;
        else if (_wcsicmp(m.c_str(), L"renderer") == 0)
            g.renderer = true;
        else if (!m.empty())
            g.modules.push_back(m);
        s = e + 1;
    }
#ifdef _WIN64
    g.timing = false;
#else
    g.timing = ConfigString(L"Trace", L"Timing", L"OPTIPERF_TRACE_TIMING", L"1") != L"0";
#endif
    g.mark = ConfigString(L"Trace", L"MarkFile", L"OPTIPERF_TRACE_MARK", L"");
    std::wstring log = ConfigString(L"Log", L"File", L"OPTIPERF_LOG", L"");
    const bool absolute = log.size() > 1 && (log[1] == L':' || (log[0] == L'\\' && log[1] == L'\\'));
    g.dir = absolute && log.find_last_of(L'\\') != std::wstring::npos ? log.substr(0, log.find_last_of(L'\\')) : PluginDir();

    g.thunks = static_cast<char*>(VirtualAlloc(nullptr, kCodeBytes + kCounterBytes, MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    g.internalSeen = Zeroed<uint64_t>(kMaxSlots);
    if (!g.thunks || !g.internalSeen) {
        Log("call trace: could not reserve memory; tracing off");
        return;
    }
    g.queryThread = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    InitializeCriticalSection(&g.hookLock);
    InitializeCriticalSection(&g.snapLock);
    QueryPerformanceFrequency(&g.qpcFreq);
    QueryPerformanceCounter(&g.qpc0);
    g.qpcLast = g.qpc0;
    g.tscLast = __rdtsc();
    g.slotsFile = Open(L"optiperf_trace_slots.tsv", "slot\tmodule\tbase\tclass\toffset\tvtable\tindex\ttarget\r\n");
    g.seriesFile = Open(L"optiperf_trace_series.tsv", "t\tmark\tthread\tslot\tcalls\tincl\texcl\r\n");
    if (!g.slotsFile || !g.seriesFile) {
        Log("call trace: could not create the output files in %ls; tracing off", g.dir.c_str());
        return;
    }
    g.enabled = true;
    if (HANDLE t = CreateThread(nullptr, 0, Snapshotter, nullptr, 0, nullptr)) CloseHandle(t);
    Log("call trace ON: modules=%ls timing=%d, output in %ls (developer tool: slows the game down)", mods.c_str(),
        int(g.timing), g.dir.c_str());
}

void CallTrace_OnModuleLoaded(const wchar_t* baseName, char* base, size_t /*size*/) {
    if (!g.enabled || !Wanted(baseName)) return;
    HookModule(baseName, base);
}

void CallTrace_Report(const char* when) {
    if (!g.enabled) return;
    const bool final = std::strcmp(when, "final") == 0;
    if (final) Snapshot(true);
    Log("call trace [%s]: %u vtables, %ld slots hooked, %ld threads seen", when, g.vtables, g.slotCount, g.threadCount);
}

void CallTrace_SetFlush(bool inFlush) { t_inFlush = inFlush; }
void CallTrace_SetPhase(int phase) { t_phase = uint32_t(phase) & 15; }
int CallTrace_Phase() { return int(t_phase); }

namespace {
// the hooks other parts registered (CallTrace_SeeThrough)
constexpr int kMaxSeeThrough = 64;
struct SeeThroughHook {
    void* hook;
    void* original;
};
SeeThroughHook g_seeThrough[kMaxSeeThrough];
int g_seeThroughCount = 0;
}  // namespace

void CallTrace_SeeThrough(void* hook, void* original) {
    if (g_seeThroughCount < kMaxSeeThrough) g_seeThrough[g_seeThroughCount++] = SeeThroughHook{hook, original};
}

void* CallTrace_Original(void* entry) {
    // through the tracer's thunks, the renderer gate's and the registered hooks, whichever wraps the other
    for (int depth = 0; depth < 4; ++depth) {
        void* const next = RenderGate_Original(entry);
        if (next != entry) {
            entry = next;
            continue;
        }
        bool hooked = false;
        for (int i = 0; i < g_seeThroughCount && !hooked; ++i)
            if (g_seeThrough[i].hook == entry) {
                entry = g_seeThrough[i].original;
                hooked = true;
            }
        if (hooked) continue;
        const char* p = static_cast<const char*>(entry);
        if (!g.thunks || p < g.thunks) return entry;
        const size_t off = size_t(p - g.thunks);
        if (off >= size_t(g.slotCount) * kThunkSize || off % kThunkSize) return entry;
        entry = OrigOf(uint32_t(off / kThunkSize));
    }
    return entry;
}

}  // namespace optiperf
