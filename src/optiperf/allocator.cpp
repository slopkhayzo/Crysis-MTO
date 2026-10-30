// Replaces CryEngine's allocator (CrySystem.dll: CryMalloc / CryFree / CryRealloc) with mimalloc.
//
// Why: the original small-block allocator serialises every allocation of every thread behind one spinlock (a wall for
// any multithreading work), and in the 64-bit build it stores free-list links as 32-bit values: once memory comes
// from above 4 GB (modern drivers reserve much of the low address space) the lists corrupt and the game crashes at
// level load (research/docs/cpu-multithreading-plan.md, section 3.5).
//
// Contract kept from the original (CryGetMemSize's code is checked once at hook time, it encodes the block layout):
//   CryMalloc(size, &allocated) -> p   with int `size` stored at p-4, allocated = size + 4
//   CryFree(p) -> size + 4             CryRealloc(p, size, &allocated): null p = CryMalloc; else move the block
// New blocks are 16-byte aligned (the original's are only 4-byte aligned, so callers cope with either).
//
// The plugin may be loaded after the engine has started allocating (Ultimate ASI Loader loads plugins once game code
// runs), so pointers that mimalloc doesn't own go back to the original functions.
//
// 64-bit: the engine also truncates heap pointers outside the allocator (CrySystem's own init writes through a pointer
// cut to 32 bits), so all engine memory is kept below 4 GB: mimalloc only uses address space reserved there.
//
// Owner tags (x64, a diagnostic, off by default: the pointer census, census.cpp): each block keeps the address of the
// code that allocated it (CryMalloc's caller; CryRealloc's for a block it moved) in the mimalloc block's first 8 bytes,
// before the header, which nothing else uses. A freed block's first bytes are mimalloc's free-list link instead.
//
// [Allocator] Mode = mimalloc (default) | off            (env OPTIPERF_ALLOCATOR)
//             LowMemoryMB = 1536  (x64: address space reserved below 4 GB up front; more is added on demand)
//             Owners = off (default) | on                (env OPTIPERF_ALLOCATOR_OWNERS)

#include "allocator.h"

#include <windows.h>
#include <intrin.h>

#include <mimalloc.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "config.h"
#include "hooks.h"
#include "log.h"
#include "scan.h"

