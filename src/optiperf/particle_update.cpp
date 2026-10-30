// The particle emitters' update on several threads: see particle_update.h.
//
// The batch: at the manager's first update call (the update loop's own call, known by its return address; the loop's
// code is checked by pattern), the manager's emitter list (+0x68: an array of emitter pointers, its count at -4) is
// copied and cut into one range per thread (the main thread's first): each thread updates its own range from its
// front, four emitters a claim, then takes from the other ranges' backs (the same emitters stay on the same thread
// from frame to frame, in its caches). A range is one word, its end (high half) and next index (low half), claimed by
// a compare-exchange; the next batch replaces each in one store, so a late claim can't reach into it. The main thread
// then waits for the done count. The loop's update calls find their emitter next in the copy (a few ahead at most)
// and return at once.
//
// The serial stubs, 16 bytes each in memory of our own (their targets on a read-write page after them):
//   mov eax, id
//   jmp [rip+check]   ParticleSerialCheck64 (particle_update_x64.asm): the calling thread's flag (t_particleSerial,
//                     optiperf's static TLS: the game uses up the TEB's 64 TlsAlloc slots), then straight on to the
//                     target, or the call under the lock (ParticleSerial64) on a batch thread
// A vtable entry is pointed at its stub only while a batch runs (one aligned store each way; the entries' pages are
// made writable once, at the second flush): the game's own calls to these classes the rest of the frame, the physics'
// and the sound system's many, go straight on. The stubs aren't registered with the tracer's see-through (nothing
// measures these classes' vtables).
// SVisEnviron::Update is detoured (MinHook) to a hook that takes the lock only when the emitter's environment isn't
// computed yet (GetVisAreaFromPos, IntersectsVisAreas: once an emitter). Each frame an emitter inside a vis area asks
// CVisArea::FindVisArea whether the camera's area reaches it: that search keeps its visited areas in a function-static
// list it hands the recursive search as an argument, so on a batch thread FindVisArea's detour hands it a list of the
// thread's own instead (the same search, nothing shared).
// The static meshes' and the characters' Release isn't made on a batch thread: emitters share those objects, their
// counts aren't atomic, and the last Release deletes the object, with renderer calls that off the main thread would
// pass the gate beside the flush. It goes on a list, under the lock, and the main thread makes them after the batch.
// While the workers run, the vtable counters count nothing (g_vtableCountingOff, vtable_counter.h): the timer's getters
// go through the system snapshot's counting copy beside the flush, and every thread of the batch calls them.

#include "particle_update.h"

#include <windows.h>
#include <intrin.h>
#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "calltrace.h"
#include "config.h"
#include "flush_thread.h"
#include "hooks.h"
#include "log.h"
#include "render_gate.h"
#include "scan.h"
#include "vtable_counter.h"

#ifdef _WIN64
extern "C" {
void** g_particleSerialTargets = nullptr;              // (particle_update_x64.asm reads them)
__declspec(thread) unsigned char t_particleSerial = 0;  // a batch thread's: its serial calls take the lock
void ParticleSerial64();
void ParticleSerialCheck64();
void ParticleSerialEnter(unsigned id);
void ParticleSerialLeave();
}
#endif

