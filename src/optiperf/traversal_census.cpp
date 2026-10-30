// The traversal census: see traversal_census.h.
//
// The windows are made by two hooks in the 3D engine: C3DEngine::InitShadowFrustums (the shadow casters' walk is in
// it: InitShadowFrustum_SUN, then FillFrustumCastersList_SUN → MakeShadowCastersList per cascade) and
// C3DEngine::UpdateRNTmpDataPool, which RenderScene calls last. A census starts in the first InitShadowFrustums of a
// due frame on the main thread at recursion level 1 (the main view's; a reflection's runs at level 2), after its flush.
//
// Pages and their states: every committed read-write page of mimalloc's arenas and every writable page of the Cry*
// modules (optiperf's own memory is elsewhere: this file's buffers are VirtualAlloc'ed). Armed, a page is guarded;
// the first access in the window raises STATUS_GUARD_PAGE_VIOLATION (the system then takes the guard off). On the main
// thread a read is logged and the page made read-only, so that a later write raises an access violation, which is
// logged and the page opened; a first write is logged and the page left open. Another thread's access is logged
// (without a chain) and the page guarded (or made read-only) again after that one instruction (a single step), at most
// kOtherRearms times a page and window; after that the page stays open for the window (its later main-thread accesses
// are then missed: the record of the other thread's access says so). The handler takes no lock and allocates nothing.

#include "traversal_census.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#include "allocator.h"
#include "config.h"
#include "hooks.h"
#include "log.h"
#include "scan.h"
#include "shadow_prepass.h"