namespace optiperf {
namespace {

constexpr size_t kHeader = 16;  // user pointer = mimalloc block + 16; the original's int size header sits at user - 4

using MallocFn = void*(__cdecl*)(size_t size, size_t* allocated);
using FreeFn = size_t(__cdecl*)(void* p);
using ReallocFn = void*(__cdecl*)(void* p, size_t size, size_t* allocated);

FreeFn g_origFree = nullptr;
void* g_origMalloc = nullptr;
void* g_origRealloc = nullptr;
bool g_installed = false;
bool g_owners = false;  // owner tags in the blocks' first 8 bytes
volatile LONG g_foreignFrees = 0, g_foreignReallocs = 0, g_failed = 0;

// Entry points as compiled in build 6156; CryGetMemSize must read the int header at p-4 and add 4.
#ifdef _WIN64
constexpr const char* kMallocPattern = "48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 83 EC 40";
constexpr const char* kFreePattern = "40 53 48 83 EC 40 80 3D ?? ?? ?? ?? 00";
constexpr const char* kReallocPattern = "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40";
constexpr const char* kGetMemSizePattern = "48 63 41 FC 48 83 C0 04 C3";
#else
constexpr const char* kMallocPattern = "83 EC 18 53 55 8B 6C 24 24 33 DB";
constexpr const char* kFreePattern = "83 EC 18 80 3D ?? ?? ?? ?? 00";
constexpr const char* kReallocPattern = "83 EC 18 80 3D ?? ?? ?? ?? 00 55 56 57";
constexpr const char* kGetMemSizePattern = "8B 44 24 04 8B 40 FC 83 C0 04 C3";
#endif

#ifdef _WIN64
// The 64-bit engine truncates heap pointers to 32 bits in places (CrySystem's own init does), so every block it gets
// must live below 4 GB. The original allocator happened to stay there until a level was loaded; mimalloc instead gets
// address space reserved below 4 GB (committed on demand) and is not allowed to take memory from the OS elsewhere.
// Reserves up to `wantBytes` of free address space below 4 GB for mimalloc: the largest free regions first, each taken
// from its top end (the OS hands out memory bottom-up in this process, so others keep the low parts). Deterministic:
// one pass over the address map, explicit addresses. Returns the number of bytes reserved.
// The arenas reserved, for the diagnostics that watch the engine's memory (append-only: a refill may add one while a
// reader walks them).
constexpr int kMaxArenas = 64;
struct Arena {
    char* base;
    size_t size;
};
Arena g_arenas[kMaxArenas];
std::atomic<int> g_arenaCount{0};

size_t ReserveLowArenas(size_t wantBytes) {
    constexpr uintptr_t kLow = 0x10000000, kHigh = uintptr_t(1) << 32;
    constexpr size_t kAlign = size_t(32) << 20, kMinChunk = size_t(128) << 20;  // mimalloc segment alignment on x64
    struct Region {
        uintptr_t start;
        size_t size;
    };
    std::vector<Region> free;
    for (uintptr_t a = kLow; a < kHigh;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = (std::min)(base + mbi.RegionSize, kHigh);
        if (mbi.State == MEM_FREE) {
            const uintptr_t s = ((std::max)(base, a) + kAlign - 1) & ~(kAlign - 1), e = end & ~(kAlign - 1);
            if (e > s && e - s >= kMinChunk) free.push_back({s, e - s});
        }
        if (end <= a) break;
        a = end;
    }
    std::sort(free.begin(), free.end(), [](const Region& x, const Region& y) { return x.size > y.size; });
    size_t total = 0;
    for (const Region& r : free) {
        const size_t take = (std::min)(r.size, wantBytes - total) & ~(kAlign - 1);
        if (take < kMinChunk) break;
        void* p = VirtualAlloc(reinterpret_cast<void*>(r.start + r.size - take), take, MEM_RESERVE, PAGE_READWRITE);
        if (!p) continue;  // taken in the meantime
        if (!mi_manage_os_memory(p, take, /*is_committed*/ false, /*is_large*/ false, /*is_zero*/ true, -1)) {
            VirtualFree(p, 0, MEM_RELEASE);
            continue;
        }
        Log("allocator: %zu MB reserved below 4 GB at 0x%p", take >> 20, p);
        const int n = g_arenaCount.load(std::memory_order_relaxed);
        if (n < kMaxArenas) {
            g_arenas[n] = {static_cast<char*>(p), take};
            g_arenaCount.store(n + 1, std::memory_order_release);
        }
        total += take;
        if (total >= wantBytes) break;
    }
    return total;
}

SRWLOCK g_refillLock = SRWLOCK_INIT;
bool g_osFallback = false;

// Called when mimalloc ran out of low memory: reserve more below 4 GB, or as a last resort allow memory from anywhere.
bool Refill() {
    AcquireSRWLockExclusive(&g_refillLock);
    Log("allocator: low memory arena full, reserving more");
    bool ok = true;
    if (!g_osFallback && ReserveLowArenas(size_t(256) << 20) == 0) {
        Log("allocator: WARNING: no address space left below 4 GB; allowing memory above it (the 64-bit engine may crash)");
        mi_option_set(mi_option_disallow_os_alloc, 0);
        g_osFallback = true;
    } else if (g_osFallback) {
        ok = false;  // already unrestricted: a real out-of-memory
    }
    ReleaseSRWLockExclusive(&g_refillLock);
    return ok;
}
#endif

inline void* Finish(char* block, size_t size, size_t* allocated) {
    char* user = block + kHeader;
    const int header = int(size);
    std::memcpy(user - 4, &header, 4);
    if (allocated) *allocated = size + 4;
    return user;
}

inline size_t HeaderSize(const void* p) {
    int header;
    std::memcpy(&header, static_cast<const char*>(p) - 4, 4);
    return size_t(unsigned(header));
}

volatile LONG g_firstLogged = 0;

inline void Tag(char* block, void* site) {
    if (g_owners) std::memcpy(block, &site, sizeof site);
}

// --- pins (allocator.h) -----------------------------------------------------------------------------------------
// A pinned block carries a mark in the header's free bytes (user - 8; the owner tag is before them, the size after):
// nothing writes them at an allocation, so a block's leftover bytes may read as a mark, which at worst defers its free
// to the next EndPins. Frees and reallocs read the mark only while pins are active.
constexpr uint32_t kPinMark = 0x214E4950u, kDyingMark = 0x21594944u;  // "PIN!", "DIY!"
constexpr size_t kMaxPins = size_t(1) << 15, kMaxDeferred = size_t(1) << 14;
bool g_pinsEnabled = false;
volatile LONG g_pinsActive = 0;
void (*g_pinJoin)() = nullptr;
DWORD g_pinThread = 0;  // the thread that pins and joins (the main thread)
SRWLOCK g_pinLock = SRWLOCK_INIT;
char* g_pins[kMaxPins];  // the pinned blocks' user pointers
size_t g_pinCount = 0;
char* g_deferred[kMaxDeferred];  // the frees of pinned blocks, made at EndPins
size_t g_deferredCount = 0;
struct PinStats {
    uint64_t windows = 0, pinned = 0, notOurs = 0, full = 0, deferred = 0, deferredOther = 0, overflow = 0;
    uint64_t reallocJoins = 0, reallocMoves = 0, dying = 0;
    size_t mostPins = 0, mostDeferred = 0;
} g_ps;

inline uint32_t PinWord(const void* user) {
    uint32_t m;
    std::memcpy(&m, static_cast<const char*>(user) - 8, 4);
    return m;
}
inline void SetPinWord(void* user, uint32_t m) { std::memcpy(static_cast<char*>(user) - 8, &m, 4); }

// the user pointer of the allocator's block that p points into (and its usable bytes), else null
char* UserOf(const void* p, size_t* usable = nullptr) {
    if (!p || !mi_is_in_heap_region(p)) return nullptr;
    char* block = nullptr;
    size_t size = 0;
    if (!Allocator_BlockOf(p, &block, &size) || size <= kHeader) return nullptr;
    char* const user = block + kHeader;
    if (static_cast<const char*>(p) < user) return nullptr;
    if (usable) *usable = size - kHeader;
    return user;
}
// the last block pinned (the render objects the parts pin share the renderer's pool chunks; a pinned block can't be
// reused before EndPins, its free being kept)
char* g_lastPinUser = nullptr;
size_t g_lastPinSize = 0;

// a pinned block's free: kept until EndPins, marked dying (false: not pinned now, or no room)
bool Defer(void* p) {
    AcquireSRWLockExclusive(&g_pinLock);
    bool ok = false;
    if (g_pinsActive && PinWord(p) == kPinMark) {
        if (g_deferredCount < kMaxDeferred) {
            SetPinWord(p, kDyingMark);
            g_deferred[g_deferredCount++] = static_cast<char*>(p);
            g_ps.mostDeferred = (std::max)(g_ps.mostDeferred, g_deferredCount);
            ++(GetCurrentThreadId() == g_pinThread ? g_ps.deferred : g_ps.deferredOther);
            ok = true;
        } else {
            ++g_ps.overflow;  // (freed as asked: a step after the flush may then write into it)
        }
    }
    ReleaseSRWLockExclusive(&g_pinLock);
    return ok;
}

void* MallocFor(size_t size, size_t* allocated, void* site) {
    auto* block = static_cast<char*>(mi_malloc(size + kHeader));
#ifdef _WIN64
    if (!block && Refill()) block = static_cast<char*>(mi_malloc(size + kHeader));
#endif
    if (!block) {
        InterlockedIncrement(&g_failed);
        if (allocated) *allocated = 0;
        return nullptr;
    }
    if (!g_firstLogged && InterlockedExchange(&g_firstLogged, 1) == 0)
        Log("allocator: first block at 0x%p (%zu bytes)", block + kHeader, size);
    Tag(block, site);
    return Finish(block, size, allocated);
}

void* __cdecl MyMalloc(size_t size, size_t* allocated) { return MallocFor(size, allocated, _ReturnAddress()); }

size_t __cdecl MyFree(void* p) {
    if (!p) return 0;
    if (!mi_is_in_heap_region(p)) {  // allocated before the hook was installed
        InterlockedIncrement(&g_foreignFrees);
        return g_origFree(p);
    }
    const size_t size = HeaderSize(p);
    if (g_pinsActive && PinWord(p) == kPinMark && Defer(p)) return size + 4;
    mi_free(static_cast<char*>(p) - kHeader);
    return size + 4;
}

void* __cdecl MyRealloc(void* p, size_t size, size_t* allocated) {
    if (!p) return MyMalloc(size, allocated);
    if (!mi_is_in_heap_region(p)) {  // move a pre-hook block into mimalloc, like the original's alloc-copy-free
        InterlockedIncrement(&g_foreignReallocs);
        void* moved = MallocFor(size, allocated, _ReturnAddress());
        if (!moved) return nullptr;
        std::memcpy(moved, p, (std::min)(size, HeaderSize(p)));
        g_origFree(p);
        return moved;
    }
    if (g_pinsActive && PinWord(p) == kPinMark) {
        // a pinned block: on the main thread the steps after the flush write first (the join), as vanilla's order has
        // them; else (another thread, or no flush to join) it moves, and the old block is kept until EndPins
        if (GetCurrentThreadId() == g_pinThread && g_pinJoin) {
            ++g_ps.reallocJoins;
            g_pinJoin();
        }
        if (g_pinsActive && PinWord(p) == kPinMark) {
            const size_t old = HeaderSize(p);
            void* const moved = MallocFor(size, allocated, _ReturnAddress());
            if (!moved) return nullptr;
            std::memcpy(moved, p, (std::min)(size, old));
            if (!Defer(p)) mi_free(static_cast<char*>(p) - kHeader);
            ++g_ps.reallocMoves;
            return moved;
        }
    }
    auto* block = static_cast<char*>(mi_realloc(static_cast<char*>(p) - kHeader, size + kHeader));
#ifdef _WIN64
    if (!block && Refill()) block = static_cast<char*>(mi_realloc(static_cast<char*>(p) - kHeader, size + kHeader));
#endif
    if (!block) {
        InterlockedIncrement(&g_failed);
        if (allocated) *allocated = 0;
        return nullptr;
    }
    Tag(block, _ReturnAddress());
    return Finish(block, size, allocated);
}

#ifdef _WIN64
// The owner tags read mimalloc's page layout (allocator_owners.cpp): checked first on blocks of the allocator's own,
// small, medium, large and huge (in a segment of their own, past 32 MiB), at their start, inside and at their end.
bool OwnersWork() {
    const size_t sizes[] = {24, 3000, 300000, size_t(5) << 20, size_t(20) << 20, size_t(40) << 20};
    bool ok = true;
    for (size_t n : sizes) {
        auto* b = static_cast<char*>(mi_malloc(n));
        if (!b) return false;
        for (size_t at : {size_t(0), n / 3, n - 1}) {
            char* block = nullptr;
            size_t size = 0;
            ok = ok && Allocator_BlockOf(b + at, &block, &size) && block == b && size >= n;
        }
        mi_free(b);
    }
    return ok;
}
#endif

bool Looks(char* fn, const char* pattern) { return fn && MatchesAt(fn, ParsePattern(pattern)); }

// Must run before the first mimalloc allocation. x64: memory only from address space reserved below 4 GB. When that is
// full, mimalloc must neither reserve a new arena elsewhere (arena_reserve = 0) nor fall back to the OS
// (disallow_os_alloc); the allocation then fails and Refill() reserves more below 4 GB.
bool ConfigureMimalloc(size_t lowMemoryMb) {
#ifdef _WIN64
    mi_option_set(mi_option_arena_reserve, 0);
    mi_option_set(mi_option_disallow_os_alloc, 1);
    return ReserveLowArenas(lowMemoryMb << 20) != 0;
#else
    (void)lowMemoryMb;
    mi_option_set(mi_option_arena_reserve, 64 * 1024);  // KiB; reserve address space in smaller steps in a 32-bit process
    return true;
#endif
}

}  // namespace

void Allocator_OnModuleLoaded(const wchar_t* baseName, char* base, size_t /*size*/) {
    if (_wcsicmp(baseName, L"CrySystem.dll") != 0 || g_installed) return;
    const std::wstring mode = ConfigString(L"Allocator", L"Mode", L"OPTIPERF_ALLOCATOR", L"mimalloc");
    if (_wcsicmp(mode.c_str(), L"off") == 0 || mode == L"0") {
        Log("CrySystem.dll: allocator replacement disabled ([Allocator] Mode=off)");
        return;
    }
    auto* cryMalloc = static_cast<char*>(FindExport(base, "CryMalloc"));
    auto* cryFree = static_cast<char*>(FindExport(base, "CryFree"));
    auto* cryRealloc = static_cast<char*>(FindExport(base, "CryRealloc"));
    auto* cryGetMemSize = static_cast<char*>(FindExport(base, "CryGetMemSize"));
    const char* bad = !Looks(cryMalloc, kMallocPattern)       ? "CryMalloc"
                      : !Looks(cryFree, kFreePattern)         ? "CryFree"
                      : !Looks(cryRealloc, kReallocPattern)   ? "CryRealloc"
                      : !Looks(cryGetMemSize, kGetMemSizePattern) ? "CryGetMemSize"
                                                              : nullptr;
    if (bad) {
        Log("CrySystem.dll: allocator replacement not applied: %s does not look as expected", bad);
        return;
    }
    const size_t lowMb = size_t(_wtoi(ConfigString(L"Allocator", L"LowMemoryMB", L"OPTIPERF_ALLOCATOR_LOW_MB", L"1536").c_str()));
    if (!ConfigureMimalloc(lowMb)) {
        Log("CrySystem.dll: allocator replacement not applied: could not reserve address space below 4 GB");
        return;
    }
#ifdef _WIN64
    const std::wstring owners = ConfigString(L"Allocator", L"Owners", L"OPTIPERF_ALLOCATOR_OWNERS", L"off");
    if (_wcsicmp(owners.c_str(), L"on") == 0 || owners == L"1") {
        g_owners = OwnersWork();
        Log("CrySystem.dll: allocator owner tags: %s", g_owners ? "on (each block keeps the code that allocated it)"
                                                                : "off: mimalloc's page layout is not the expected one");
    }
#endif
    const HookSpec hooks[] = {
        {"CryFree", cryFree, reinterpret_cast<void*>(&MyFree), reinterpret_cast<void**>(&g_origFree)},
        {"CryRealloc", cryRealloc, reinterpret_cast<void*>(&MyRealloc), &g_origRealloc},
        {"CryMalloc", cryMalloc, reinterpret_cast<void*>(&MyMalloc), &g_origMalloc},
    };
    if (!InstallHooks(hooks, std::size(hooks))) {
        Log("CrySystem.dll: allocator replacement not applied: hooking failed");
        return;
    }
    g_installed = true;
    Log("CrySystem.dll: allocator replaced by mimalloc %d (CryMalloc/CryFree/CryRealloc hooked)", mi_version());
}

bool Allocator_TracksOwners() { return g_owners; }

void Allocator_ForEachArena(void (*fn)(char* base, size_t size, void* ctx), void* ctx) {
#ifdef _WIN64
    const int n = g_arenaCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) fn(g_arenas[i].base, g_arenas[i].size, ctx);
#else
    (void)fn;
    (void)ctx;
#endif
}