namespace optiperf {
namespace {

enum class PMode { Off, On, Serial, Time };

PMode ReadMode() {
    const std::wstring v = ConfigString(L"ParticleUpdate", L"Mode", L"OPTIPERF_PARTICLE_UPDATE", L"off");
    if (_wcsicmp(v.c_str(), L"on") == 0 || v == L"1") return PMode::On;
    if (_wcsicmp(v.c_str(), L"serial") == 0) return PMode::Serial;
    if (_wcsicmp(v.c_str(), L"time") == 0) return PMode::Time;
    return PMode::Off;
}

#ifdef _WIN64
const char* ModeName(PMode m) {
    return m == PMode::On ? "on" : m == PMode::Serial ? "serial" : m == PMode::Time ? "time" : "off";
}

// --- patterns (x64 Cry3DEngine.dll, build 6156; research/profiling/re/mkpattern.py) --------------------------------
// CPartManager::Update(this), from its start (its profiler label's one-time setup); at +0x29 the load of the 3D
// engine's gEnv pointer (mov rax, [rip+X])
constexpr const char kPartManagerUpdate[] =
    "40 53 41 55 48 83 EC 78 8B 05 ?? ?? ?? ?? 4C 8B E9 48 8D 1D ?? ?? ?? ?? A8 01 0F 85 ?? ?? ?? ?? 83 C8 01 89 05 "
    "?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8B 08 48 8D 05 ?? ?? ?? ??";
constexpr size_t kGEnvLoad = 0x29;
constexpr size_t kPartUpdateBytes = 0x6A5;  // (the function's length: where its update loop is looked for)
// its update loop's start: the list (this, r13, +0x68; its count at -4), each emitter's Update (vtable +0x60), then
// IsAlive (+0x18)
constexpr const char kUpdateLoop[] =
    "49 8B 45 68 33 F6 39 70 FC 0F 86 ?? ?? ?? ?? 33 ED 4C 8D 3D ?? ?? ?? ?? 66 66 90 66 66 90 66 66 90 49 8B 45 68 "
    "48 8B 1C 28 48 8B CB 48 8B 03 FF 50 60 4C 8B 1B 48 8B CB 41 FF 53 18 84 C0";
constexpr size_t kUpdateReturn = 0x32;  // the update call's return address, from the loop's start
constexpr size_t kListAt = 0x68;
// CVisArea::FindVisArea(this, another, maxRecursion, skipDisabledPortals), from its start (its static list's one-time
// setup); its call of the recursive search (+0x1279D0), the list the fifth argument; the search's start, the list's
// count (+8) against its capacity (+0xC)
constexpr const char kFindVisArea[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 48 83 EC 30 8B 05 ?? ?? ?? ?? 33 FF 41 0F B6 F1 "
    "A8 01 49 63 D8 48 8B EA 4C 8B E1 75 28 83 C8 01 48 8D 0D ?? ?? ?? ?? 48 89 3D ?? ?? ?? ?? 89 3D ?? ?? ?? ?? 89 3D "
    "?? ?? ?? ?? 89 05 ?? ?? ?? ?? E8 ?? ?? ?? ??";
constexpr const char kFindVisAreaCall[] =
    "48 8D 05 ?? ?? ?? ?? 44 0F B6 CE 44 8B C3 48 8B D5 49 8B CC 48 89 44 24 20 E8 ?? ?? ?? ??";
constexpr size_t kFindVisAreaBytes = 0xFE, kFindVisAreaCallAt = 0x19;  // (the call's offset in that pattern)
constexpr const char kVisSearch[] =
    "44 88 4C 24 20 44 89 44 24 18 48 89 54 24 10 53 57 48 83 EC 38 48 8B 5C 24 70 4C 8B D2 48 8B F9 8B 43 08 3B 43 0C "
    "7C 1E";
// SVisEnviron::Update(this, ...), from its start
constexpr const char kVisEnvironUpdate[] =
    "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 40 48 89 6C 24 50 33 ED 49 8B D8 40 38 29 48 8B F2 48 8B F9 0F 85 ?? "
    "?? ?? ?? 8B 05 ?? ?? ?? ?? 4C 89 64 24 58 4C 8D 25 ?? ?? ?? ?? A8 01";
constexpr unsigned kUpdateSlot = 12;     // CParticleEmitter::Update, as the loop calls it
constexpr unsigned kGetCameraSlot = 44;  // IRenderer::GetCamera (Mod SDK)
constexpr size_t kProfilerOn = 0xD3;     // gEnv's bProfilerEnabled (CPartManager::Update reads it there)

constexpr uint32_t kMinBatch = 8;
constexpr uint32_t kChunk = 4;  // emitters a claim
constexpr unsigned kAhead = 8;  // how far ahead in the copy the loop's call is looked for
constexpr int kMaxWorkers = 8;
constexpr unsigned kMaxSerial = 256;
constexpr size_t kStub = 16;
constexpr int kMaxSlots = 160;  // a class's vtable is measured up to this
constexpr int kAll = -1;
constexpr SIZE_T kWorkerStack = SIZE_T(1) << 20;

// The classes whose methods a batch thread calls under the lock: every slot, or the listed ones; release: the slot of
// a Release kept for the main thread (ReleaseHook), the index into State's releases, or -1.
constexpr int kReleases = 2, kNoRelease = -1;
struct SerialClass {
    const wchar_t* module;
    const char* mangled;
    const char* name;
    int n;
    int slots[8];
    int releaseSlot, release;
};
const SerialClass kClasses[] = {
    {L"CrySoundSystem.dll", ".?AVCSoundSystem@@", "CSoundSystem", 1, {kAll}, kNoRelease, kNoRelease},
    {L"CrySoundSystem.dll", ".?AVCSound@@", "CSound", 1, {kAll}, kNoRelease, kNoRelease},
    // the creation (both forms), placeholders, removal, ids, box queries and ray tests
    {L"CryPhysics.dll", ".?AVCPhysicalWorld@@", "CPhysicalWorld", 7, {12, 13, 14, 15, 16, 28, 29}, kNoRelease, kNoRelease},
    // SetParams, Action, AddGeometry, RemoveGeometry (not GetParams, GetStatus: the readers; nor AddRef, Release: an
    // atomic count off the physics thread, and the physics' own calls are many)
    {L"CryPhysics.dll", ".?AVCParticleEntity@@", "CParticleEntity", 4, {3, 6, 7, 8}, kNoRelease, kNoRelease},
    {L"CryPhysics.dll", ".?AVCRigidEntity@@", "CRigidEntity", 4, {3, 6, 7, 8}, kNoRelease, kNoRelease},
    {L"CryPhysics.dll", ".?AVCPhysicalEntity@@", "CPhysicalEntity", 4, {3, 6, 7, 8}, kNoRelease, kNoRelease},
    // AddRef, ComputeExtent, GetRandomPos; Release kept for the main thread (the counts aren't atomic, emitters share them)
    {L"Cry3DEngine.dll", ".?AUCStatObj@@", "CStatObj", 3, {0, 25, 26}, 1, 0},
    {L"CryAnimation.dll", ".?AVCCharInstance@@", "CCharInstance", 3, {0, 20, 21}, 1, 1},
    {L"CrySystem.dll", ".?AVCFrameProfileSystem@@", "CFrameProfileSystem", 1, {1}, kNoRelease, kNoRelease},  // AddFrameProfiler
};

using UpdateFn = void (*)(void*);
using CameraFn = void* (*)(void*);

struct Patch {
    void** entry;
    void* stub;  // (or a hook of ours)
    void* original;
};

using ReleaseFn = void (*)(void*);
struct KeptRelease {
    ReleaseFn fn;
    void* object;
};
using VisUpdateFn = void (*)(char* self, void* position, float* box);
struct VisList {  // the vis areas' search's list (a PodArray of areas)
    void** data;
    int count, capacity;
};
using FindVisAreaFn = char (*)(void* self, void* another, int maxRecursion, char skipDisabledPortals);
using VisSearchFn = char (*)(void* area, void* another, int maxRecursion, char skipDisabledPortals, VisList* list);

#pragma warning(push)
#pragma warning(disable : 4324)  // (padded to their cache lines: that is what alignas is for here)
struct alignas(64) Worker {
    HANDLE thread = nullptr, go = nullptr;
    uint64_t batches = 0, updates = 0, stolen = 0, cycles = 0;  // (its own thread's)
};

struct alignas(64) Range {
    std::atomic<uint64_t> word{0};  // its end (high half), its next index (low half)
};

struct Stats {
    uint64_t updates = 0, batches = 0, emitters = 0, mainEmitters = 0;
    uint64_t few = 0, profiler = 0, notFirst = 0, cameraLost = 0;
    uint64_t skipped = 0, ahead = 0, newCalls = 0, unbatchedCalls = 0;
    uint64_t wallCycles = 0, mainCycles = 0, waitCycles = 0, longest = 0, over2ms = 0, mainStolen = 0;
    uint64_t managerCycles = 0;  // the manager's whole update (time: the game's own)
    uint64_t keptReleases = 0;   // Releases the main thread made after a batch
};

struct State {
    PMode mode = PMode::Off;
    bool installed = false, setupTried = false, ready = false;
    int flushes = 0;
    DWORD mainThread = 0;
    char* e3d = nullptr;
    UpdateFn update = nullptr;        // CParticleEmitter::Update
    UpdateFn partUpdate = nullptr;    // CPartManager::Update's trampoline
    VisUpdateFn visUpdate = nullptr;  // SVisEnviron::Update's
    FindVisAreaFn findVisArea = nullptr;  // CVisArea::FindVisArea's
    VisSearchFn visSearch = nullptr;      // the recursive search it calls
    const void* loopReturn = nullptr;
    char** genv = nullptr;  // the 3D engine's gEnv pointer
    // the manager's update (main thread)
    char* manager = nullptr;
    bool inUpdate = false, batched = false;
    uint32_t cursor = 0, count = 0;
    std::vector<void*> list;  // the batch's copy of the emitter list
    // the batch, for the workers: a range per thread (the main thread's first)
    Range ranges[kMaxWorkers + 1];
    int rangeCount = 1;
    alignas(64) std::atomic<uint32_t> done{0};
    void* const* items = nullptr;
    unsigned mxcsr = 0;
    // the renderer's camera, with the gate (the render thread)
    void* renderer = nullptr;
    int cameraIndex = -1;
    CameraFn cameraNext = nullptr;
    void* camera = nullptr;  // the main thread's answer at the batch's start
    int workerCount = 0;
    Worker workers[kMaxWorkers];
    // the serial calls
    CRITICAL_SECTION lock;
    std::vector<Patch> patches;  // the vtable entries pointed at the stubs while a batch runs
    ReleaseFn releases[kReleases] = {};
    unsigned releaseIds[kReleases] = {};
    std::vector<KeptRelease> kept;  // a batch's Releases (under the lock), for the main thread after it
    uint8_t* stubs = nullptr;
    unsigned stubCount = 0;
    char labels[kMaxSerial][40] = {};
    uint64_t serialCalls[kMaxSerial] = {};  // (under the lock)
    uint64_t contended = 0, lockWaitCycles = 0;
    Stats st;
};
#pragma warning(pop)
State g;
thread_local bool t_worker = false;
thread_local VisList t_visList = {};  // a batch thread's list for the vis areas' search (grown by the search itself)

double Ms(uint64_t cycles) { return double(cycles) / 3.0e6; }

char* Unique(char* base, const char* text) {
    const auto hits = FindInCode(base, ParsePattern(text), 2);
    return hits.size() == 1 ? hits[0] : nullptr;
}

// --- the batch -------------------------------------------------------------------------------------------------------
// Up to kChunk emitters of range r, from its front (its own thread) or its back (another); false once it is empty.
bool Claim(int r, bool front, uint32_t* first, uint32_t* count) {
    std::atomic<uint64_t>& w = g.ranges[r].word;
    uint64_t c = w.load(std::memory_order_acquire);
    for (;;) {
        const uint32_t next = uint32_t(c), end = uint32_t(c >> 32);
        if (next >= end) return false;
        const uint32_t k = (std::min)(kChunk, end - next);
        const uint64_t to = front ? (uint64_t(end) << 32) | (next + k) : (uint64_t(end - k) << 32) | next;
        if (w.compare_exchange_weak(c, to, std::memory_order_acq_rel, std::memory_order_acquire)) {
            *first = front ? next : end - k;
            *count = k;
            return true;
        }
    }
}

void Updates(uint32_t first, uint32_t count) {
    for (uint32_t i = first; i < first + count; ++i) g.update(g.items[i]);
    g.done.fetch_add(count, std::memory_order_release);
}

// Thread `own`'s part: its range, then the others' backs; how many it updated (those of other ranges added to stolen).
uint32_t Work(int own, uint64_t* stolen) {
    uint32_t mine = 0, first = 0, k = 0;
    while (Claim(own, true, &first, &k)) {
        Updates(first, k);
        mine += k;
    }
    const int n = g.rangeCount;
    for (int d = 1; d < n; ++d)
        while (Claim((own + d) % n, false, &first, &k)) {
            Updates(first, k);
            mine += k;
            *stolen += k;
        }
    return mine;
}

DWORD WINAPI WorkerMain(void* p) {
    Worker& w = *static_cast<Worker*>(p);
    const int own = int(&w - g.workers) + 1;  // (its range: the main thread has the first)
    t_worker = true;
    t_particleSerial = 1;  // (a worker makes nothing but batches)
    for (;;) {
        if (WaitForSingleObject(w.go, INFINITE) != WAIT_OBJECT_0) return 1;
        _mm_setcsr(g.mxcsr);
        const uint64_t t0 = __rdtsc();
        const uint32_t n = Work(own, &w.stolen);
        w.cycles += __rdtsc() - t0;
        w.updates += n;
        ++w.batches;
    }
}

// the main thread's camera, through the vtable as the emitters ask for it (the gate and the render queue answer it)
void* CameraOnMain() {
    void** vt = *static_cast<void***>(g.renderer);
    return reinterpret_cast<CameraFn>(vt[kGetCameraSlot])(g.renderer);
}

// GetCamera through the gate: a worker gets the main thread's answer at the batch's start
void* CameraHook(void* r) {
    if (t_worker) return g.camera;
    return g.cameraNext(r);
}

bool ProfilerOn() {
    const char* env = *g.genv;
    return env && env[kProfilerOn] != 0;
}

// the vtable entries of the serial calls at their stubs (for a batch), or back at their functions
void Swap(bool in) {
    for (const Patch& p : g.patches) *reinterpret_cast<void* volatile*>(p.entry) = in ? p.stub : p.original;
}

void RunBatch(uint32_t n) {
    g.items = g.list.data();
    Swap(true);
    const bool workers = g.mode == PMode::On && g.workerCount > 0;
    if (workers) {
        g.mxcsr = _mm_getcsr();
        if (g.cameraIndex >= 0) g.camera = CameraOnMain();
        g_vtableCountingOff = 1;
    }
    const int t = workers ? g.workerCount + 1 : 1;
    g.rangeCount = t;
    g.done.store(0, std::memory_order_relaxed);
    for (int r = 0; r < t; ++r) {
        const uint32_t from = uint32_t(uint64_t(n) * uint64_t(r) / uint64_t(t));
        const uint32_t to = uint32_t(uint64_t(n) * uint64_t(r + 1) / uint64_t(t));
        g.ranges[r].word.store((uint64_t(to) << 32) | from, std::memory_order_release);
    }
    if (workers)
        for (int i = 0; i < g.workerCount; ++i) SetEvent(g.workers[i].go);
    const uint64_t t0 = __rdtsc();
    t_particleSerial = 1;
    const uint32_t mine = Work(0, &g.st.mainStolen);
    t_particleSerial = 0;
    const uint64_t t1 = __rdtsc();
    while (g.done.load(std::memory_order_acquire) < n) _mm_pause();
    const uint64_t t2 = __rdtsc();
    g_vtableCountingOff = 0;
    Swap(false);
    for (const KeptRelease& k : g.kept) k.fn(k.object);
    g.st.keptReleases += g.kept.size();
    g.kept.clear();
    Stats& s = g.st;
    ++s.batches;
    s.emitters += n;
    s.mainEmitters += mine;
    s.mainCycles += t1 - t0;
    s.waitCycles += t2 - t1;
    s.wallCycles += t2 - t0;
    s.longest = (std::max)(s.longest, t2 - t0);
    if (t2 - t0 > uint64_t(6.0e6)) ++s.over2ms;
}

// At the loop's first update call (self: the list's first emitter): the batch, unless it isn't to be made this time.
bool StartBatch(void* self) {
    Stats& s = g.st;
    void** const arr = *reinterpret_cast<void***>(g.manager + kListAt);
    const uint32_t n = arr ? reinterpret_cast<const uint32_t*>(arr)[-1] : 0;
    if (n == 0 || arr[0] != self) {
        ++s.notFirst;
        return false;
    }
    if (n < kMinBatch) {
        ++s.few;
        return false;
    }
    if (ProfilerOn()) {
        ++s.profiler;
        return false;
    }
    if (g.cameraIndex >= 0 && RenderGate_Target(unsigned(g.cameraIndex)) != reinterpret_cast<void*>(&CameraHook)) {
        ++s.cameraLost;  // (another part redirected GetCamera since: the workers' answer would be missed)
        return false;
    }
    g.list.assign(arr, arr + n);
    g.count = n;
    RunBatch(n);
    return true;
}

// --- the hooks -------------------------------------------------------------------------------------------------------
// SVisEnviron::Update(self, position, box): on a batch thread under the lock when the environment isn't computed yet
// (self[0] clear: GetVisAreaFromPos, IntersectsVisAreas); else a few compares, the emitter's own field, and for one in
// a vis area CVisArea::FindVisArea (below)
void VisEnvironHook(char* self, void* position, float* box) {
    if (t_particleSerial && self[0] == 0) {
        ParticleSerialEnter(0);
        g.visUpdate(self, position, box);
        ParticleSerialLeave();
        return;
    }
    g.visUpdate(self, position, box);
}

// A shared object's Release on a batch thread: kept for the main thread, right after the batch
template <int I>
void ReleaseHook(void* object) {
    if (!t_particleSerial) return g.releases[I](object);
    ParticleSerialEnter(g.releaseIds[I]);
    g.kept.push_back({g.releases[I], object});
    ParticleSerialLeave();
}

// CVisArea::FindVisArea: on a batch thread, the search with the thread's own list instead of the function's static one
char FindVisAreaHook(void* self, void* another, int maxRecursion, char skipDisabledPortals) {
    if (!t_particleSerial) return g.findVisArea(self, another, maxRecursion, skipDisabledPortals);
    t_visList.count = 0;
    return g.visSearch(self, another, maxRecursion, skipDisabledPortals, &t_visList);
}

void PartUpdateHook(void* self) {
    if (GetCurrentThreadId() != g.mainThread) return g.partUpdate(self);
    g.manager = static_cast<char*>(self);
    g.inUpdate = true;
    g.batched = false;
    g.count = 0;
    g.cursor = 0;
    const uint64_t t0 = __rdtsc();
    g.partUpdate(self);
    g.st.managerCycles += __rdtsc() - t0;
    g.inUpdate = false;
    ++g.st.updates;
}

void EmitterUpdateHook(void* self) {
    if (!g.inUpdate || _ReturnAddress() != g.loopReturn || GetCurrentThreadId() != g.mainThread)
        return g.update(self);
    if (!g.batched) {
        g.batched = true;
        if (StartBatch(self)) {
            g.cursor = 1;  // (self was the copy's first)
            return;
        }
    }
    if (g.count == 0) {
        ++g.st.unbatchedCalls;
        return g.update(self);
    }
    for (uint32_t k = 0; k < kAhead && g.cursor + k < g.count; ++k)
        if (g.list[g.cursor + k] == self) {
            g.st.ahead += k;
            g.cursor += k + 1;
            ++g.st.skipped;
            return;
        }
    ++g.st.newCalls;  // (an emitter the list gained since the batch: updated here, as in the game)
    g.update(self);
}

// --- the serial stubs ------------------------------------------------------------------------------------------------
void WriteStub(unsigned id) {
    uint8_t* const s = g.stubs + size_t(id) * kStub;
    uint8_t* const targets = reinterpret_cast<uint8_t*>(g_particleSerialTargets);
    const int32_t toCheck = int32_t(targets + size_t(kMaxSerial) * 8 - (s + 11));
    uint8_t c[kStub] = {0xB8, 0, 0, 0, 0,        // mov eax, id
                        0xFF, 0x25, 0, 0, 0, 0,  // jmp [rip+check]
                        0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    std::memcpy(c + 1, &id, 4);
    std::memcpy(c + 7, &toCheck, 4);
    std::memcpy(s, c, kStub);
}

// whether p is in optiperf's own image (another part's hook in a vtable: the skinning part's GetSkeletonPose)
bool InOurImage(const void* p) {
    static char* base = nullptr;
    static size_t size = 0;
    if (!base) {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&InOurImage), &self))
            return false;
        base = reinterpret_cast<char*>(self);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        size = nt->OptionalHeader.SizeOfImage;
    }
    return p >= base && p < base + size;
}

// a class's vtable length: its entries leading into the module's code (or into another part's hook)
int Slots(char* module, void** vt) {
    int n = 0;
    while (n < kMaxSlots) {
        const void* fn = CallTrace_Original(vt[n]);
        if (!IsInCode(module, static_cast<const char*>(fn), 1) && !InOurImage(fn)) break;
        ++n;
    }
    return n;
}

bool PatchEntry(void** entry, void* to) {
    DWORD old;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(entry, to);
    VirtualProtect(entry, sizeof(void*), old, &old);
    return true;
}

int WorkerCount() {
    const std::wstring v =
        ConfigString(L"ParticleUpdate", L"Workers", L"OPTIPERF_PARTICLE_UPDATE_WORKERS", L"auto");
    if (!v.empty() && _wcsicmp(v.c_str(), L"auto") != 0)
        return (std::max)(1, (std::min)(kMaxWorkers, int(wcstol(v.c_str(), nullptr, 10))));
    const unsigned long long others = FlushThread_OthersMask();
    DWORD_PTR process = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    const int k = others ? int(__popcnt64(others)) - 1 : int(__popcnt64(process)) / 2 - 1;
    return (std::max)(1, (std::min)(4, k));
}

// At the second flush, on the main thread: everything located and checked, then put in (the emitters' update slot
// last).
void Setup(void* renderer) {
    g.setupTried = true;
    g.mainThread = GetCurrentThreadId();
    g.renderer = renderer;
    g.e3d = reinterpret_cast<char*>(GetModuleHandleW(L"Cry3DEngine.dll"));
    const char* why = g.e3d ? nullptr : "no Cry3DEngine.dll";
    char *partUpdate = nullptr, *loop = nullptr, *visUpdate = nullptr;
    void** emitterVt = nullptr;
    if (!why && !(partUpdate = Unique(g.e3d, kPartManagerUpdate))) why = "CPartManager::Update not found";
    if (!why) {
        const auto hits = FindInRange(partUpdate, kPartUpdateBytes, ParsePattern(kUpdateLoop), 2);
        if (hits.size() == 1)
            loop = hits[0];
        else
            why = "CPartManager::Update's update loop isn't as expected";
    }
    if (!why) {
        char* const load = partUpdate + kGEnvLoad;
        g.genv = reinterpret_cast<char**>(load + 7 + int32_t(ReadU32(load + 3)));
        if (!IsInImage(g.e3d, reinterpret_cast<char*>(g.genv), 8) || !*g.genv) why = "the 3D engine's gEnv not found";
    }
    if (!why && g.mode == PMode::Time) {  // (the manager's update timed, nothing else)
        HookSpec hook = {"CPartManager::Update", partUpdate, reinterpret_cast<void*>(&PartUpdateHook),
                         reinterpret_cast<void**>(&g.partUpdate)};
        if (!InstallHooks(&hook, 1)) {
            Log("particle update: off: could not write the hook");
            return;
        }
        g.ready = true;
        Log("particle update: mode=time: CPartManager::Update +0x%X timed (the game's own update)",
            unsigned(partUpdate - g.e3d));
        return;
    }
    if (!why && !(visUpdate = Unique(g.e3d, kVisEnvironUpdate))) why = "SVisEnviron::Update not found";
    char* findVisArea = nullptr;
    if (!why && !(findVisArea = Unique(g.e3d, kFindVisArea))) why = "CVisArea::FindVisArea not found";
    if (!why) {
        const auto hits = FindInRange(findVisArea, kFindVisAreaBytes, ParsePattern(kFindVisAreaCall), 2);
        char* const search = hits.size() == 1 ? RelTarget(hits[0] + kFindVisAreaCallAt) : nullptr;
        if (search && IsInCode(g.e3d, search, 40) && MatchesAt(search, ParsePattern(kVisSearch)))
            g.visSearch = reinterpret_cast<VisSearchFn>(search);
        else
            why = "CVisArea::FindVisArea's search isn't as expected";
    }
    if (!why) {
        emitterVt = FindRttiVtable(g.e3d, ".?AVCParticleEmitter@@");
        if (!emitterVt || !IsInCode(g.e3d, static_cast<const char*>(CallTrace_Original(emitterVt[kUpdateSlot])), 1))
            why = "CParticleEmitter's vtable not found";
    }
    // the renderer's GetCamera through the gate (the render thread): the workers' answer goes in front of it
    if (!why && RenderGate_Slots() > 0) {
        g.cameraIndex = renderer ? RenderGate_IndexOf(*static_cast<char***>(renderer), kGetCameraSlot) : -1;
        if (g.cameraIndex < 0) why = "the renderer's GetCamera isn't gated";
    }
    // the serial calls' vtable entries
    struct Entry {
        void** entry;
        unsigned id;
        int release;  // a Release kept for the main thread (its index), or -1
    };
    std::vector<Entry> patches;
    char found[512] = "";
    size_t used = 0;
    for (const SerialClass& c : kClasses) {
        if (why) break;
        char* const module = reinterpret_cast<char*>(GetModuleHandleW(c.module));
        if (!module) continue;  // (not loaded: nothing of it can be called)
        void** const vt = FindRttiVtable(module, c.mangled);
        const int slots = vt ? Slots(module, vt) : 0;
        if (!slots) {
            why = "a class of the serial calls not found";
            Log("particle update: %s's vtable not found in %ls", c.name, c.module);
            break;
        }
        int added = 0;
        for (int k = 0; k < c.n && !why; ++k) {
            const int first = c.slots[k] == kAll ? 0 : c.slots[k], last = c.slots[k] == kAll ? slots : c.slots[k] + 1;
            if (last > slots) {
                why = "a class's vtable is shorter than expected";
                Log("particle update: %s's vtable has %d slots", c.name, slots);
                break;
            }
            for (int s = first; s < last; ++s) {
                if (patches.size() + 1 >= kMaxSerial) {
                    why = "too many serial calls";
                    break;
                }
                const unsigned id = unsigned(patches.size()) + 1;  // (id 0: SVisEnviron::Update)
                patches.push_back({vt + s, id, -1});
                snprintf(g.labels[id], sizeof g.labels[id], "%s %d", c.name, s);
                ++added;
            }
        }
        if (!why && c.release != kNoRelease) {
            if (c.releaseSlot >= slots || patches.size() + 1 >= kMaxSerial) {
                why = "a class's vtable is shorter than expected";
            } else {
                const unsigned id = unsigned(patches.size()) + 1;
                patches.push_back({vt + c.releaseSlot, id, c.release});
                snprintf(g.labels[id], sizeof g.labels[id], "%s %d (kept)", c.name, c.releaseSlot);
                ++added;
            }
        }
        const int w = snprintf(found + used, sizeof found - used, "%s%s %d", used ? ", " : "", c.name, added);
        if (w > 0 && size_t(w) < sizeof found - used) used += size_t(w);
    }
    if (why) {
        Log("particle update: off: %s", why);
        return;
    }
    // the stubs: the code pages, then their targets (and the entry) on a page of their own
    const size_t code = (size_t(kMaxSerial) * kStub + 0xFFF) & ~size_t(0xFFF);
    g.stubs = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, code + 0x1000 * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g.stubs) {
        Log("particle update: off: no memory for the stubs");
        return;
    }
    g_particleSerialTargets = reinterpret_cast<void**>(g.stubs + code);
    g_particleSerialTargets[kMaxSerial] = reinterpret_cast<void*>(&ParticleSerialCheck64);
    for (const Entry& p : patches) g_particleSerialTargets[p.id] = *p.entry;
    std::snprintf(g.labels[0], sizeof g.labels[0], "SVisEnviron::Update");
    g.stubCount = unsigned(patches.size()) + 1;
    for (unsigned id = 1; id < g.stubCount; ++id) WriteStub(id);  // (id 0: SVisEnviron::Update's hook, counted)
    DWORD old;
    if (!VirtualProtect(g.stubs, code, PAGE_EXECUTE_READ, &old)) {
        Log("particle update: off: could not make the stubs executable");
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), g.stubs, code);
    InitializeCriticalSectionAndSpinCount(&g.lock, 4000);
    // the workers (waiting until the first batch)
    if (g.mode == PMode::On) {
        const int n = WorkerCount();
        // (normal: they share the other threads' processors with the physics', the particle thread's, the streaming's)
        const bool above = _wcsicmp(ConfigString(L"ParticleUpdate", L"Priority", L"OPTIPERF_PARTICLE_UPDATE_PRIORITY",
                                                 L"normal").c_str(), L"above") == 0;
        const unsigned long long others = FlushThread_OthersMask();
        for (int i = 0; i < n; ++i) {
            Worker& w = g.workers[i];
            w.go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            DWORD id = 0;
            w.thread = w.go ? CreateThread(nullptr, kWorkerStack, &WorkerMain, &w, STACK_SIZE_PARAM_IS_A_RESERVATION, &id)
                            : nullptr;
            if (!w.thread) break;
            if (above) SetThreadPriority(w.thread, THREAD_PRIORITY_ABOVE_NORMAL);
            if (others) SetThreadAffinityMask(w.thread, DWORD_PTR(others));
            using SetDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
            if (auto set = reinterpret_cast<SetDescriptionFn>(
                    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))) {
                wchar_t name[48];
                swprintf(name, 48, L"optiperf particles %d", i + 1);
                set(w.thread, name);
            }
            g.workerCount = i + 1;
        }
        if (g.workerCount == 0) {
            Log("particle update: off: no worker thread could be started (error %lu)", GetLastError());
            return;
        }
    }
    HookSpec hooks[] = {
        {"CPartManager::Update", partUpdate, reinterpret_cast<void*>(&PartUpdateHook),
         reinterpret_cast<void**>(&g.partUpdate)},
        {"SVisEnviron::Update", visUpdate, reinterpret_cast<void*>(&VisEnvironHook),
         reinterpret_cast<void**>(&g.visUpdate)},
        {"CVisArea::FindVisArea", findVisArea, reinterpret_cast<void*>(&FindVisAreaHook),
         reinterpret_cast<void**>(&g.findVisArea)},
    };
    if (!InstallHooks(hooks, 3)) {
        Log("particle update: off: could not write the hooks");
        return;
    }
    // the entries' pages made writable for good (a batch then points them at the stubs and back with plain stores)
    int unwritable = 0;
    for (const Entry& p : patches) {
        DWORD was = 0;
        if (!VirtualProtect(p.entry, sizeof(void*), PAGE_READWRITE, &was)) {
            ++unwritable;
        } else if (p.release < 0) {
            g.patches.push_back({p.entry, g.stubs + size_t(p.id) * kStub, *p.entry});
        } else {
            static void* const kHooks[kReleases] = {reinterpret_cast<void*>(&ReleaseHook<0>),
                                                    reinterpret_cast<void*>(&ReleaseHook<1>)};
            g.releases[p.release] = reinterpret_cast<ReleaseFn>(*p.entry);
            g.releaseIds[p.release] = p.id;
            g.patches.push_back({p.entry, kHooks[p.release], *p.entry});
        }
    }
    if (unwritable) {  // (a serial call left as it is: no batch may run)
        Log("particle update: off: %d vtable entries can't be made writable", unwritable);
        return;
    }
    if (g.cameraIndex >= 0) {
        g.cameraNext = reinterpret_cast<CameraFn>(RenderGate_Target(unsigned(g.cameraIndex)));
        RenderGate_Redirect(unsigned(g.cameraIndex), reinterpret_cast<void*>(&CameraHook));
    }
    g.list.reserve(4096);
    g.kept.reserve(1024);
    g.loopReturn = loop + kUpdateReturn;
    g.update = reinterpret_cast<UpdateFn>(emitterVt[kUpdateSlot]);
    if (!PatchEntry(&emitterVt[kUpdateSlot], reinterpret_cast<void*>(&EmitterUpdateHook))) {
        Log("particle update: off: could not write CParticleEmitter's vtable");
        return;
    }
    g.ready = true;
    Log("particle update: mode=%s, %d worker%s%s: CPartManager::Update +0x%X (its loop's update call +0x%X), "
        "CParticleEmitter::Update +0x%X; serial calls: SVisEnviron::Update +0x%X (computing an environment), %s (%u "
        "stubs); CVisArea::FindVisArea +0x%X with a list per thread; the renderer's GetCamera %s",
        ModeName(g.mode), g.workerCount, g.workerCount == 1 ? "" : "s",
        FlushThread_OthersMask() ? " (on the other threads' processors)" : "", unsigned(partUpdate - g.e3d),
        unsigned(static_cast<const char*>(g.loopReturn) - g.e3d),
        unsigned(reinterpret_cast<char*>(g.update) - g.e3d), unsigned(visUpdate - g.e3d), found, g.stubCount,
        unsigned(findVisArea - g.e3d), g.cameraIndex >= 0 ? "answered to the workers (gated)" : "called by the workers (no gate)");
}
#endif  // _WIN64

}  // namespace