namespace optiperf {
namespace {

#ifdef _WIN64
// x64 Cry3DEngine.dll: C3DEngine::InitShadowFrustums(this) and C3DEngine::UpdateRNTmpDataPool(this, bool), from their
// starts (their profiler labels' one-time setup); research/profiling/re/mkpattern.py
constexpr const char kInitShadowFrustums[] =
    "48 83 EC 58 8B 05 ?? ?? ?? ?? 48 89 5C 24 60 48 89 6C 24 68 4C 89 64 24 50 4C 89 6C 24 48 4C 89 74 24 40 45 33 "
    "F6 A8 01 4C 8B E1 45 8D 6E 02 48 8D 1D ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 83 C8 01";
constexpr const char kUpdateRNTmpDataPool[] =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 8B 05 ?? ?? ?? ?? 33 DB 0F B6 EA A8 01 48 8B F9 48 "
    "8D 35 ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 83 C8 01 89 05 ?? ?? ?? ??";
// CPartManager::Update(this): every particle emitter's update (CParticleEmitter vtable slot 12), the dead ones removed
constexpr const char kPartManagerUpdate[] =
    "40 53 41 55 48 83 EC 78 8B 05 ?? ?? ?? ?? 4C 8B E9 48 8D 1D ?? ?? ?? ?? A8 01 0F 85 ?? ?? ?? ?? 83 C8 01 89 05 "
    "?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8B 08 48 8D 05 ?? ?? ?? ??";
constexpr unsigned kEmitterUpdateSlot = 12;  // CParticleEmitter::Update, as CPartManager::Update calls it (+0x60)

constexpr uintptr_t kPage = 0x1000;
constexpr size_t kMaxRecords = size_t(1) << 18;
constexpr size_t kMaxPages = size_t(1) << 20;  // 4 GB of pages
constexpr size_t kMaxRanges = size_t(1) << 16;
constexpr int kChain = 8;
constexpr uint8_t kOtherRearms = 2;
constexpr DWORD kTrapFlag = 0x100;
constexpr size_t kMaxFieldPages = 8192;  // pages a field census watches (the conflicts of the page census before it)
constexpr uint16_t kFieldAccesses = 2000;  // accesses a page and window a field census logs before it leaves it open

const wchar_t* const kModules[] = {L"Cry3DEngine.dll",  L"CryEntitySystem.dll", L"CryAnimation.dll",  L"CryAction.dll",
                                   L"CryGame.dll",      L"CrySystem.dll",       L"CryPhysics.dll",    L"CryAISystem.dll",
                                   L"CryScriptSystem.dll", L"CrySoundSystem.dll", L"CryNetwork.dll",  L"CryMovie.dll",
                                   L"CryInput.dll",     L"CryFont.dll",         L"CryRenderD3D10.dll"};

enum Window : int { kNone = 0, kCasters = 1, kRest = 2, kParticles = 3 };
const char* const kWindowNames[4] = {"-", "casters", "rest", "particles"};
enum Subject : int { kSubjectCasters = 0, kSubjectParticles = 1 };  // what the census watches
enum Kind : uint8_t { kRead = 0, kWrite = 1, kWriteAfterRead = 2 };
const char* const kKindNames[3] = {"R", "W", "WR"};  // first read; first write; a write after the first read
enum PageState : uint8_t { kUntracked = 0, kGuarded, kReadOnly, kOpen };

struct Record {
    uint8_t window, kind, main, gaveUp;  // gaveUp: an access after which the page stays open for the window
    uint32_t thread;
    uint32_t unit, pad;  // the particles' census: the emitter update running on the main thread (1, 2...; 0: none)
    uintptr_t data, code;
    uintptr_t chain[kChain];
};

struct Range {
    uintptr_t start, end;
    DWORD protect;  // its protection outside the windows (read-write, or copy-on-write for an image's)
    size_t firstPage;
    bool armed;
};

struct Pending {  // per thread: the page to protect again after this thread's single step
    uintptr_t page;
    DWORD protect;
    bool active;
};
thread_local Pending t_pending;

using InitShadowFn = void (*)(void*);
using UpdatePoolFn = void (*)(void*, uint64_t);
using PartUpdateFn = void (*)(void*);
using EmitterUpdateFn = void (*)(void*);
using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
constexpr size_t kMaxIoPages = 4096;

struct State {
    bool installed = false, setupTried = false, ready = false;
    bool fieldsMode = false;  // TraversalCensus=fields: each page census is followed by a field census the next frame
    bool field = false;       // the census running (or next) is a field census: every access to the pages it watches
    uintptr_t* fieldPages = nullptr;  // the page census's conflict pages, for the field census after it
    size_t fieldPageCount = 0;
    uint16_t* accesses = nullptr;     // a field census: accesses logged per page in the window
    uint64_t every = 400, max = 3, frames = 0, censuses = 0, pageCensuses = 0;
    DWORD mainThread = 0;
    int* level = nullptr;
    char* e3d = nullptr;
    InitShadowFn initShadow = nullptr;
    UpdatePoolFn updatePool = nullptr;
    PartUpdateFn partUpdate = nullptr;
    EmitterUpdateFn emitterUpdate = nullptr;
    int subject = kSubjectCasters;
    uint32_t unit = 0, units = 0;  // the particles' census: the emitter update running (main thread), and how many
    ReadFileFn readFile = nullptr;
    PVOID handler = nullptr;
    // pages a file read went into while armed: opened for it (the kernel's copy into a guarded or read-only page would
    // fail the read rather than raise an exception here), and kept open through the census frame
    uintptr_t ioPages[kMaxIoPages] = {};
    std::atomic<size_t> ioCount{0};
    bool due = false, restOpen = false;
    std::atomic<int> window{kNone};
    std::atomic<int> steps{0};  // other threads' single steps still to come (a disarm waits for them)
    // the buffers (VirtualAlloc: outside the arenas)
    Record* records = nullptr;
    std::atomic<size_t> count{0};
    Range* ranges = nullptr;
    size_t rangeCount = 0, pageCount = 0;
    uint8_t* pages = nullptr;   // PageState per tracked page
    uint8_t* rearms = nullptr;  // other threads' re-guards per page in the window
    // statistics
    uint64_t recordCounts[3][2] = {};  // window x (other threads, main)
    std::atomic<uint64_t> lost{0}, gaveUp{0};
    uint64_t armFailed = 0, pagesTracked = 0, arenaPages = 0, imagePages = 0, cyclesCasters = 0, cyclesRest = 0,
             cyclesWrite = 0, incomplete = 0, ioTotal = 0, swept = 0;
    std::wstring tsv;
};
State g;

// --- the tracked pages --------------------------------------------------------------------------------------------
void AddRange(uintptr_t start, uintptr_t end, DWORD protect) {
    if (end <= start || g.rangeCount >= kMaxRanges) return;
    const size_t n = (end - start) / kPage;
    if (g.pageCount + n > kMaxPages) return;
    g.ranges[g.rangeCount++] = {start, end, protect, 0, false};
    g.pageCount += n;
}

void AddCommitted(uintptr_t from, uintptr_t to, bool image) {
    for (uintptr_t a = from; a < to;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) != sizeof mbi) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = (std::min)(base + mbi.RegionSize, to);
        const DWORD p = mbi.Protect;
        if (mbi.State == MEM_COMMIT && (p == PAGE_READWRITE || (image && p == PAGE_WRITECOPY))) {
            const size_t before = g.pageCount;
            AddRange((std::max)(base, a), end, p);
            (image ? g.imagePages : g.arenaPages) += g.pageCount - before;
        }
        if (end <= a) break;
        a = end;
    }
}