bool Allocator_OwnerOf(const void* p, const void** site, const char** start, size_t* size) {
#ifdef _WIN64
    char* block = nullptr;
    size_t bytes = 0;
    if (!g_owners || !p || !mi_is_in_heap_region(p) || !Allocator_BlockOf(p, &block, &bytes) || bytes <= kHeader)
        return false;
    std::memcpy(site, block, sizeof *site);
    *start = block + kHeader;
    *size = bytes - kHeader;
    return true;
#else
    (void)p;
    (void)site;
    (void)start;
    (void)size;
    return false;
#endif
}

bool Allocator_TestSetup(size_t lowMemoryMb, bool owners) {
    if (!ConfigureMimalloc(lowMemoryMb)) return false;
    g_origFree = [](void*) -> size_t { return 0; };  // tests never pass foreign pointers
    g_installed = true;
#ifdef _WIN64
    g_owners = owners && OwnersWork();
#else
    (void)owners;
#endif
    return true;
}
void* Allocator_TestMalloc(size_t size, size_t* allocated) { return MyMalloc(size, allocated); }
size_t Allocator_TestFree(void* p) { return MyFree(p); }
void* Allocator_TestRealloc(void* p, size_t size, size_t* allocated) { return MyRealloc(p, size, allocated); }

void Allocator_Report(const char* when) {
    if (!g_installed) return;
    size_t elapsed, user, sys, rss, peakRss, commit, peakCommit, faults;
    mi_process_info(&elapsed, &user, &sys, &rss, &peakRss, &commit, &peakCommit, &faults);
    Log("allocator [%s]: process commit %.0f MB (peak %.0f), working set %.0f MB (peak %.0f) | pre-hook blocks freed/moved: "
        "%ld/%ld | failed allocations: %ld",
        when, commit / 1048576.0, peakCommit / 1048576.0, rss / 1048576.0, peakRss / 1048576.0, g_foreignFrees,
        g_foreignReallocs, g_failed);
    Allocator_PinReport(when);
}

