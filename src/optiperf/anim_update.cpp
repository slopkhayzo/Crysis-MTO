// The characters' forward kinematics on several threads: see anim_update.h.
//
// CSkeletonAnim's step (+0x8FEC0, from the pre-process and from a parent's post-process for an attached character):
// its guard, the animation's update (+0x904E0), forward kinematics (+0x90B60) if the pose's flag says so, the queues'
// cleanup, the frame's stamp. The pre-process's hook marks the call (g.deferFk) when the character's forward kinematics
// is to wait; the forward kinematics' hook then records its arguments (copies: the first is a field the post-process
// overwrites) and returns, and the step goes on. A character's post-process (CCharInstance vtable slot 13) is recorded
// the same way, in the same job. The queue a waiting forward kinematics reads may gain animations meanwhile: it stops
// at the first one not activated yet, which the animation's update does; a queue cleared meanwhile leaves the pose as
// it is, as in the game between frames.
//
// The batch: a job list, a word with its end (high half) and next index (low half), claimed with a fetch-add, so a late
// worker's claim can only reach into the batch it reads the word of. The main thread takes part, then waits for the
// done count.

#include "anim_update.h"

#include <windows.h>
#include <intrin.h>
#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
#include "scan.h"

namespace optiperf {
namespace {

enum class AMode { Off, On, Serial, Time, Verify };

AMode ReadMode() {
    const std::wstring v = ConfigString(L"AnimUpdate", L"Mode", L"OPTIPERF_ANIM_UPDATE", L"off");
    if (_wcsicmp(v.c_str(), L"on") == 0 || v == L"1") return AMode::On;
    if (_wcsicmp(v.c_str(), L"serial") == 0) return AMode::Serial;
    if (_wcsicmp(v.c_str(), L"time") == 0) return AMode::Time;
    if (_wcsicmp(v.c_str(), L"verify") == 0) return AMode::Verify;
    return AMode::Off;
}

#ifdef _WIN64
const char* ModeName(AMode m) {
    return m == AMode::On       ? "on"
           : m == AMode::Serial ? "serial"
           : m == AMode::Time   ? "time"
           : m == AMode::Verify ? "verify"
                                : "off";
}

// --- patterns (x64 CryAnimation.dll, CryEntitySystem.dll, build 6156; research/profiling/re/mkpattern.py) ------------
// CSkeletonAnim's step, up to its calls of the animation's update and of forward kinematics
constexpr const char kStep[] =
    "48 89 5C 24 20 55 57 41 55 48 83 EC 30 F6 81 5C 03 00 00 02 45 8B E9 49 8B D8 48 8B FA 48 8B E9 77 1F 48 8B 05 ?? "
    "?? ?? ?? 4C 8B 99 B0 07 00 00 44 8B 50 68 45 39 93 70 12 00 00 0F 84 ?? ?? ?? ?? 48 89 74 24 50 4C 89 64 24 58 4C "
    "89 74 24 60 0F 29 74 24 20 E8 ?? ?? ?? ?? 4C 8B 9D A8 07 00 00 41 F6 83 9C 08 00 00 02 74 0E 4C 8B C3 48 8B D7 48 "
    "8B CD E8 ?? ?? ?? ??";
constexpr size_t kStepUpdateCall = 0x55, kStepFkCall = 0x74;
// in forward kinematics, its profiler check: the load of the module's gEnv pointer (mov rax, [rip+X])
constexpr const char kFkEnvLoad[] = "48 8B 05 ?? ?? ?? ?? 40 38 B0 D3 00 00 00";
constexpr size_t kFkEnvSearch = 0x400;
// CControllerPQLog::DecodeKey(this, out, animation, time)
constexpr const char kDecodeKey[] =
    "48 8B C4 48 89 58 10 48 89 68 18 56 57 41 54 48 81 EC 00 01 00 00 0F 29 70 D8 0F 29 78 C8 44 0F 29 40 B8 4C 89 68 "
    "08 8B 05 ?? ?? ?? ?? 33 F6 A8 01 49 63 E8 48 8B FA 44 0F 28 C3 48 8B D9";
// the two samplers with the static scratch for an animation-driven character's root motion
constexpr const char kSampler6[] =
    "48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 18 02 00 00 4C 8B 91 00 "
    "05 00 00 49 63 78 48 F3 41 0F 10 50 50 4C 2B 91 F8 04 00 00";
constexpr const char kSampler8[] =
    "48 8B C4 48 89 50 10 48 89 48 08 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 18 02 00 00 41 83 F9 09 44 0F 29 B0 "
    "28 FF FF FF 0F 29 70 A8 41 8B D9 4D 8B F8 4C 8B E2 48 8B E9";
// the TCB splines' evaluation (sorted and their tangents computed at the first, a key cached): a vector's, a rotation's
constexpr const char kSplineVec[] =
    "48 89 5C 24 18 48 89 6C 24 20 57 48 83 EC 40 F6 41 20 01 0F 29 74 24 30 49 8B E8 0F 28 F1 48 8B D9 74 7D 48 8B 79 "
    "10 48 89 74 24 58 48 8B 71 08 48 3B F7 74 53";
constexpr const char kSplineQuat[] =
    "40 53 57 41 54 48 83 EC 40 F6 41 20 01 0F 29 74 24 30 49 8B F8 0F 28 F1 48 8B D9 49 BC 65 21 0B 59 C8 42 16 B2 0F "
    "84 ?? ?? ?? ?? 48 89 6C 24 68 48 8B 69 08 48 89 74 24 70 48 8B 71 10 48 3B EE";
// the key-time tracks' searches: (track, animation, time, &difference) -> key, the last time, difference and key cached
// in the track (+0, +4, +8), its keys at +0x10; the key read back from the track at the function's end
constexpr const char kTrackSearch0[] =
    "F3 0F 10 05 ?? ?? ?? ?? 4D 8B D9 0F 28 CA 0F 2F D0 4C 8B D1 76 03 0F 28 C8 48 8B 05 ?? ?? ?? ?? 48 63 D2 48 69 "
    "D2 58 01 00 00 48 03 90 08 01 00 00 F3 0F 10 82 D8 00 00 00 F3 0F 10 92 DC 00 00 00 F3 0F 5C D0 F3 0F 59 D1 F3 "
    "0F 58 D0 F3 0F 59 15 ?? ?? ?? ?? 0F 2E 11 75 0A 8B 41 04 41 89 01 8B 41 08 C3 4C 8B 49 10 0F B7 51 0C F3 0F 11 "
    "11 41 0F B6 01";
constexpr const char kTrackSearch1[] =
    "F3 0F 10 05 ?? ?? ?? ?? 4D 8B D9 0F 28 CA 0F 2F D0 4C 8B D1 76 03 0F 28 C8 48 8B 05 ?? ?? ?? ?? 48 63 D2 48 69 "
    "D2 58 01 00 00 48 03 90 08 01 00 00 F3 0F 10 82 D8 00 00 00 F3 0F 10 92 DC 00 00 00 F3 0F 5C D0 F3 0F 59 D1 F3 "
    "0F 58 D0 F3 0F 59 15 ?? ?? ?? ?? 0F 2E 11 75 0A 8B 41 04 41 89 01 8B 41 08 C3 4C 8B 49 10 0F B7 51 0C F3 0F 11 "
    "11 41 0F B7 01";
constexpr const char kTrackSearch2[] =
    "48 89 5C 24 20 57 F3 0F 10 05 ?? ?? ?? ?? 49 8B F9 0F 28 CA 0F 2F D0 48 8B D9 76 03 0F 28 C8 48 8B 05 ?? ?? ?? "
    "??";
// the same inlined in a whole sample: (track, out, animation, time)
constexpr const char kTrackSample0[] =
    "48 83 EC 28 0F 29 74 24 10 F3 0F 10 35 ?? ?? ?? ?? 4C 8B DA 0F 2F DE 4C 8B D1 76 03 0F 28 DE 48 8B 05 ?? ?? ?? "
    "??";
constexpr const char kTrackSample1[] =
    "40 53 48 83 EC 40 F3 0F 10 05 ?? ?? ?? ?? 0F 28 CB 48 8B DA 0F 2F D8 4C 8B C9 76 03 0F 28 C8 48 8B 05 ?? ?? ?? "
    "??";
constexpr const char kTrackSample2[] =
    "48 89 5C 24 10 56 48 83 EC 50 F3 0F 10 05 ?? ?? ?? ?? 0F 29 74 24 40 48 8B F2 0F 2F D8 48 8B D9";
constexpr const char kTrackSample3[] =
    "40 57 48 83 EC 60 F3 0F 10 25 ?? ?? ?? ?? 0F 28 CB 48 8B FA 0F 2F DC 4C 8B D1 76 03 0F 28 CC 48 8B 05 ?? ?? ?? "
    "??";
constexpr const char kTrackSample4[] =
    "40 53 48 83 EC 70 F3 0F 10 25 ?? ?? ?? ?? 0F 28 CB 48 8B DA 0F 2F DC 4C 8B D1 76 03 0F 28 CC 48 8B 05 ?? ?? ?? "
    "??";
// and CKeyTimesInformation's (the bit set, 32-bit, 16-bit, 8-bit times): the cache at +0x18, +0x1C, +0x20, the keys at +0x10
constexpr const char kInfoSearch0[] =
    "40 57 F3 0F 10 05 ?? ?? ?? ?? 49 8B F9 0F 28 CA 0F 2F D0 4C 8B D9 76 03 0F 28 C8 48 8B 05 ?? ?? ?? ??";
constexpr const char kInfoSearch1[] =
    "F3 0F 10 05 ?? ?? ?? ?? 4D 8B D9 0F 28 CA 0F 2F D0 4C 8B D1 76 03 0F 28 C8 48 8B 05 ?? ?? ?? ?? 48 63 D2 48 69 "
    "D2 58 01 00 00 48 03 90 08 01 00 00 F3 0F 10 82 D8 00 00 00 F3 0F 10 92 DC 00 00 00 F3 0F 5C D0 F3 0F 59 D1 F3 "
    "0F 58 D0 F3 0F 59 15 ?? ?? ?? ?? 0F 2E 51 18 75 0A 8B 41 1C 41 89 01 8B 41 20 C3 4C 8B 49 10 F3 0F 11 51 18 41 "
    "0F 2F 11";
constexpr const char kInfoSearch2[] =
    "F3 0F 10 05 ?? ?? ?? ?? 4D 8B D9 0F 28 CA 0F 2F D0 4C 8B D1 76 03 0F 28 C8 48 8B 05 ?? ?? ?? ?? 48 63 D2 48 69 "
    "D2 58 01 00 00 48 03 90 08 01 00 00 F3 0F 10 82 D8 00 00 00 F3 0F 10 92 DC 00 00 00 F3 0F 5C D0 F3 0F 59 D1 F3 "
    "0F 58 D0 F3 0F 59 15 ?? ?? ?? ?? 0F 2E 51 18 75 0A 8B 41 1C 41 89 01 8B 41 20 C3 4C 8B 49 10 F3 0F 11 51 18 41 "
    "8B 51 FC 8D 42 FF 48 63 C8 41 0F B7 01";
constexpr const char kInfoSearch3[] =
    "F3 0F 10 05 ?? ?? ?? ?? 4D 8B D9 0F 28 CA 0F 2F D0 4C 8B D1 76 03 0F 28 C8 48 8B 05 ?? ?? ?? ?? 48 63 D2 48 69 "
    "D2 58 01 00 00 48 03 90 08 01 00 00 F3 0F 10 82 D8 00 00 00 F3 0F 10 92 DC 00 00 00 F3 0F 5C D0 F3 0F 59 D1 F3 "
    "0F 58 D0 F3 0F 59 15 ?? ?? ?? ?? 0F 2E 51 18 75 0A 8B 41 1C 41 89 01 8B 41 20 C3 4C 8B 49 10 F3 0F 11 51 18 41 "
    "8B 51 FC 8D 42 FF 48 63 C8 41 0F B6 01";
// the animation set's name lookup (lower-cases the name into a static buffer, CRCs it, looks it up)
constexpr const char kNameLookup[] =
    "48 89 7C 24 08 48 8B FA 4C 8D 0D ?? ?? ?? ?? 4C 8B D1 0F B6 07 49 8B D1 84 C0 74 25 49 2B F9 90";
// the animation's warnings (a format and its arguments)
constexpr const char kWarning[] =
    "48 89 54 24 10 4C 89 44 24 18 4C 89 4C 24 20 53 B8 30 10 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B 05";
// the entity system's deletion of an entity (every path: the pending list, a removal made at once, the rest)
constexpr const char kDeleteEntity[] =
    "48 8B C4 48 83 EC 78 48 89 58 08 48 89 70 F0 48 89 78 E8 4C 89 60 E0 8B 05 ?? ?? ?? ?? 45 33 E4 A8 01 48 8B FA 48 "
    "8B F1 48 8D 1D ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 48 8B 49 18 83 C8 01";
// the pre-process's and the post-process's starts (their vtable slots checked)
constexpr const char kPre[] =
    "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 54 41 55 41 56 48 81 EC 90 00 00 00";
constexpr const char kPost[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 81 EC E0 00 00 00 8B 84 24 18 01 00 00 F3 0F 10 84 24 10 01";

constexpr unsigned kAddRefSlot = 0, kReleaseSlot = 1, kPreSlot = 12, kPostSlot = 13;  // ICharacterInstance (Mod SDK)
constexpr unsigned kEsUpdateSlot = 2, kEsGetEntitySlot = 8;                           // IEntitySystem
constexpr unsigned kSysGetGameSlot = 53, kGameGetFrameworkSlot = 9;                   // ISystem, IGame
// IEntity's; IGameFramework's GetClientActorId: here seven slots after the Mod SDK's (CryGame compares its answer with
// an entity's id; the SDK's slot 40 is a blocking spawn)
constexpr unsigned kEntityGetCharacterSlot = 77, kFwGetClientActorIdSlot = 47;
constexpr unsigned kAddFrameProfilerSlot = 1;                                         // IFrameProfileSystem
constexpr size_t kProfilerOn = 0xD3;       // gEnv's bProfilerEnabled
constexpr size_t kRootMotion = 0x728;      // CSkeletonAnim: the relative rotation and translation (16 bytes)
constexpr size_t kRootMotionBytes = 16;
constexpr size_t kPoseAnim = 0x8D0;        // CSkeletonPose: its CSkeletonAnim
constexpr size_t kAnimFlags = 0x35C;       // CSkeletonAnim: bit 0 animation-driven motion (the samplers' static scratch)
constexpr size_t kAnimOwner = 0x7B0;       // CSkeletonAnim: its CCharInstance
constexpr size_t kAnimPose = 0x7A8;        // CSkeletonAnim: its CSkeletonPose
// CSkeletonPose: the joints' arrays forward kinematics writes (a QuatT each), for Mode=verify
constexpr size_t kPoseRelative = 0x4E0, kPoseAbsolute = 0x4F8, kPoseAbsoluteEnd = 0x500;
constexpr float kVerifyTolerance = 1.0e-3f;
constexpr size_t kDecodeHeader = 0x20;     // CControllerPQLog: the vtable, the keys' and times' pointers; then...
constexpr size_t kDecodeBytes = 0x40;      // ...the cached time and value
constexpr size_t kTrackBytes = 0x20;       // a track: the cached time at 0; the rest as the searches read it
constexpr size_t kInfoBytes = 0x28, kInfoTime = 0x18;  // a CKeyTimesInformation: the cached time at +0x18
constexpr int kTrackSearches = 3, kTrackSamples = 5, kInfoSearches = 4;
// a cached time no animation's key time equals (not a NaN: the game's compare, ucomiss then jne, takes a NaN as equal)
constexpr float kNoTime = -1.0e30f;

constexpr uint32_t kMinBatch = 6;   // forward kinematics, for the workers to be woken
constexpr uint64_t kWarmup = 8;     // flushes on the main thread alone (the statics' one-time setups)
constexpr size_t kMaxJobs = 2048;
constexpr uint32_t kTable = 4096;   // (a power of two, at least twice kMaxJobs)
constexpr int kMaxWorkers = 8;
constexpr SIZE_T kWorkerStack = SIZE_T(2) << 20;

using PreFn = void (*)(char* self, const float* phys, const float* loc, const void* camera, uint32_t onRender);
using PostFn = void (*)(char* self, const float* phys, const float* loc, void* attachment, float zoom, uint32_t onRender);
using FkFn = void (*)(char* anim, const float* phys, const float* loc);
using AnimUpdateFn = void (*)(char* anim);
using EntityUpdateFn = void (*)(void* self);
using DeleteEntityFn = void (*)(void* self, void* entity);
using DecodeKeyFn = float* (*)(char* self, float* out, int animation, float time);
using Sampler6Fn = void* (*)(char* pose, void* out, void* a, void* b, void* c, float d);
using Sampler8Fn = void* (*)(char* pose, void* out, void* a, void* b, void* c, void* d, void* e, float f);
using SplineFn = void (*)(void* spline, float time, float* out);
using AddFrameProfilerFn = void (*)(void* self, void* profiler);
using TrackSearchFn = uint32_t (*)(char* track, int animation, float time, float* difference);
using TrackSampleFn = void* (*)(char* track, float* out, int animation, float time);
using NameLookupFn = void* (*)(void* map, const char* name);
using WarningFn = void (*)(void* file, const char* format, void* a, void* b, void* c, void* d, void* e, void* f, void* h, void* i);

struct Job {
    char* instance;
    char* anim;  // its CSkeletonAnim while its forward kinematics waits, else null
    bool post;   // its post-process waits
    float zoom;
    uint32_t onRender;
    float fkPhys[7], fkLoc[8];      // QuatT, QuatTS
    float postPhys[7], postLoc[8];
};

struct Slot {  // the instance's job this flush (an entry of another generation is free)
    char* key;
    uint32_t generation, job;
};

#pragma warning(push)
#pragma warning(disable : 4324)  // (padded to their cache lines: that is what alignas is for here)
struct alignas(64) Worker {
    HANDLE thread = nullptr, go = nullptr;
    uint64_t batches = 0, fks = 0, cycles = 0;  // (its own thread's)
};

struct Stats {
    uint64_t updates = 0, updateCycles = 0;
    uint64_t preCalls = 0, preCycles = 0, postCalls = 0, postCycles = 0, fkCalls = 0, fkCycles = 0;  // (made in place)
    uint64_t fkWaited = 0, postWaited = 0;
    uint64_t otherCaller = 0, localPlayer = 0, localFound = 0, tooMany = 0;
    uint64_t fkMadeFirst = 0;  // a post-process made in place, its waiting forward kinematics first
    uint64_t flushes = 0, batches = 0, deletions = 0;
    uint64_t soloProfiler = 0, soloFew = 0, soloWarmup = 0;
    uint64_t fkCycles2 = 0, waitCycles = 0, postCycles2 = 0, flushCycles = 0, longest = 0, over2ms = 0;
    uint64_t mainFks = 0;
    uint64_t verifyBatch = 0, verifyBatchDiffer = 0, verifySolo = 0, verifySoloDiffer = 0, verifyLogged = 0;
    uint64_t lockedSamplers = 0, lockedSplines = 0, lockedProfilers = 0, lockedLookups = 0, contended = 0, lockWaitCycles = 0;
};

struct State {
    AMode mode = AMode::Off;
    bool installed = false, setupTried = false, ready = false;
    int flushCount = 0;
    DWORD mainThread = 0;
    char *anim = nullptr, *action = nullptr, *entity = nullptr;
    size_t animSize = 0, actionSize = 0, entitySize = 0;
    char** genv = nullptr;
    // the game's functions (trampolines)
    PreFn pre = nullptr;
    PostFn post = nullptr;
    FkFn fk = nullptr;
    AnimUpdateFn animUpdate = nullptr;
    EntityUpdateFn entityUpdate = nullptr;
    DeleteEntityFn deleteEntity = nullptr;
    DecodeKeyFn decodeKey = nullptr;
    Sampler6Fn sampler6 = nullptr;
    Sampler8Fn sampler8 = nullptr;
    SplineFn splineVec = nullptr, splineQuat = nullptr;
    AddFrameProfilerFn addFrameProfiler = nullptr;
    TrackSearchFn trackSearch[kTrackSearches] = {};
    TrackSampleFn trackSample[kTrackSamples] = {};
    TrackSearchFn infoSearch[kInfoSearches] = {};
    NameLookupFn nameLookup = nullptr;
    WarningFn warning = nullptr;
    // the entity update (main thread)
    bool defer = false;       // the mode makes work wait
    bool window = false;      // inside the entity update
    bool flushing = false;
    bool deferFk = false;     // inside a pre-process whose forward kinematics is to wait
    float rootSaved[4] = {};  // the root motion, from before the animation's update
    void* localCharacter = nullptr;
    std::vector<Job> jobs;
    std::vector<float> verifyCopy;
    uint32_t generation = 1;
    Slot table[kTable] = {};
    // the batch, for the workers
    alignas(64) std::atomic<uint64_t> claim{0};  // its end (high half), the next index (low half)
    alignas(64) std::atomic<uint32_t> done{0};
    Job* items = nullptr;
    unsigned mxcsr = 0;
    int workerCount = 0;
    Worker workers[kMaxWorkers];
    CRITICAL_SECTION lock;
    Stats st;
};
#pragma warning(pop)
State g;
thread_local bool t_batch = false;  // a thread of a batch, while it works

double Ms(uint64_t cycles) { return double(cycles) / 3.0e6; }

char* Unique(char* base, const char* text) {
    const auto hits = FindInCode(base, ParsePattern(text), 2);
    return hits.size() == 1 ? hits[0] : nullptr;
}

template <class R, class... A>
R VCall(void* self, unsigned slot, A... a) {
    return reinterpret_cast<R (*)(void*, A...)>((*static_cast<void***>(self))[slot])(self, a...);
}

size_t ImageSize(const char* base) {
    if (!base) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

bool ProfilerOn() {
    const char* env = *g.genv;
    return env && env[kProfilerOn] != 0;
}

// the calls that may wait: the characters' (CryAction), the render proxies' (CryEntitySystem)
bool ChosenCaller(const void* ret) {
    const char* r = static_cast<const char*>(ret);
    return (r >= g.action && r < g.action + g.actionSize) || (r >= g.entity && r < g.entity + g.entitySize);
}

void* LocalCharacter(void* entitySystem) {
    void* const system = *reinterpret_cast<void**>(*g.genv);  // (gEnv's first member)
    void* const game = system ? VCall<void*>(system, kSysGetGameSlot) : nullptr;
    void* const framework = game ? VCall<void*>(game, kGameGetFrameworkSlot) : nullptr;
    const uint32_t id = framework ? VCall<uint32_t>(framework, kFwGetClientActorIdSlot) : 0;
    void* const entity = id ? VCall<void*>(entitySystem, kEsGetEntitySlot, id) : nullptr;
    return entity ? VCall<void*>(entity, kEntityGetCharacterSlot, 0) : nullptr;
}

void LockEnter() {
    if (!TryEnterCriticalSection(&g.lock)) {
        const uint64_t t0 = __rdtsc();
        EnterCriticalSection(&g.lock);
        g.st.lockWaitCycles += __rdtsc() - t0;
        ++g.st.contended;
    }
}

// --- the jobs (main thread) -------------------------------------------------------------------------------------------
uint32_t TableStart(const char* instance) {
    return uint32_t((uint64_t(reinterpret_cast<uintptr_t>(instance) >> 6) * 0x9E3779B97F4A7C15ull) >> 52) & (kTable - 1);
}

Job* FindJob(const char* instance) {
    for (uint32_t i = TableStart(instance);; i = (i + 1) & (kTable - 1)) {
        const Slot& s = g.table[i];
        if (s.generation != g.generation) return nullptr;
        if (s.key == instance) return &g.jobs[s.job];
    }
}

Job* NewJob(char* instance) {
    if (g.jobs.size() >= kMaxJobs) return nullptr;
    uint32_t i = TableStart(instance);
    while (g.table[i].generation == g.generation) i = (i + 1) & (kTable - 1);
    g.table[i] = {instance, g.generation, uint32_t(g.jobs.size())};
    g.jobs.push_back({});
    Job& j = g.jobs.back();
    j.instance = instance;
    VCall<void>(instance, kAddRefSlot);  // (kept alive for the flush; the count isn't atomic: main thread, no batch)
    return &j;
}

// --- the flush --------------------------------------------------------------------------------------------------------
void RunFk(Job& j) {
    std::memset(j.anim + kRootMotion, 0, kRootMotionBytes);  // (as the animation's update left it for forward kinematics)
    g.fk(j.anim, j.fkPhys, j.fkLoc);
}

uint32_t Work() {
    uint32_t mine = 0;
    for (;;) {
        const uint64_t w = g.claim.fetch_add(1, std::memory_order_acq_rel);
        const uint32_t i = uint32_t(w), end = uint32_t(w >> 32);
        if (i >= end) return mine;
        Job& j = g.items[i];
        if (j.anim) {
            RunFk(j);
            ++mine;
        }
        g.done.fetch_add(1, std::memory_order_release);
    }
}

DWORD WINAPI WorkerMain(void* p) {
    Worker& w = *static_cast<Worker*>(p);
    t_batch = true;
    for (;;) {
        if (WaitForSingleObject(w.go, INFINITE) != WAIT_OBJECT_0) return 1;
        _mm_setcsr(g.mxcsr);
        const uint64_t t0 = __rdtsc();
        w.fks += Work();
        w.cycles += __rdtsc() - t0;
        ++w.batches;
    }
}

// Mode=verify: forward kinematics made again here, on the main thread alone, against what the flush made of it (a batch's
// or, as a control, the main thread's own)
void VerifyFk(Job& j, bool batch) {
    char* const pose = *reinterpret_cast<char**>(j.anim + kAnimPose);
    const float* const relative = *reinterpret_cast<float**>(pose + kPoseRelative);
    const float* const absolute = *reinterpret_cast<float**>(pose + kPoseAbsolute);
    const size_t floats = size_t(*reinterpret_cast<char**>(pose + kPoseAbsoluteEnd) - reinterpret_cast<const char*>(absolute)) / sizeof(float);
    const size_t root = kRootMotionBytes / sizeof(float);
    if (g.verifyCopy.size() < floats * 2 + root) g.verifyCopy.resize(floats * 2 + root);
    float* const c = g.verifyCopy.data();
    std::memcpy(c, relative, floats * sizeof(float));
    std::memcpy(c + floats, absolute, floats * sizeof(float));
    std::memcpy(c + floats * 2, j.anim + kRootMotion, kRootMotionBytes);
    RunFk(j);
    const float* const newRoot = reinterpret_cast<const float*>(j.anim + kRootMotion);
    size_t differ = 0, first = 0;
    float was = 0, now = 0;
    const auto compare = [&](const float* x, const float* y, size_t n, size_t base) {
        for (size_t i = 0; i < n; ++i)
            if (!(std::fabs(x[i] - y[i]) <= kVerifyTolerance)) {
                if (!differ) {
                    first = base + i;
                    was = x[i];
                    now = y[i];
                }
                ++differ;
            }
    };
    compare(c, relative, floats, 0);
    compare(c + floats, absolute, floats, floats);
    compare(c + floats * 2, newRoot, root, floats * 2);
    Stats& s = g.st;
    ++(batch ? s.verifyBatch : s.verifySolo);
    if (!differ) return;
    ++(batch ? s.verifyBatchDiffer : s.verifySoloDiffer);
    if (++s.verifyLogged <= 40)
        Log("anim update: verify: %s: a pose of %zu floats differs at %zu places, first at %zu: %f, made again %f (the relative pose, "
            "then the absolute one, then the root motion)",
            batch ? "a batch's" : "the main thread's own", floats, differ, first, double(was), double(now));
}

// What waits, now: forward kinematics (all at once), then the post-processes in the order the game asked for them
void Flush() {
    if (g.flushing || g.jobs.empty()) return;
    g.flushing = true;
    Stats& s = g.st;
    const uint64_t t0 = __rdtsc();
    const uint32_t n = uint32_t(g.jobs.size());
    uint32_t fks = 0;
    for (const Job& j : g.jobs) fks += j.anim != nullptr;
    bool workers = (g.mode == AMode::On || g.mode == AMode::Verify) && g.workerCount > 0;
    if (workers && s.flushes < kWarmup) {
        ++s.soloWarmup;
        workers = false;
    } else if (workers && fks < kMinBatch) {
        ++s.soloFew;
        workers = false;
    } else if (workers && ProfilerOn()) {
        ++s.soloProfiler;
        workers = false;
    }
    uint64_t t1 = t0;
    if (workers) {
        g.mxcsr = _mm_getcsr();
        g.items = g.jobs.data();
        g.done.store(0, std::memory_order_relaxed);
        g.claim.store(uint64_t(n) << 32, std::memory_order_release);
        for (int i = 0; i < g.workerCount; ++i) SetEvent(g.workers[i].go);
        t_batch = true;
        s.mainFks += Work();
        t_batch = false;
        t1 = __rdtsc();
        while (g.done.load(std::memory_order_acquire) < n) _mm_pause();
        ++s.batches;
    } else {
        for (Job& j : g.jobs)
            if (j.anim) RunFk(j);
        t1 = __rdtsc();
    }
    const uint64_t t2 = __rdtsc();
    if (g.mode == AMode::Verify)
        for (Job& j : g.jobs)
            if (j.anim) VerifyFk(j, workers);
    for (const Job& j : g.jobs)
        if (j.post) g.post(j.instance, j.postPhys, j.postLoc, nullptr, j.zoom, j.onRender);
    const uint64_t t3 = __rdtsc();
    for (const Job& j : g.jobs) VCall<void>(j.instance, kReleaseSlot);
    g.jobs.clear();
    ++g.generation;
    g.flushing = false;
    const uint64_t t4 = __rdtsc();
    ++s.flushes;
    s.fkCycles2 += t1 - t0;
    s.waitCycles += t2 - t1;
    s.postCycles2 += t3 - t2;
    s.flushCycles += t4 - t0;
    s.longest = (std::max)(s.longest, t4 - t0);
    if (t4 - t0 > uint64_t(6.0e6)) ++s.over2ms;
}

// --- the hooks: the main thread's calls -------------------------------------------------------------------------------
void PreHook(char* self, const float* phys, const float* loc, const void* camera, uint32_t onRender) {
    if (!g.window || g.flushing || GetCurrentThreadId() != g.mainThread) return g.pre(self, phys, loc, camera, onRender);
    Stats& s = g.st;
    ++s.preCalls;
    if (g.defer) {
        if (!ChosenCaller(_ReturnAddress()))
            ++s.otherCaller;
        else if (self == g.localCharacter)
            ++s.localPlayer;
        else
            g.deferFk = true;
    }
    const uint64_t t0 = __rdtsc();
    g.pre(self, phys, loc, camera, onRender);
    s.preCycles += __rdtsc() - t0;
    g.deferFk = false;
}

void PostHook(char* self, const float* phys, const float* loc, void* attachment, float zoom, uint32_t onRender) {
    if (!g.window || g.flushing || GetCurrentThreadId() != g.mainThread)
        return g.post(self, phys, loc, attachment, zoom, onRender);
    Stats& s = g.st;
    ++s.postCalls;
    Job* j = nullptr;
    if (g.defer) {
        j = FindJob(self);
        if (!attachment && ChosenCaller(_ReturnAddress()) && self != g.localCharacter) {
            if (!j) j = NewJob(self);
            if (j) {
                j->post = true;
                j->zoom = zoom;
                j->onRender = onRender;
                std::memcpy(j->postPhys, phys, sizeof j->postPhys);
                std::memcpy(j->postLoc, loc, sizeof j->postLoc);
                ++s.postWaited;
                return;
            }
            ++s.tooMany;
        }
        if (j && j->anim) {  // (a post-process made in place: its forward kinematics comes first)
            RunFk(*j);
            j->anim = nullptr;
            ++s.fkMadeFirst;
        }
    }
    const uint64_t t0 = __rdtsc();
    g.post(self, phys, loc, attachment, zoom, onRender);
    s.postCycles += __rdtsc() - t0;
}

// The animation's update resets the root motion, for forward kinematics to set: when that waits, the game's readers
// right after get the last step's
void AnimUpdateHook(char* anim) {
    if (!g.deferFk || GetCurrentThreadId() != g.mainThread) return g.animUpdate(anim);
    std::memcpy(g.rootSaved, anim + kRootMotion, kRootMotionBytes);
    g.animUpdate(anim);
}

void FkHook(char* anim, const float* phys, const float* loc) {
    if (!g.window || g.flushing || GetCurrentThreadId() != g.mainThread) return g.fk(anim, phys, loc);
    Stats& s = g.st;
    if (g.deferFk) {
        char* const instance = *reinterpret_cast<char**>(anim + kAnimOwner);
        Job* j = FindJob(instance);
        if (!j) j = NewJob(instance);
        if (j) {
            j->anim = anim;
            std::memcpy(j->fkPhys, phys, sizeof j->fkPhys);
            std::memcpy(j->fkLoc, loc, sizeof j->fkLoc);
            std::memcpy(anim + kRootMotion, g.rootSaved, kRootMotionBytes);
            ++s.fkWaited;
            return;
        }
        ++s.tooMany;
    }
    ++s.fkCalls;
    const uint64_t t0 = __rdtsc();
    g.fk(anim, phys, loc);
    s.fkCycles += __rdtsc() - t0;
}

void EntityUpdateHook(void* self) {
    if (GetCurrentThreadId() != g.mainThread) return g.entityUpdate(self);
    const uint64_t t0 = __rdtsc();
    if (g.defer) {
        g.localCharacter = LocalCharacter(self);
        g.st.localFound += g.localCharacter != nullptr;
    }
    g.window = true;
    g.entityUpdate(self);
    g.window = false;
    Flush();
    g.st.updateCycles += __rdtsc() - t0;
    ++g.st.updates;
}

void DeleteEntityHook(void* self, void* entity) {
    if (GetCurrentThreadId() == g.mainThread && !g.jobs.empty() && !g.flushing) {
        ++g.st.deletions;
        Flush();
    }
    g.deleteEntity(self, entity);
}

// --- the hooks: what a batch's threads share --------------------------------------------------------------------------
// Decodes through a copy of the controller's header, its cache never matching: nothing shared is written
float* DecodeKeyHook(char* self, float* out, int animation, float time) {
    if (!t_batch) return g.decodeKey(self, out, animation, time);
    alignas(16) char copy[kDecodeBytes];
    std::memcpy(copy, self, kDecodeHeader);
    std::memcpy(copy + kDecodeHeader, &kNoTime, sizeof kNoTime);
    return g.decodeKey(copy, out, animation, time);
}

// A track's last-found key: the same, through a copy of the track (with its cache as it is, a thread reads a key another
// has yet to write: an index out of its keys)
template <int I>
uint32_t TrackSearchHook(char* track, int animation, float time, float* difference) {
    if (!t_batch) return g.trackSearch[I](track, animation, time, difference);
    alignas(16) char copy[kTrackBytes];
    std::memcpy(copy, track, sizeof copy);
    std::memcpy(copy, &kNoTime, sizeof kNoTime);
    return g.trackSearch[I](copy, animation, time, difference);
}

template <int I>
void* TrackSampleHook(char* track, float* out, int animation, float time) {
    if (!t_batch) return g.trackSample[I](track, out, animation, time);
    alignas(16) char copy[kTrackBytes];
    std::memcpy(copy, track, sizeof copy);
    std::memcpy(copy, &kNoTime, sizeof kNoTime);
    return g.trackSample[I](copy, out, animation, time);
}

template <int I>
uint32_t InfoSearchHook(char* self, int animation, float time, float* difference) {
    if (!t_batch) return g.infoSearch[I](self, animation, time, difference);
    alignas(16) char copy[kInfoBytes];
    std::memcpy(copy, self, sizeof copy);
    std::memcpy(copy + kInfoTime, &kNoTime, sizeof kNoTime);
    return g.infoSearch[I](copy, animation, time, difference);
}

bool AnimationDriven(const char* pose) {
    return (static_cast<uint8_t>((*reinterpret_cast<char* const*>(pose + kPoseAnim))[kAnimFlags]) & 1) != 0;
}

void* Sampler6Hook(char* pose, void* out, void* a, void* b, void* c, float d) {
    if (!t_batch || !AnimationDriven(pose)) return g.sampler6(pose, out, a, b, c, d);
    LockEnter();
    ++g.st.lockedSamplers;
    void* const r = g.sampler6(pose, out, a, b, c, d);
    LeaveCriticalSection(&g.lock);
    return r;
}

void* Sampler8Hook(char* pose, void* out, void* a, void* b, void* c, void* d, void* e, float f) {
    if (!t_batch || !AnimationDriven(pose)) return g.sampler8(pose, out, a, b, c, d, e, f);
    LockEnter();
    ++g.st.lockedSamplers;
    void* const r = g.sampler8(pose, out, a, b, c, d, e, f);
    LeaveCriticalSection(&g.lock);
    return r;
}

void SplineVecHook(void* spline, float time, float* out) {
    if (!t_batch) return g.splineVec(spline, time, out);
    LockEnter();
    ++g.st.lockedSplines;
    g.splineVec(spline, time, out);
    LeaveCriticalSection(&g.lock);
}

void SplineQuatHook(void* spline, float time, float* out) {
    if (!t_batch) return g.splineQuat(spline, time, out);
    LockEnter();
    ++g.st.lockedSplines;
    g.splineQuat(spline, time, out);
    LeaveCriticalSection(&g.lock);
}

void AddFrameProfilerHook(void* self, void* profiler) {
    if (!t_batch) return g.addFrameProfiler(self, profiler);
    LockEnter();
    ++g.st.lockedProfilers;
    g.addFrameProfiler(self, profiler);
    LeaveCriticalSection(&g.lock);
}

void* NameLookupHook(void* map, const char* name) {
    if (!t_batch) return g.nameLookup(map, name);
    LockEnter();
    ++g.st.lockedLookups;
    void* const r = g.nameLookup(map, name);
    LeaveCriticalSection(&g.lock);
    return r;
}

// (its arguments as they came, the stack's raw: what a format reads of them)
void WarningHook(void* file, const char* format, void* a, void* b, void* c, void* d, void* e, void* f, void* h, void* i) {
    if (!t_batch) return g.warning(file, format, a, b, c, d, e, f, h, i);
    LockEnter();
    ++g.st.lockedLookups;
    g.warning(file, format, a, b, c, d, e, f, h, i);
    LeaveCriticalSection(&g.lock);
}

// --- setup ------------------------------------------------------------------------------------------------------------
int WorkerCount() {
    const std::wstring v = ConfigString(L"AnimUpdate", L"Workers", L"OPTIPERF_ANIM_UPDATE_WORKERS", L"auto");
    if (!v.empty() && _wcsicmp(v.c_str(), L"auto") != 0)
        return (std::max)(1, (std::min)(kMaxWorkers, int(wcstol(v.c_str(), nullptr, 10))));
    const unsigned long long others = FlushThread_OthersMask();
    DWORD_PTR process = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    const int k = others ? int(__popcnt64(others)) - 1 : int(__popcnt64(process)) / 2 - 1;
    return (std::max)(1, (std::min)(4, k));
}

// At the second flush, on the main thread: everything located and checked, then put in.
void Setup() {
    g.setupTried = true;
    g.mainThread = GetCurrentThreadId();
    g.anim = reinterpret_cast<char*>(GetModuleHandleW(L"CryAnimation.dll"));
    g.action = reinterpret_cast<char*>(GetModuleHandleW(L"CryAction.dll"));
    g.entity = reinterpret_cast<char*>(GetModuleHandleW(L"CryEntitySystem.dll"));
    g.animSize = ImageSize(g.anim);
    g.actionSize = ImageSize(g.action);
    g.entitySize = ImageSize(g.entity);
    char* const sys = reinterpret_cast<char*>(GetModuleHandleW(L"CrySystem.dll"));
    const char* why = g.anim && g.action && g.entity && sys ? nullptr : "a module isn't loaded";
    char *step = nullptr, *fk = nullptr, *update = nullptr, *decode = nullptr, *sampler6 = nullptr, *sampler8 = nullptr;
    char *splineVec = nullptr, *splineQuat = nullptr, *deleteEntity = nullptr, *pre = nullptr, *post = nullptr;
    char *entityUpdate = nullptr, *addProfiler = nullptr, *nameLookup = nullptr, *warning = nullptr;
    const char* const searchPatterns[kTrackSearches] = {kTrackSearch0, kTrackSearch1, kTrackSearch2};
    const char* const samplePatterns[kTrackSamples] = {kTrackSample0, kTrackSample1, kTrackSample2, kTrackSample3, kTrackSample4};
    const char* const infoPatterns[kInfoSearches] = {kInfoSearch0, kInfoSearch1, kInfoSearch2, kInfoSearch3};
    char *searches[kTrackSearches] = {}, *samples[kTrackSamples] = {}, *infos[kInfoSearches] = {};
    if (!why && !(step = Unique(g.anim, kStep))) why = "CSkeletonAnim's step not found";
    if (!why) {
        update = RelTarget(step + kStepUpdateCall);
        fk = RelTarget(step + kStepFkCall);
        if (!IsInCode(g.anim, update, 16) || !IsInCode(g.anim, fk, 16)) why = "the step's calls aren't as expected";
    }
    if (!why) {
        const auto hits = FindInRange(fk, kFkEnvSearch, ParsePattern(kFkEnvLoad), 1);
        if (hits.empty()) {
            why = "forward kinematics' profiler check not found";
        } else {
            g.genv = reinterpret_cast<char**>(hits[0] + 7 + int32_t(ReadU32(hits[0] + 3)));
            if (!IsInImage(g.anim, reinterpret_cast<char*>(g.genv), 8) || !*g.genv) why = "the animation's gEnv not found";
        }
    }
    if (!why && !(decode = Unique(g.anim, kDecodeKey))) why = "CControllerPQLog::DecodeKey not found";
    if (!why && !(sampler6 = Unique(g.anim, kSampler6))) why = "the first sampler not found";
    if (!why && !(sampler8 = Unique(g.anim, kSampler8))) why = "the second sampler not found";
    if (!why && !(splineVec = Unique(g.anim, kSplineVec))) why = "the vector spline's evaluation not found";
    if (!why && !(splineQuat = Unique(g.anim, kSplineQuat))) why = "the rotation spline's evaluation not found";
    if (!why && !(deleteEntity = Unique(g.entity, kDeleteEntity))) why = "the entity system's deletion not found";
    for (int i = 0; !why && i < kTrackSearches; ++i)
        if (!(searches[i] = Unique(g.anim, searchPatterns[i]))) why = "a track's key search not found";
    for (int i = 0; !why && i < kTrackSamples; ++i)
        if (!(samples[i] = Unique(g.anim, samplePatterns[i]))) why = "a track's sample not found";
    for (int i = 0; !why && i < kInfoSearches; ++i)
        if (!(infos[i] = Unique(g.anim, infoPatterns[i]))) why = "a key-time class's search not found";
    if (!why && !(nameLookup = Unique(g.anim, kNameLookup))) why = "the name lookup not found";
    if (!why && !(warning = Unique(g.anim, kWarning))) why = "the animation's warnings not found";
    if (!why) {
        void** const vt = FindRttiVtable(g.anim, ".?AVCCharInstance@@");
        if (!vt) {
            why = "CCharInstance's vtable not found";
        } else {
            pre = static_cast<char*>(CallTrace_Original(vt[kPreSlot]));
            post = static_cast<char*>(CallTrace_Original(vt[kPostSlot]));
            if (!MatchesAt(pre, ParsePattern(kPre)) || !MatchesAt(post, ParsePattern(kPost)))
                why = "the pre- or post-process isn't as expected";
        }
    }
    if (!why) {
        void** const vt = FindRttiVtable(g.entity, ".?AVCEntitySystem@@");
        entityUpdate = vt ? static_cast<char*>(CallTrace_Original(vt[kEsUpdateSlot])) : nullptr;
        if (!entityUpdate || !IsInCode(g.entity, entityUpdate, 16)) why = "CEntitySystem's vtable not found";
    }
    if (!why) {
        void** const vt = FindRttiVtable(sys, ".?AVCFrameProfileSystem@@");
        addProfiler = vt ? static_cast<char*>(CallTrace_Original(vt[kAddFrameProfilerSlot])) : nullptr;
        if (!addProfiler || !IsInCode(sys, addProfiler, 16)) why = "CFrameProfileSystem's vtable not found";
    }
    if (why) {
        Log("anim update: off: %s", why);
        return;
    }
    if (g.mode == AMode::Time) {  // (the game's own times, nothing else)
        HookSpec hooks[] = {
            {"SkeletonPreProcess", pre, reinterpret_cast<void*>(&PreHook), reinterpret_cast<void**>(&g.pre)},
            {"SkeletonPostProcess", post, reinterpret_cast<void*>(&PostHook), reinterpret_cast<void**>(&g.post)},
            {"forward kinematics", fk, reinterpret_cast<void*>(&FkHook), reinterpret_cast<void**>(&g.fk)},
            {"CEntitySystem::Update", entityUpdate, reinterpret_cast<void*>(&EntityUpdateHook),
             reinterpret_cast<void**>(&g.entityUpdate)},
        };
        if (!InstallHooks(hooks, 4)) {
            Log("anim update: off: could not write the hooks");
            return;
        }
        g.ready = true;
        Log("anim update: mode=time: the entity update +0x%X, the pre- and post-process, forward kinematics +0x%X timed",
            unsigned(entityUpdate - g.entity), unsigned(fk - g.anim));
        return;
    }
    InitializeCriticalSectionAndSpinCount(&g.lock, 4000);
    g.jobs.reserve(kMaxJobs);
    if (g.mode == AMode::On || g.mode == AMode::Verify) {
        const int n = WorkerCount();
        const unsigned long long others = FlushThread_OthersMask();
        for (int i = 0; i < n; ++i) {
            Worker& w = g.workers[i];
            w.go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            DWORD id = 0;
            w.thread = w.go ? CreateThread(nullptr, kWorkerStack, &WorkerMain, &w, STACK_SIZE_PARAM_IS_A_RESERVATION, &id)
                            : nullptr;
            if (!w.thread) break;
            if (others) SetThreadAffinityMask(w.thread, DWORD_PTR(others));
            using SetDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
            if (auto set = reinterpret_cast<SetDescriptionFn>(
                    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))) {
                wchar_t name[48];
                swprintf(name, 48, L"optiperf animation %d", i + 1);
                set(w.thread, name);
            }
            g.workerCount = i + 1;
        }
        if (g.workerCount == 0) {
            Log("anim update: off: no worker thread could be started (error %lu)", GetLastError());
            return;
        }
    }
    g.defer = true;
    HookSpec hooks[] = {
        {"SkeletonPreProcess", pre, reinterpret_cast<void*>(&PreHook), reinterpret_cast<void**>(&g.pre)},
        {"SkeletonPostProcess", post, reinterpret_cast<void*>(&PostHook), reinterpret_cast<void**>(&g.post)},
        {"the animation's update", update, reinterpret_cast<void*>(&AnimUpdateHook), reinterpret_cast<void**>(&g.animUpdate)},
        {"forward kinematics", fk, reinterpret_cast<void*>(&FkHook), reinterpret_cast<void**>(&g.fk)},
        {"CEntitySystem::Update", entityUpdate, reinterpret_cast<void*>(&EntityUpdateHook),
         reinterpret_cast<void**>(&g.entityUpdate)},
        {"the entity deletion", deleteEntity, reinterpret_cast<void*>(&DeleteEntityHook),
         reinterpret_cast<void**>(&g.deleteEntity)},
        {"CControllerPQLog::DecodeKey", decode, reinterpret_cast<void*>(&DecodeKeyHook), reinterpret_cast<void**>(&g.decodeKey)},
        {"the first sampler", sampler6, reinterpret_cast<void*>(&Sampler6Hook), reinterpret_cast<void**>(&g.sampler6)},
        {"the second sampler", sampler8, reinterpret_cast<void*>(&Sampler8Hook), reinterpret_cast<void**>(&g.sampler8)},
        {"the vector spline", splineVec, reinterpret_cast<void*>(&SplineVecHook), reinterpret_cast<void**>(&g.splineVec)},
        {"the rotation spline", splineQuat, reinterpret_cast<void*>(&SplineQuatHook), reinterpret_cast<void**>(&g.splineQuat)},
        {"AddFrameProfiler", addProfiler, reinterpret_cast<void*>(&AddFrameProfilerHook),
         reinterpret_cast<void**>(&g.addFrameProfiler)},
        {"the name lookup", nameLookup, reinterpret_cast<void*>(&NameLookupHook), reinterpret_cast<void**>(&g.nameLookup)},
        {"the animation's warnings", warning, reinterpret_cast<void*>(&WarningHook), reinterpret_cast<void**>(&g.warning)},
        {"a track's search", searches[0], reinterpret_cast<void*>(&TrackSearchHook<0>), reinterpret_cast<void**>(&g.trackSearch[0])},
        {"a track's search", searches[1], reinterpret_cast<void*>(&TrackSearchHook<1>), reinterpret_cast<void**>(&g.trackSearch[1])},
        {"a track's search", searches[2], reinterpret_cast<void*>(&TrackSearchHook<2>), reinterpret_cast<void**>(&g.trackSearch[2])},
        {"a track's sample", samples[0], reinterpret_cast<void*>(&TrackSampleHook<0>), reinterpret_cast<void**>(&g.trackSample[0])},
        {"a track's sample", samples[1], reinterpret_cast<void*>(&TrackSampleHook<1>), reinterpret_cast<void**>(&g.trackSample[1])},
        {"a track's sample", samples[2], reinterpret_cast<void*>(&TrackSampleHook<2>), reinterpret_cast<void**>(&g.trackSample[2])},
        {"a track's sample", samples[3], reinterpret_cast<void*>(&TrackSampleHook<3>), reinterpret_cast<void**>(&g.trackSample[3])},
        {"a track's sample", samples[4], reinterpret_cast<void*>(&TrackSampleHook<4>), reinterpret_cast<void**>(&g.trackSample[4])},
        {"a key-time class's search", infos[0], reinterpret_cast<void*>(&InfoSearchHook<0>), reinterpret_cast<void**>(&g.infoSearch[0])},
        {"a key-time class's search", infos[1], reinterpret_cast<void*>(&InfoSearchHook<1>), reinterpret_cast<void**>(&g.infoSearch[1])},
        {"a key-time class's search", infos[2], reinterpret_cast<void*>(&InfoSearchHook<2>), reinterpret_cast<void**>(&g.infoSearch[2])},
        {"a key-time class's search", infos[3], reinterpret_cast<void*>(&InfoSearchHook<3>), reinterpret_cast<void**>(&g.infoSearch[3])},
    };
    if (!InstallHooks(hooks, sizeof hooks / sizeof hooks[0])) {
        g.defer = false;
        Log("anim update: off: could not write the hooks");
        return;
    }
    g.ready = true;
    Log("anim update: mode=%s, %d worker%s%s: the step +0x%X, the animation's update +0x%X, forward kinematics +0x%X, the "
        "pre-process +0x%X, the post-process +0x%X; the entity update +0x%X, the entity deletion +0x%X; shared: "
        "DecodeKey +0x%X, the samplers +0x%X, +0x%X, the splines +0x%X, +0x%X",
        ModeName(g.mode), g.workerCount, g.workerCount == 1 ? "" : "s",
        FlushThread_OthersMask() ? " (on the other threads' processors)" : "", unsigned(step - g.anim),
        unsigned(update - g.anim), unsigned(fk - g.anim), unsigned(pre - g.anim), unsigned(post - g.anim),
        unsigned(entityUpdate - g.entity), unsigned(deleteEntity - g.entity), unsigned(decode - g.anim),
        unsigned(sampler6 - g.anim), unsigned(sampler8 - g.anim), unsigned(splineVec - g.anim),
        unsigned(splineQuat - g.anim));
}
#endif  // _WIN64

}  // namespace

void AnimUpdate_Install(const wchar_t* moduleName) {
#ifndef _WIN64
    if (ReadMode() != AMode::Off) Log("%ls: anim update off: 64-bit game only", moduleName);
#else
    if (g.installed) return;
    g.mode = ReadMode();
    if (g.mode == AMode::Off) return;
    g.installed = true;
    Log("%ls: anim update: mode=%s (set up at the second flush)", moduleName, ModeName(g.mode));
#endif
}

void AnimUpdate_FlushEntry() {
#ifdef _WIN64
    if (!g.installed || g.setupTried) return;
    if (++g.flushCount >= 2) Setup();
#endif
}

void AnimUpdate_Report(const char* when) {
#ifdef _WIN64
    if (!g.installed || !g.ready) return;
    const Stats& s = g.st;
    const double u = double(s.updates ? s.updates : 1);
    Log("anim update [%s]: mode=%s: the entity update %.3f ms a frame (%llu frames); made in place: pre-process %.3f ms "
        "(%.1f calls), post-process %.3f ms (%.1f), forward kinematics %.3f ms (%.1f)",
        when, ModeName(g.mode), Ms(s.updateCycles) / u, static_cast<unsigned long long>(s.updates), Ms(s.preCycles) / u,
        double(s.preCalls) / u, Ms(s.postCycles) / u, double(s.postCalls) / u, Ms(s.fkCycles) / u, double(s.fkCalls) / u);
    if (g.mode == AMode::Time) return;
    uint64_t workCycles = 0, workerFks = 0;
    for (int i = 0; i < g.workerCount; ++i) {
        workCycles += g.workers[i].cycles;
        workerFks += g.workers[i].fks;
    }
    const double f = double(s.flushes ? s.flushes : 1);
    Log("anim update [%s]: waited a frame: forward kinematics %.1f, post-processes %.1f; made in place: other callers "
        "%llu, the local player %llu (a character of its in %llu frames), too many %llu; forward kinematics made first %llu | flushes %llu (%.1f a frame, "
        "before a deletion %llu), batches %llu; on the main thread alone: first flushes %llu, few %llu, profiler %llu | a "
        "flush %.3f ms: forward kinematics %.3f ms (the workers' own %.3f ms a batch, the main thread's share %.0f %%), "
        "its wait %.3f ms, post-processes %.3f ms; longest %.3f ms, over 2 ms %llu | locked: samplers %llu, splines %llu, "
        "profiler labels %llu, name lookups and warnings %llu, waited for %llu times (%.3f ms in all)",
        when, double(s.fkWaited) / u, double(s.postWaited) / u, static_cast<unsigned long long>(s.otherCaller),
        static_cast<unsigned long long>(s.localPlayer), static_cast<unsigned long long>(s.localFound),
        static_cast<unsigned long long>(s.tooMany),
        static_cast<unsigned long long>(s.fkMadeFirst), static_cast<unsigned long long>(s.flushes), double(s.flushes) / u,
        static_cast<unsigned long long>(s.deletions), static_cast<unsigned long long>(s.batches),
        static_cast<unsigned long long>(s.soloWarmup), static_cast<unsigned long long>(s.soloFew),
        static_cast<unsigned long long>(s.soloProfiler), Ms(s.flushCycles) / f, Ms(s.fkCycles2) / f,
        Ms(workCycles) / double(s.batches ? s.batches : 1),
        s.mainFks + workerFks ? 100.0 * double(s.mainFks) / double(s.mainFks + workerFks) : 0.0, Ms(s.waitCycles) / f,
        Ms(s.postCycles2) / f, Ms(s.longest), static_cast<unsigned long long>(s.over2ms),
        static_cast<unsigned long long>(s.lockedSamplers), static_cast<unsigned long long>(s.lockedSplines),
        static_cast<unsigned long long>(s.lockedProfilers), static_cast<unsigned long long>(s.lockedLookups),
        static_cast<unsigned long long>(s.contended),
        Ms(s.lockWaitCycles));
    if (g.mode == AMode::Verify)
        Log("anim update [%s]: verify: forward kinematics made again on the main thread: of a batch's %llu, differing %llu; of the "
            "main thread's own (the control) %llu, differing %llu",
            when, static_cast<unsigned long long>(s.verifyBatch), static_cast<unsigned long long>(s.verifyBatchDiffer),
            static_cast<unsigned long long>(s.verifySolo), static_cast<unsigned long long>(s.verifySoloDiffer));
#else
    (void)when;
#endif
}

}  // namespace optiperf