void ParticleUpdate_Install(const wchar_t* moduleName) {
#ifndef _WIN64
    if (ReadMode() != PMode::Off) Log("%ls: particle update off: 64-bit game only", moduleName);
#else
    if (g.installed) return;
    g.mode = ReadMode();
    if (g.mode == PMode::Off) return;
    if (_wcsicmp(ConfigString(L"Debug", L"TraversalCensus", L"OPTIPERF_TRAVERSAL_CENSUS", L"off").c_str(), L"off") !=
        0) {
        Log("%ls: particle update off: the traversal census is on (it watches the same calls)", moduleName);
        return;
    }
    g.installed = true;
    Log("%ls: particle update: mode=%s (set up at the second flush)", moduleName, ModeName(g.mode));
#endif
}

void ParticleUpdate_FlushEntry(void* renderer) {
#ifdef _WIN64
    if (!g.installed || g.setupTried) return;
    if (++g.flushes >= 2) Setup(renderer);
#else
    (void)renderer;
#endif
}

void ParticleUpdate_Report(const char* when) {
#ifdef _WIN64
    if (!g.installed || !g.ready) return;
    const Stats& s = g.st;
    Log("particle update [%s]: mode=%s: the manager's update %.3f ms a frame (%llu frames)", when, ModeName(g.mode),
        Ms(s.managerCycles) / double(s.updates ? s.updates : 1), static_cast<unsigned long long>(s.updates));
    if (g.mode == PMode::Time) return;
    const double b = double(s.batches ? s.batches : 1);
    uint64_t workCycles = s.mainCycles, serial = 0;
    for (int i = 0; i < g.workerCount; ++i) workCycles += g.workers[i].cycles;
    for (unsigned id = 0; id < g.stubCount; ++id) serial += g.serialCalls[id];
    Log("particle update [%s]: mode=%s, %d workers | manager updates %llu, batches %llu (%.1f emitters each, %.0f %% by "
        "the main thread); not batched: fewer than %u emitters %llu, profiler on %llu, the loop's first call not the "
        "list's first %llu, GetCamera redirected since %llu | the loop's calls skipped %llu (found ahead %llu), made: "
        "emitters new since the batch %llu, in frames without one %llu | a batch %.3f ms (the threads' time in it "
        "%.3f ms), the main thread's own part %.3f ms, then its wait %.3f ms; longest %.3f ms, over 2 ms %llu | serial "
        "calls %llu (%.1f a batch), waited for %llu times (%.3f ms in all) | Releases kept for the main thread %llu",
        when, ModeName(g.mode), g.workerCount, static_cast<unsigned long long>(s.updates),
        static_cast<unsigned long long>(s.batches), double(s.emitters) / b,
        s.emitters ? 100.0 * double(s.mainEmitters) / double(s.emitters) : 0.0, kMinBatch,
        static_cast<unsigned long long>(s.few), static_cast<unsigned long long>(s.profiler),
        static_cast<unsigned long long>(s.notFirst), static_cast<unsigned long long>(s.cameraLost),
        static_cast<unsigned long long>(s.skipped), static_cast<unsigned long long>(s.ahead),
        static_cast<unsigned long long>(s.newCalls), static_cast<unsigned long long>(s.unbatchedCalls),
        Ms(s.wallCycles) / b, Ms(workCycles) / b, Ms(s.mainCycles) / b, Ms(s.waitCycles) / b, Ms(s.longest),
        static_cast<unsigned long long>(s.over2ms), static_cast<unsigned long long>(serial), double(serial) / b,
        static_cast<unsigned long long>(g.contended), Ms(g.lockWaitCycles),
        static_cast<unsigned long long>(s.keptReleases));
    char line[1536] = "";
    size_t n = 0;
    for (int i = 0; i < g.workerCount; ++i) {
        const Worker& w = g.workers[i];
        const int k = snprintf(line + n, sizeof line - n, " %d: %llu/%llu (%llu from other ranges)/%.1f", i + 1,
                               static_cast<unsigned long long>(w.batches), static_cast<unsigned long long>(w.updates),
                               static_cast<unsigned long long>(w.stolen), Ms(w.cycles));
        if (k > 0 && size_t(k) < sizeof line - n) n += size_t(k);
    }
    if (g.workerCount)
        Log("particle update [%s]: workers (batches/emitters/ms):%s; the main thread took %llu from other ranges", when,
            line, static_cast<unsigned long long>(s.mainStolen));
    // the serial calls made most
    unsigned order[kMaxSerial];
    for (unsigned id = 0; id < g.stubCount; ++id) order[id] = id;
    std::sort(order, order + g.stubCount, [](unsigned a, unsigned c) { return g.serialCalls[a] > g.serialCalls[c]; });
    n = 0;
    line[0] = 0;
    for (unsigned k = 0; k < g.stubCount && k < 16 && g.serialCalls[order[k]]; ++k) {
        const int w = snprintf(line + n, sizeof line - n, "%s %s:%llu", k ? "," : "", g.labels[order[k]],
                               static_cast<unsigned long long>(g.serialCalls[order[k]]));
        if (w > 0 && size_t(w) < sizeof line - n) n += size_t(w);
    }
    Log("particle update [%s]: serial calls made most:%s", when, n ? line : " none");
#else
    (void)when;
#endif
}

}  // namespace optiperf

#ifdef _WIN64
// The serial lock, from ParticleSerial64 on a batch thread (recursive: a serial call may make another).
extern "C" void ParticleSerialEnter(unsigned id) {
    using optiperf::g;
    if (!TryEnterCriticalSection(&g.lock)) {
        const uint64_t t0 = __rdtsc();
        EnterCriticalSection(&g.lock);
        g.lockWaitCycles += __rdtsc() - t0;
        ++g.contended;
    }
    if (id < optiperf::kMaxSerial) ++g.serialCalls[id];
}

extern "C" void ParticleSerialLeave() { LeaveCriticalSection(&optiperf::g.lock); }
#endif