void Allocator_EnablePins(void (*join)()) {
#ifdef _WIN64
    if (g_pinsEnabled) return;
    if (!g_installed) {
        Log("allocator: pins off (the allocator isn't installed): the steps after a flush beside the main thread aren't "
            "kept from freed objects");
        return;
    }
    // (the pins find a block by mimalloc's page layout: checked on blocks of the allocator's own, at their start and
    // inside them)
    bool ok = true;
    for (size_t n : {size_t(24), size_t(3000), size_t(300000), size_t(5) << 20}) {
        auto* user = static_cast<char*>(MallocFor(n, nullptr, nullptr));
        if (!user) return;
        ok = ok && UserOf(user) == user && UserOf(user + n / 2) == user && UserOf(user + n - 1) == user;
        mi_free(user - kHeader);
    }
    if (!ok) {
        Log("allocator: pins off (the allocator's blocks can't be found from inside them): the steps after a flush "
            "beside the main thread aren't kept from freed objects");
        return;
    }
    g_pinJoin = join;
    g_pinsEnabled = true;
    Log("allocator: pins on (the blocks the steps after a flush write into are kept until it is joined)");
#else
    (void)join;
#endif
}

void Allocator_BeginPins() {
    if (!g_pinsEnabled) return;
    if (g_pinsActive) Allocator_EndPins();  // (a window that wasn't ended)
    g_pinThread = GetCurrentThreadId();
    g_lastPinUser = nullptr;
    ++g_ps.windows;
    InterlockedExchange(&g_pinsActive, 1);
}