void BuildRanges() {
    g.rangeCount = g.pageCount = 0;
    g.arenaPages = g.imagePages = 0;
    if (g.field) {  // the page census's conflict pages, each still committed and writable, runs of them merged
        for (size_t i = 0; i < g.fieldPageCount; ++i) {
            const uintptr_t p = g.fieldPages[i];
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT ||
                (mbi.Protect != PAGE_READWRITE && mbi.Protect != PAGE_WRITECOPY))
                continue;
            if (g.rangeCount && g.ranges[g.rangeCount - 1].end == p && g.ranges[g.rangeCount - 1].protect == mbi.Protect) {
                g.ranges[g.rangeCount - 1].end += kPage;
                ++g.pageCount;
            } else {
                AddRange(p, p + kPage, mbi.Protect);
            }
        }
        size_t first = 0;
        for (size_t i = 0; i < g.rangeCount; ++i) {
            g.ranges[i].firstPage = first;
            first += (g.ranges[i].end - g.ranges[i].start) / kPage;
        }
        g.pagesTracked = g.pageCount;
        return;
    }
    Allocator_ForEachArena(
        [](char* base, size_t size, void*) {
            AddCommitted(reinterpret_cast<uintptr_t>(base), reinterpret_cast<uintptr_t>(base) + size, false);
        },
        nullptr);
    for (const wchar_t* name : kModules) {
        auto* m = reinterpret_cast<char*>(GetModuleHandleW(name));
        if (!m) continue;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(m + reinterpret_cast<IMAGE_DOS_HEADER*>(m)->e_lfanew);
        IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
            if (!(s->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            const uintptr_t from = reinterpret_cast<uintptr_t>(m) + s->VirtualAddress;
            const uintptr_t to = (from + (std::max)(s->Misc.VirtualSize, s->SizeOfRawData) + kPage - 1) & ~(kPage - 1);
            AddCommitted(from, to, true);
        }
    }
    std::sort(g.ranges, g.ranges + g.rangeCount, [](const Range& x, const Range& y) { return x.start < y.start; });
    size_t first = 0;
    for (size_t i = 0; i < g.rangeCount; ++i) {
        g.ranges[i].firstPage = first;
        first += (g.ranges[i].end - g.ranges[i].start) / kPage;
    }
    g.pagesTracked = g.pageCount;
}

// the tracked range holding addr (armed, or any with all), or null
const Range* RangeOf(uintptr_t addr, bool all = false) {
    const Range* r = std::upper_bound(g.ranges, g.ranges + g.rangeCount, addr,
                                      [](uintptr_t a, const Range& x) { return a < x.start; });
    if (r == g.ranges) return nullptr;
    --r;
    return addr < r->end && (r->armed || all) ? r : nullptr;
}

void OpenPage(uintptr_t page, bool note);

void Arm(int window) {
    std::memset(g.pages, kUntracked, g.pageCount);
    std::memset(g.rearms, 0, g.pageCount);
    if (g.field) std::memset(g.accesses, 0, g.pageCount * sizeof(uint16_t));
    for (size_t i = 0; i < g.rangeCount; ++i) {
        Range& r = g.ranges[i];
        DWORD old;
        r.armed = VirtualProtect(reinterpret_cast<void*>(r.start), r.end - r.start, r.protect | PAGE_GUARD, &old) != 0;
        if (r.armed)
            std::memset(g.pages + r.firstPage, kGuarded, (r.end - r.start) / kPage);
        else
            ++g.armFailed;
    }
    // the pages file reads have gone into this census frame stay open (an overlapped read may still be filling them)
    const size_t io = (std::min)(g.ioCount.load(std::memory_order_acquire), kMaxIoPages);
    for (size_t i = 0; i < io; ++i) OpenPage(g.ioPages[i], false);
    g.window.store(window, std::memory_order_release);
}

// a page this census guards or made read-only, back to its usual protection for the rest of the census frame
void OpenPage(uintptr_t page, bool note) {
    const Range* r = RangeOf(page);
    if (!r) return;
    const size_t pi = r->firstPage + (page - r->start) / kPage;
    if (g.pages[pi] == kOpen) return;
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(page), kPage, r->protect, &old);
    g.pages[pi] = kOpen;
    if (!note) return;
    const size_t i = g.ioCount.fetch_add(1, std::memory_order_acq_rel);
    if (i < kMaxIoPages) g.ioPages[i] = page;
}

BOOL WINAPI ReadFileHook(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED ov) {
    if (g.window.load(std::memory_order_acquire) != kNone && buf && n)
        for (uintptr_t p = uintptr_t(buf) & ~(kPage - 1); p < uintptr_t(buf) + n; p += kPage) OpenPage(p, true);
    return g.readFile(h, buf, n, read, ov);
}

void Disarm() {
    g.window.store(kNone, std::memory_order_release);
    // an other thread's single step still to come would protect its page again after the restore below: wait for them
    // (a step that sees no window protects nothing; 200 ms at most, for a step that never comes)
    const ULONGLONG until = GetTickCount64() + 200;
    while (g.steps.load(std::memory_order_acquire) > 0 && GetTickCount64() < until) Sleep(0);
    for (size_t i = 0; i < g.rangeCount; ++i) {
        Range& r = g.ranges[i];
        if (!r.armed) continue;
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(r.start), r.end - r.start, r.protect, &old))
            for (uintptr_t p = r.start; p < r.end; p += kPage) {  // (a page decommitted meanwhile fails the whole call)
                MEMORY_BASIC_INFORMATION mbi;
                if (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_COMMIT)
                    VirtualProtect(reinterpret_cast<void*>(p), kPage, r.protect, &old);
            }
        r.armed = false;
    }
}