bool Allocator_Pin(const void* p) {
    if (!g_pinsActive) return false;
    if (g_lastPinUser && static_cast<const char*>(p) >= g_lastPinUser &&
        static_cast<const char*>(p) < g_lastPinUser + g_lastPinSize)
        return true;
    size_t usable = 0;
    char* const user = UserOf(p, &usable);
    if (!user) {
        ++g_ps.notOurs;
        return false;
    }
    bool ok = true;
    AcquireSRWLockExclusive(&g_pinLock);
    const uint32_t m = PinWord(user);
    if (m != kPinMark && m != kDyingMark) {
        if (g_pinCount < kMaxPins) {
            SetPinWord(user, kPinMark);
            g_pins[g_pinCount++] = user;
            g_ps.mostPins = (std::max)(g_ps.mostPins, g_pinCount);
            ++g_ps.pinned;
        } else {
            ++g_ps.full;
            ok = false;
        }
    }
    if (ok) {
        g_lastPinUser = user;
        g_lastPinSize = usable;
    }
    ReleaseSRWLockExclusive(&g_pinLock);
    return ok;
}

bool Allocator_Dying(const void* p) {
    if (!g_pinsActive) return false;
    char* const user = UserOf(p);
    if (!user || PinWord(user) != kDyingMark) return false;
    ++g_ps.dying;
    return true;
}

void Allocator_EndPins() {
    if (!g_pinsActive) return;
    AcquireSRWLockExclusive(&g_pinLock);
    InterlockedExchange(&g_pinsActive, 0);
    g_lastPinUser = nullptr;
    for (size_t i = 0; i < g_pinCount; ++i)
        if (PinWord(g_pins[i]) == kPinMark) SetPinWord(g_pins[i], 0);
    g_pinCount = 0;
    for (size_t i = 0; i < g_deferredCount; ++i) {
        SetPinWord(g_deferred[i], 0);
        mi_free(g_deferred[i] - kHeader);
    }
    g_deferredCount = 0;
    ReleaseSRWLockExclusive(&g_pinLock);
}

void Allocator_PinReport(const char* when) {
    if (!g_pinsEnabled) return;
    const double w = double(g_ps.windows ? g_ps.windows : 1);
    Log("allocator pins [%s]: windows %llu, blocks pinned %.1f per window (most %zu; not the allocator's %llu, no room "
        "%llu) | frees deferred to the join: %llu on the main thread, %llu on others (most at once %zu; no room %llu) | "
        "reallocs of a pinned block: joined first %llu, moved %llu | dying blocks the steps skipped %llu",
        when, g_ps.windows, double(g_ps.pinned) / w, g_ps.mostPins, g_ps.notOurs, g_ps.full, g_ps.deferred,
        g_ps.deferredOther, g_ps.mostDeferred, g_ps.overflow, g_ps.reallocJoins, g_ps.reallocMoves, g_ps.dying);
}

}  // namespace optiperf