// --- the handler ---------------------------------------------------------------------------------------------------
void Note(int window, uint8_t kind, bool main, bool gaveUp, uintptr_t data, const EXCEPTION_POINTERS* e,
          bool chain = true) {
    const size_t i = g.count.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMaxRecords) {
        g.lost.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Record& r = g.records[i];
    r.window = uint8_t(window);
    r.kind = kind;
    r.main = main;
    r.gaveUp = gaveUp;
    r.thread = GetCurrentThreadId();
    r.unit = main ? g.unit : 0;
    r.data = data;
    r.code = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    std::memset(r.chain, 0, sizeof r.chain);
    if (!main || !chain) return;
    CONTEXT c = *e->ContextRecord;  // the faulting function's callers, unwound from its context
    for (int k = 0; k < kChain; ++k) {
        DWORD64 imageBase = 0;
        RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (f) {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, f, &c, &handlerData, &establisher, nullptr);
        } else {  // a leaf: its return address at [rsp]
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
        }
        if (!c.Rip) break;
        r.chain[k] = uintptr_t(c.Rip);
    }
}

// an other thread's access: protect the page again (with `protect`) after this one instruction
void StepThenProtect(EXCEPTION_POINTERS* e, uintptr_t page, DWORD protect) {
    t_pending = {page, protect, true};
    g.steps.fetch_add(1, std::memory_order_acq_rel);
    e->ContextRecord->EFlags |= kTrapFlag;
}

bool Writable(DWORD p) {
    p &= 0xFF;
    return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

LONG CALLBACK OnFault(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* x = e->ExceptionRecord;
    const DWORD code = x->ExceptionCode;
    DWORD old;
    if (code == STATUS_SINGLE_STEP) {
        if (!t_pending.active) return EXCEPTION_CONTINUE_SEARCH;
        if (g.window.load(std::memory_order_acquire) != kNone)  // (disarmed meanwhile: nothing to protect again)
            VirtualProtect(reinterpret_cast<void*>(t_pending.page), kPage, t_pending.protect, &old);
        t_pending.active = false;
        g.steps.fetch_sub(1, std::memory_order_acq_rel);
        e->ContextRecord->EFlags &= ~kTrapFlag;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if ((code != STATUS_GUARD_PAGE_VIOLATION && code != STATUS_ACCESS_VIOLATION) || x->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t data = uintptr_t(x->ExceptionInformation[1]);
    const Range* any = RangeOf(data, true);
    if (!any) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t page = data & ~(kPage - 1);
    const bool write = x->ExceptionInformation[0] == 1;
    const int window = g.window.load(std::memory_order_acquire);
    if (code == STATUS_ACCESS_VIOLATION) {
        // another thread may have opened the page since this access was refused (or a disarm restored it): try again;
        // and a page left read-only by a race once the window is over is given back its protection
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<void*>(page), &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT)
            return EXCEPTION_CONTINUE_SEARCH;
        if (!write) {  // (this census never makes a page unreadable)
            const DWORD p = mbi.Protect & 0xFF;
            return p != PAGE_NOACCESS && !(mbi.Protect & PAGE_GUARD) ? EXCEPTION_CONTINUE_EXECUTION
                                                                     : EXCEPTION_CONTINUE_SEARCH;
        }
        if (Writable(mbi.Protect)) return EXCEPTION_CONTINUE_EXECUTION;
        if ((mbi.Protect & 0xFF) != PAGE_READONLY) return EXCEPTION_CONTINUE_SEARCH;  // (not a page this census set)
        if (window == kNone || !any->armed) {
            VirtualProtect(reinterpret_cast<void*>(page), kPage, any->protect, &old);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    } else if (window == kNone || !any->armed) {
        return EXCEPTION_CONTINUE_EXECUTION;  // a guard from just before a disarm, or left by a race: it is off now
    }
    const Range* r = any;
    const size_t pi = r->firstPage + (data - r->start) / kPage;
    const bool main = GetCurrentThreadId() == g.mainThread;
    if (code == STATUS_GUARD_PAGE_VIOLATION && g.field) {  // a field census: every access, the page guarded again
        const uint16_t n = g.accesses[pi];
        // (the particles' pages are mostly an emitter's own particles, written all over: a few accesses show who
        // touches a page; and once the records are full, nothing more to log)
        const uint16_t cap = g.subject == kSubjectParticles ? 48 : kFieldAccesses;
        const bool more = n < cap && !t_pending.active && g.count.load(std::memory_order_relaxed) < kMaxRecords;
        Note(window, write ? kWrite : kRead, main, !more, data, e, n == 0);
        if (more) {
            g.accesses[pi] = uint16_t(n + 1);
            StepThenProtect(e, page, r->protect | PAGE_GUARD);
        } else {
            g.pages[pi] = kOpen;
            g.gaveUp.fetch_add(1, std::memory_order_relaxed);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == STATUS_GUARD_PAGE_VIOLATION) {  // (the guard is off the page now: it has its usual protection)
        if (main) {
            Note(window, write ? kWrite : kRead, true, false, data, e);
            if (write) {
                g.pages[pi] = kOpen;
            } else {
                VirtualProtect(reinterpret_cast<void*>(page), kPage, PAGE_READONLY, &old);
                g.pages[pi] = kReadOnly;
            }
        } else {
            const bool more = g.rearms[pi] < kOtherRearms;
            Note(window, write ? kWrite : kRead, false, !more, data, e);
            if (more && !t_pending.active) {
                ++g.rearms[pi];
                StepThenProtect(e, page, r->protect | PAGE_GUARD);
            } else {
                g.pages[pi] = kOpen;
                g.gaveUp.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // a write to a page this census made read-only (the access violations above that weren't are taken care of)
    VirtualProtect(reinterpret_cast<void*>(page), kPage, r->protect, &old);
    if (main) {
        Note(window, kWriteAfterRead, true, false, data, e);
        g.pages[pi] = kOpen;
    } else {
        Note(window, kWriteAfterRead, false, false, data, e);
        if (!t_pending.active)  // read-only again after its write, for the main thread's
            StepThenProtect(e, page, PAGE_READONLY);
        else
            g.pages[pi] = kOpen;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// --- the records' file ---------------------------------------------------------------------------------------------
struct ModName {
    HMODULE h;
    char name[64];
};
ModName g_mods[64];
int g_modCount = 0;

// "Module.dll+rva" for an address in a module, else its hex
void Where(uintptr_t a, char* out, size_t n) {
    HMODULE h = nullptr;
    if (a && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(a), &h) && h) {
        const char* name = nullptr;
        for (int i = 0; i < g_modCount; ++i)
            if (g_mods[i].h == h) name = g_mods[i].name;
        if (!name && g_modCount < 64) {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(h, path, MAX_PATH);
            const wchar_t* b = wcsrchr(path, L'\\');
            ModName& m = g_mods[g_modCount++];
            m.h = h;
            WideCharToMultiByte(CP_UTF8, 0, b ? b + 1 : path, -1, m.name, int(sizeof m.name), nullptr, nullptr);
            name = m.name;
        }
        snprintf(out, n, "%s+%llX", name ? name : "?", static_cast<unsigned long long>(a - uintptr_t(h)));
        return;
    }
    snprintf(out, n, "%llX", static_cast<unsigned long long>(a));
}

void WriteRecords() {
    const uint64_t t0 = __rdtsc();
    const size_t n = (std::min)(g.count.load(std::memory_order_relaxed), kMaxRecords);
    HANDLE f = CreateFileW(g.tsv.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log("traversal census: could not open %ls", g.tsv.c_str());
        return;
    }
    LARGE_INTEGER size;
    std::string out;
    out.reserve(1 << 20);
    if (GetFileSizeEx(f, &size) && size.QuadPart == 0)
        out += "census\tgranularity\twindow\tkind\tmain\tgave_up\tthread\tunit\tdata\tblock_size\tblock_offset\t"
               "allocated_by\tcode\tchain\n";
    char line[1536], data[96], code[96], site[96], hop[96];
    DWORD written;
    for (size_t i = 0; i < n; ++i) {
        const Record& r = g.records[i];
        if (r.window == kNone || r.window > kParticles) continue;
        if (r.window <= kRest) ++g.recordCounts[r.window][r.main ? 1 : 0];
        const void* s = nullptr;
        const char* start = nullptr;
        size_t bytes = 0;
        unsigned long long blockSize = 0, offset = 0;
        site[0] = '-';
        site[1] = 0;
        if (Allocator_OwnerOf(reinterpret_cast<const void*>(r.data), &s, &start, &bytes)) {
            blockSize = bytes;
            offset = r.data - uintptr_t(start);
            Where(uintptr_t(s), site, sizeof site);
            snprintf(data, sizeof data, "heap:%llX", static_cast<unsigned long long>(r.data));
        } else {
            Where(r.data, data, sizeof data);
            if (!std::strchr(data, '+')) {  // (an arena page outside mimalloc's blocks: its metadata, or a freed block)
                char raw[40];
                snprintf(raw, sizeof raw, "heap:%s", data);
                snprintf(data, sizeof data, "%s", raw);
            }
        }
        Where(r.code, code, sizeof code);
        int len = snprintf(line, sizeof line, "%llu\t%s\t%s\t%s\t%d\t%d\t%u\t%u\t%s\t%llu\t%llu\t%s\t%s\t",
                           static_cast<unsigned long long>(g.censuses), g.field ? "field" : "page",
                           kWindowNames[r.window], kKindNames[r.kind], int(r.main), int(r.gaveUp), r.thread, r.unit,
                           data, blockSize, offset, site, code);
        for (int k = 0; k < kChain && r.chain[k] && len < int(sizeof line) - 100; ++k) {
            Where(r.chain[k], hop, sizeof hop);
            len += snprintf(line + len, sizeof line - size_t(len), "%s%s", k ? ";" : "", hop);
        }
        line[len++] = '\n';
        out.append(line, size_t(len));
        if (out.size() > (size_t(1) << 20) - 2048) {
            WriteFile(f, out.data(), DWORD(out.size()), &written, nullptr);
            out.clear();
        }
    }
    if (!out.empty()) WriteFile(f, out.data(), DWORD(out.size()), &written, nullptr);
    CloseHandle(f);
    g.cyclesWrite += __rdtsc() - t0;
}

// a page census's conflict pages: written by the main thread in one window and touched by it in the other
void PickFieldPages() {
    const size_t n = (std::min)(g.count.load(std::memory_order_relaxed), kMaxRecords);
    std::vector<std::pair<uintptr_t, uint8_t>> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const Record& r = g.records[i];
        if (!r.main || r.window == kNone) continue;
        const bool w = r.kind != kRead;
        v.push_back({r.data & ~(kPage - 1), uint8_t(r.window == kRest ? 4 | (w ? 8 : 0) : 1 | (w ? 2 : 0))});
    }
    std::sort(v.begin(), v.end());
    g.fieldPageCount = 0;
    for (size_t i = 0; i < v.size() && g.fieldPageCount < kMaxFieldPages;) {
        size_t j = i;
        uint8_t bits = 0;
        for (; j < v.size() && v[j].first == v[i].first; ++j) bits |= v[j].second;
        // casters: written in one window and touched in the other; particles (one window): written by the main
        // thread at all (a word one emitter writes and another touches is on such a page)
        const bool pick = g.subject == kSubjectParticles ? (bits & 2) != 0
                                                         : ((bits & 2) && (bits & 4)) || ((bits & 8) && (bits & 1));
        if (pick) g.fieldPages[g.fieldPageCount++] = v[i].first;
        i = j;
    }
}

// after a census: any tracked page still guarded or read-only (a race left it so) gets its protection back
void Sweep() {
    for (size_t i = 0; i < g.rangeCount; ++i) {
        const Range& r = g.ranges[i];
        for (uintptr_t a = r.start; a < r.end;) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) != sizeof mbi) break;
            const uintptr_t end = (std::min)(reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize, r.end);
            if (mbi.State == MEM_COMMIT && ((mbi.Protect & PAGE_GUARD) || (mbi.Protect & 0xFF) == PAGE_READONLY)) {
                DWORD old;
                if (VirtualProtect(reinterpret_cast<void*>(a), end - a, r.protect, &old)) g.swept += (end - a) / kPage;
            }
            if (end <= a) break;
            a = end;
        }
    }
}

void EndCensus() {
    Disarm();
    Sweep();
    // between censuses the handler takes no page for its own (none is protected by this census any more)
    const size_t rangeCount = g.rangeCount;
    g.rangeCount = 0;
    std::atomic_thread_fence(std::memory_order_release);
    g.restOpen = false;
    g.ioTotal += g.ioCount.load(std::memory_order_relaxed);
    WriteRecords();
    Log("traversal census %llu (%s): %zu records (%llu lost), %zu pages tracked in %zu ranges (%llu heap, %llu image; "
        "%llu ranges not armed); the casters' window %.1f ms, the rest's %.1f ms",
        static_cast<unsigned long long>(g.censuses), g.field ? "fields" : "pages",
        (std::min)(g.count.load(), kMaxRecords), static_cast<unsigned long long>(g.lost.load()), g.pageCount,
        rangeCount, static_cast<unsigned long long>(g.arenaPages), static_cast<unsigned long long>(g.imagePages),
        static_cast<unsigned long long>(g.armFailed), double(g.cyclesCasters) / 3.0e6, double(g.cyclesRest) / 3.0e6);
    if (g.fieldsMode && !g.field) {  // a page census: its conflicts watched access by access in the next frame
        PickFieldPages();
        g.field = g.fieldPageCount > 0;
        g.due = g.field;
        Log("traversal census: %zu conflict pages for the field census in the next frame", g.fieldPageCount);
    } else {
        g.field = false;
    }
}

// --- the hooks -----------------------------------------------------------------------------------------------------
void InitShadowFrustumsHook(void* self) {
    if (!g.due || !g.ready || GetCurrentThreadId() != g.mainThread || !g.level || *g.level != 1)
        return g.initShadow(self);
    g.due = false;
    ++g.censuses;
    if (!g.field) ++g.pageCensuses;
    g.count.store(0, std::memory_order_relaxed);
    g.ioCount.store(0, std::memory_order_relaxed);
    BuildRanges();
    uint64_t t0 = __rdtsc();
    Arm(kCasters);
    g.initShadow(self);
    Disarm();
    const uint64_t t1 = __rdtsc();
    g.cyclesCasters = t1 - t0;
    Arm(kRest);
    g.restOpen = true;
    g.cyclesRest = __rdtsc();  // (the start; the window's length is taken at its end)
}

// the particles' census: CPartManager::Update is its one window, each emitter's update in it a unit
void PartUpdateHook(void* self) {
    if (!g.due || !g.ready || GetCurrentThreadId() != g.mainThread) return g.partUpdate(self);
    g.due = false;
    ++g.censuses;
    if (!g.field) ++g.pageCensuses;
    g.count.store(0, std::memory_order_relaxed);
    g.ioCount.store(0, std::memory_order_relaxed);
    g.units = 0;
    BuildRanges();
    const uint64_t t0 = __rdtsc();
    Arm(kParticles);
    g.partUpdate(self);
    g.cyclesCasters = __rdtsc() - t0;
    g.cyclesRest = 0;
    EndCensus();
}

void EmitterUpdateHook(void* self) {
    if (g.window.load(std::memory_order_acquire) != kParticles || GetCurrentThreadId() != g.mainThread)
        return g.emitterUpdate(self);
    const uint32_t was = g.unit;
    g.unit = ++g.units;
    g.emitterUpdate(self);
    g.unit = was;
}

void UpdatePoolHook(void* self, uint64_t b) {
    if (g.restOpen && GetCurrentThreadId() == g.mainThread) {
        g.cyclesRest = __rdtsc() - g.cyclesRest;
        EndCensus();
    }
    g.updatePool(self, b);
}

char* Unique(char* base, const char* text, const char** why, const char* what) {
    const Pattern p = ParsePattern(text);
    const auto hits = FindInCode(base, p, 2);
    if (hits.size() != 1) {
        *why = what;
        return nullptr;
    }
    return hits[0];
}

void Setup() {
    g.setupTried = true;
    g.mainThread = GetCurrentThreadId();
    g.e3d = reinterpret_cast<char*>(GetModuleHandleW(L"Cry3DEngine.dll"));
    const char* why = g.e3d ? nullptr : "no Cry3DEngine.dll";
    char *init = nullptr, *pool = nullptr, *partUpdate = nullptr;
    void** emitterVt = nullptr;
    if (g.e3d && g.subject == kSubjectCasters) {
        init = Unique(g.e3d, kInitShadowFrustums, &why, "C3DEngine::InitShadowFrustums not found");
        pool = init ? Unique(g.e3d, kUpdateRNTmpDataPool, &why, "C3DEngine::UpdateRNTmpDataPool not found") : nullptr;
    } else if (g.e3d) {
        partUpdate = Unique(g.e3d, kPartManagerUpdate, &why, "CPartManager::Update not found");
        emitterVt = partUpdate ? FindRttiVtable(g.e3d, ".?AVCParticleEmitter@@") : nullptr;
        if (partUpdate && (!emitterVt || !IsInCode(g.e3d, static_cast<const char*>(emitterVt[kEmitterUpdateSlot]), 1)))
            why = "CParticleEmitter's vtable not found";
    }
    if (g.subject == kSubjectCasters ? !pool : !(partUpdate && emitterVt && !why)) {
        Log("traversal census: off: %s", why ? why : "?");
        return;
    }
    g.records = static_cast<Record*>(VirtualAlloc(nullptr, kMaxRecords * sizeof(Record), MEM_COMMIT | MEM_RESERVE,
                                                  PAGE_READWRITE));
    g.ranges = static_cast<Range*>(VirtualAlloc(nullptr, kMaxRanges * sizeof(Range), MEM_COMMIT | MEM_RESERVE,
                                                PAGE_READWRITE));
    g.pages = static_cast<uint8_t*>(VirtualAlloc(nullptr, kMaxPages, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g.rearms = static_cast<uint8_t*>(VirtualAlloc(nullptr, kMaxPages, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g.accesses = static_cast<uint16_t*>(VirtualAlloc(nullptr, kMaxPages * sizeof(uint16_t), MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_READWRITE));
    g.fieldPages = static_cast<uintptr_t*>(VirtualAlloc(nullptr, kMaxFieldPages * sizeof(uintptr_t),
                                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g.records || !g.ranges || !g.pages || !g.rearms || !g.accesses || !g.fieldPages) {
        Log("traversal census: off: could not allocate its buffers");
        return;
    }
    const std::wstring log = LogPath();
    const size_t slash = log.find_last_of(L"\\/");
    if (log.empty() || slash == std::wstring::npos) {  // (never next to the game's executable)
        Log("traversal census: off: no log file to write its records next to");
        return;
    }
    g.tsv = log.substr(0, slash + 1) + L"traversal_census.tsv";
    void* readFile = GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "ReadFile");
    if (g.subject == kSubjectCasters) {
        HookSpec hooks[] = {
            {"C3DEngine::InitShadowFrustums", init, reinterpret_cast<void*>(&InitShadowFrustumsHook),
             reinterpret_cast<void**>(&g.initShadow)},
            {"C3DEngine::UpdateRNTmpDataPool", pool, reinterpret_cast<void*>(&UpdatePoolHook),
             reinterpret_cast<void**>(&g.updatePool)},
            {"kernelbase!ReadFile", readFile, reinterpret_cast<void*>(&ReadFileHook),
             reinterpret_cast<void**>(&g.readFile)},
        };
        if (!readFile || !InstallHooks(hooks, 3)) {
            Log("traversal census: off: could not write the hooks");
            return;
        }
    } else {
        HookSpec hooks[] = {
            {"CPartManager::Update", partUpdate, reinterpret_cast<void*>(&PartUpdateHook),
             reinterpret_cast<void**>(&g.partUpdate)},
            {"kernelbase!ReadFile", readFile, reinterpret_cast<void*>(&ReadFileHook),
             reinterpret_cast<void**>(&g.readFile)},
        };
        if (!readFile || !InstallHooks(hooks, 2)) {
            Log("traversal census: off: could not write the hooks");
            return;
        }
        // each emitter's update through the class's vtable (the call CPartManager::Update makes)
        DWORD old;
        void** slot = &emitterVt[kEmitterUpdateSlot];
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            Log("traversal census: off: could not write CParticleEmitter's vtable");
            return;
        }
        g.emitterUpdate = reinterpret_cast<EmitterUpdateFn>(*slot);
        InterlockedExchangePointer(slot, reinterpret_cast<void*>(&EmitterUpdateHook));
        VirtualProtect(slot, sizeof(void*), old, &old);
        Log("traversal census: particles: CPartManager::Update +0x%X, CParticleEmitter's update (vtable slot %u) "
            "+0x%X",
            unsigned(partUpdate - g.e3d), kEmitterUpdateSlot,
            unsigned(reinterpret_cast<char*>(g.emitterUpdate) - g.e3d));
    }
    g.handler = AddVectoredExceptionHandler(1, &OnFault);
    g.ready = g.handler != nullptr;
    if (g.subject == kSubjectCasters)
        Log("traversal census: on (one frame in %llu, %llu at most): C3DEngine::InitShadowFrustums +0x%X, "
            "UpdateRNTmpDataPool +0x%X; records to %ls%s",
            static_cast<unsigned long long>(g.every), static_cast<unsigned long long>(g.max), unsigned(init - g.e3d),
            unsigned(pool - g.e3d), g.tsv.c_str(),
            Allocator_TracksOwners() ? "" : " (no owner tags: [Allocator] Owners=off)");
    else
        Log("traversal census: on (the particles, one frame in %llu, %llu at most); records to %ls%s",
            static_cast<unsigned long long>(g.every), static_cast<unsigned long long>(g.max), g.tsv.c_str(),
            Allocator_TracksOwners() ? "" : " (no owner tags: [Allocator] Owners=off)");
}

uint64_t ReadNumber(const wchar_t* key, const wchar_t* env, uint64_t def) {
    const std::wstring v = ConfigString(L"Debug", key, env, L"");
    return v.empty() ? def : wcstoull(v.c_str(), nullptr, 10);
}
#endif  // _WIN64

}  // namespace

void TraversalCensus_Install(const wchar_t* moduleName, const ShadowPrepassTarget& sp) {
#ifndef _WIN64
    (void)sp;
    if (ConfigString(L"Debug", L"TraversalCensus", L"OPTIPERF_TRAVERSAL_CENSUS", L"off") != L"off")
        Log("%ls: traversal census off: 64-bit game only", moduleName);
#else
    if (g.installed) return;
    const std::wstring v = ConfigString(L"Debug", L"TraversalCensus", L"OPTIPERF_TRAVERSAL_CENSUS", L"off");
    const bool particles = _wcsicmp(v.c_str(), L"particles") == 0;  // (always page then field censuses)
    g.fieldsMode = particles || _wcsicmp(v.c_str(), L"fields") == 0;
    if (_wcsicmp(v.c_str(), L"casters") != 0 && !g.fieldsMode) return;
    g.subject = particles ? kSubjectParticles : kSubjectCasters;
    g.every = (std::max)(uint64_t(2), ReadNumber(L"TraversalCensusEvery", L"OPTIPERF_TRAVERSAL_CENSUS_EVERY", 400));
    g.max = ReadNumber(L"TraversalCensusMax", L"OPTIPERF_TRAVERSAL_CENSUS_MAX", 3);
    g.level = sp.level;
    g.installed = true;
    Log("%ls: traversal census: %s (set up at the second flush)", moduleName,
        particles      ? "particles (the emitters' updates, each a unit; page then field censuses)"
        : g.fieldsMode ? "fields (each page census followed by a field census)"
                       : "casters");
#endif
}

void TraversalCensus_FlushEntry() {
#ifdef _WIN64
    if (!g.installed) return;
    if (!g.setupTried) {
        if (++g.frames >= 2) Setup();
        return;
    }
    if (!g.ready || GetCurrentThreadId() != g.mainThread) return;
    if (g.restOpen) {  // (UpdateRNTmpDataPool wasn't reached in that frame: the census ends here)
        g.cyclesRest = __rdtsc() - g.cyclesRest;
        ++g.incomplete;
        EndCensus();
    }
    ++g.frames;
    if (!g.field && g.pageCensuses < g.max && g.frames % g.every == 0) g.due = true;
#endif
}

void TraversalCensus_Report(const char* when) {
#ifdef _WIN64
    if (!g.installed || !g.ready) return;
    Log("traversal census [%s]: %llu censuses (%llu without UpdateRNTmpDataPool) | records: casters %llu main + %llu "
        "other threads, rest %llu + %llu; lost %llu; pages other threads kept open %llu, file reads' pages opened "
        "%llu, pages a race left protected (fixed after the census) %llu | writing them %.1f ms",
        when, static_cast<unsigned long long>(g.censuses), static_cast<unsigned long long>(g.incomplete),
        static_cast<unsigned long long>(g.recordCounts[kCasters][1]),
        static_cast<unsigned long long>(g.recordCounts[kCasters][0]),
        static_cast<unsigned long long>(g.recordCounts[kRest][1]),
        static_cast<unsigned long long>(g.recordCounts[kRest][0]),
        static_cast<unsigned long long>(g.lost.load()), static_cast<unsigned long long>(g.gaveUp.load()),
        static_cast<unsigned long long>(g.ioTotal), static_cast<unsigned long long>(g.swept),
        double(g.cyclesWrite) / 3.0e6);
#else
    (void)when;
#endif
}

}  // namespace optiperf
