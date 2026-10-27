// Particles before the flush: Phase 2 of research/docs/cpu-multithreading-plan.md, after the environment snapshot
// (details in research/docs/particles.md). x64 CryRenderD3D10.dll only.
//
// While drawing, the flush prepares each particle render element (CREParticle::mfPrepare). It makes an allocator
// (CAllocRender, which reserves room in the renderer's particle batch, drawing the batch first when the request
// doesn't fit) and a context (the renderer's camera and two flags), and calls the element's vertex creator, a particle
// container of the 3D engine. Its SetVertices brings the container up to the frame's time, queues it for the particle
// thread's next update, adds fill lights for glowing particles near the camera, and writes its vertices and indices
// through the allocator (copying what the particle thread computed, or computing them). That is 3D-engine work inside
// the flush: about 150 calls per frame, with their timer reads and the allocator's calls back into the renderer.
//   - on: right before the flush, each particle element of the main pass's render lists (level 1) gets its
//     SetVertices call there, with the context mfPrepare would make, a recording allocator that grants what the
//     renderer's would (min(request, buffer size), which depends only on the requests), and the renderer's fill-light
//     slot recorded instead of added (a counting copy of its vtable, vtable_counter.h). In the flush, mfPrepare runs as
//     in vanilla, with the element's vertex creator swapped for a replayer. If the context is the recorded one, the
//     replayer adds the recorded fill lights and makes the recorded allocator calls on the real allocator, copying the
//     recorded vertices and indices (rebased on the batch position it gets) into each grant (particle_record.h).
//     An element prepared again in the same flush (a shadow-receiving particle: the deferred shadow pass prepares it
//     first, then the main pass) gets the same replay again. What each SetVertices call also does to the container
//     (when it computes the vertices itself, each particle's share added to three estimates, which the per-particle
//     vertex function, hooked while the pre-runs run, records exactly: ParticleVertsHook; and when it ends normally,
//     the per-frame counters: ApplyRenderCounter, the game's single-precision code) is applied right after the flush,
//     on the main thread, once per repeated prepare (ApplyRepeats): in the flush the replay reads and writes only its
//     records and the renderer, as a render thread needs. If the counters changed between the pre-run and then (the
//     particle thread computing the vertices meanwhile), the updates are applied to them all the same, and counted.
//     Without recorded shares a repeated prepare calls SetVertices as in vanilla, and so does an element prepared in
//     another context than its pre-run's, or one not pre-run. A reflection's
//     particle elements (its level 2, which the reflections' dry run takes into a capture) are pre-run where the dry
//     run takes the level: with the reflection's camera, inside its traversal, as vanilla's calls are made; they are
//     replayed when the flush replays the reflection.
//   - verify: as on, and at each replay SetVertices also runs at the flush's call, with a recording allocator and its
//     per-frame counters put back afterwards, and the two records are compared; for a repeated prepare, also the
//     counters vanilla's call leaves against those the shares and ApplyRenderCounter give, both from where the
//     element's earlier prepares in the flush, with their updates, leave them.
//
// [ShadowPrepass] Particles = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_PARTICLES)

#include "particle_prepass.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

#include "allocator.h"
#include "calltrace.h"
#include "config.h"
#include "diag.h"
#include "flush_thread.h"
#include "hooks.h"
#include "log.h"
#include "particle_record.h"
#include "scan.h"
#include "shadow_prepass.h"
#include "shadow_prepass_lists.h"
#include "vtable_counter.h"

namespace optiperf {
namespace {

enum class PMode { Off, On, Verify };

PMode ReadMode() {
    const std::wstring v = ConfigString(L"ShadowPrepass", L"Particles", L"OPTIPERF_SHADOW_PREPASS_PARTICLES", L"off");
    if (_wcsicmp(v.c_str(), L"on") == 0 || v == L"1") return PMode::On;
    if (_wcsicmp(v.c_str(), L"verify") == 0) return PMode::Verify;
    return PMode::Off;
}

#ifdef _WIN64
const char* ModeName(PMode m) { return m == PMode::On ? "on" : m == PMode::Verify ? "verify" : "off"; }

// --- patterns (x64 CryRenderD3D10.dll, build 6156) --------------------------------------------------------------------
// CREParticle::mfPrepare (element): the batch's per-frame reset; then the allocator on its stack (CAllocRender's vtable,
// the element) and the context (GetCamera, slot 44; !(GetFeatures, slot 7, bit 29); the element's flags +0xC, bit 11);
// the element's vertex creator (+0x30) slot 0 called with them; then the batch's current element
constexpr const char kPrepare[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B F1 48 8B 0D ?? ?? ?? ?? 8B 81 88 3E 01 00 39 "
    "81 8C 3E 01 00 74 61 48 63 91 0C 18 00 00 89 81 8C 3E 01 00 4C 8D 05 ?? ?? ?? ?? 41 8B 84 90 ?? "
    "?? ?? ?? 89 81 00 18 00 00 41 8B 84 D0 ?? ?? ?? ?? 89 81 08 18 00 00 41 8B 84 D0 ?? ?? ?? ?? 89 "
    "81 04 18 00 00 48 8B 81 F0 17 00 00 48 89 81 F8 17 00 00 48 8B 81 E0 17 00 00 48 89 81 E8 17 00 "
    "00 48 8B 0D ?? ?? ?? ?? 8B 7E 0C 48 8D 05 ?? ?? ?? ?? 48 89 74 24 38 48 89 44 24 30 48 8B 01 C1 "
    "EF 0B 40 80 E7 01 FF 50 38 48 8B 0D ?? ?? ?? ?? 48 8B 11 8B D8 C1 EB 1D F6 D3 80 E3 01 FF 92 60 "
    "01 00 00 48 8B 4E 30 88 5C 24 28 40 88 7C 24 29 48 89 44 24 20 48 8B 01 4C 8D 44 24 20 48 8D 54 "
    "24 30 FF 10 4C 8B 1D ?? ?? ?? ?? 41 83 BB 2C 29 01 00 00 74 07 49 89 B3 88 0A 00 00 48 8B 5C 24 "
    "50 48 8B 74 24 58 48 83 C4 40 5F C3";
constexpr int kPrepareAllocVt = 0x8E;  // lea rax, [CAllocRender's vtable]
// CAllocRender::Alloc (allocator, verts, nVerts, nInds): the batch's pointers and counts advanced by what was written;
// then, for nVerts, the reservation and the grant's pointers (+0x17F8; +0x128E0 + 2 x index count) and base
constexpr const char kAlloc[] =
    "48 89 5C 24 08 57 48 83 EC 30 48 63 02 48 8B 1D ?? ?? ?? ?? 48 8B FA 48 83 C2 04 45 8B D9 45 8B "
    "D0 48 6B C0 2C 48 01 83 F8 17 00 00 8B 42 FC 01 83 2C 29 01 00 8B 02 01 83 28 29 01 00 45 85 C0 "
    "74 4A 4C 8B 49 08 48 8B 0D ?? ?? ?? ?? 48 89 54 24 28 45 8B C3 41 8B D2 48 89 7C 24 20 E8 ?? ?? "
    "?? ?? 4C 8B 9B F8 17 00 00 4C 89 5F 08 48 8B 83 E0 28 01 00 48 63 8B 28 29 01 00 48 8D 0C 48 48 "
    "89 4F 10 8B 83 2C 29 01 00 89 47 18 48 8B 5C 24 40 48 83 C4 30 5F C3";
constexpr int kAllocReserve = 0x5D;
// the reservation (renderer, nVerts, nInds, element, &grantV, &grantI): grants the requests; unless they fit in the
// current batch (and none is open), draws it (the render callback +0xB28) and cuts them to the buffer sizes (+0x14018
// vertices, 3 x +0x1401C indices). particle_record.h's Grant is this rule.
constexpr const char kReserve[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 48 83 EC 30 48 8B 6C 24 70 4D 8B E9 "
    "45 8B E0 48 85 ED 8B F2 48 8B D9 74 03 89 55 00 48 8B 7C 24 78 48 85 FF 74 03 44 89 07 48 83 B9 "
    "88 0A 00 00 00 75 2C 8B 81 2C 29 01 00 8D 0C 02 3B 8B 18 40 01 00 7D 1B 8B 83 28 29 01 00 41 8D "
    "14 00 8B 83 1C 40 01 00 8D 0C 40 3B D1 0F 8C ?? ?? ?? ?? FF 93 28 0B 00 00 44 8B 9B 18 40 01 00 "
    "41 3B F3 7C 04 44 89 5D 00 8B 83 1C 40 01 00 8D 0C 40 44 3B E1 7C 02 89 0F";
// EF_ADDFillLight (renderer, light): 24 bytes into the current level's fill lights (+0x17A0 + level x 16, at most 0x200)
constexpr const char kAddFillLight[] =
    "48 83 EC 48 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 48 89 5C 24 50 48 63 1D ?? ?? ?? ?? 48 8B C3 "
    "48 03 C0 81 BC C1 A8 17 00 00 00 02 00 00 0F 83 ?? ?? ?? ?? 8B 02 4C 8D 44 24 20 48 81 C3 7A 01 "
    "00 00 41 89 00";
// x64 Cry3DEngine.dll: CParticleContainer::SetVertices (its IParticleVertexCreator at +0x10, allocator, context): the
// container's update (UpdateParticles), then nothing for an empty container (+0x30: the particles, count before them)
constexpr const char kSetVertices[] =
    "48 89 54 24 10 41 54 41 55 41 57 48 81 EC F0 00 00 00 4C 8D 61 F0 4C 8B E9 4D 8B F8 49 8B CC E8 "
    "?? ?? ?? ?? 49 8B 45 30 83 78 FC 00 75 10 B0 01 48 81 C4 F0 00 00 00 41 5F 41 5D 41 5C C3";

// offsets fixed in the patterns above
constexpr int kElemFlags = 0x0C, kElemCreator = 0x30, kElemFlagBit = 11;
constexpr int kRMaxVerts = 0x14018, kRMaxInds = 0x1401C;  // the particle batch's buffer sizes (indices: 3 x)
constexpr int kFeatureBit = 29;
constexpr int kVfGetFeatures = 7, kVfGetCamera = 44, kVfAddFillLight = 119;  // IRenderer slots (Mod SDK)
constexpr size_t kCamBytes = 0x214;  // CCamera
// SetVertices' writes to its vertex creator, which verify puts back after its own call: three estimates (+0xD8,
// +0xF8, +0xFC) it scales by the renders this frame (+0x108), the vertices-set flag (+0x11E); the container's lock
// (+0x110), which SetVertices and the particle thread take too
constexpr int kVcFloats[] = {0xD8, 0xF8, 0xFC, 0x108};
constexpr int kVcSetFlag = 0x11E, kVcLock = 0x110;
// whether the particle thread computed the container's vertices (+0x11D, which SetVertices copies then; its update
// clears it when the particles moved): read after a call, it tells the path the call took
constexpr int kVcComputed = 0x11D;
// The per-particle vertex function SetVertices calls when it computes the vertices itself, and the particle thread's
// ComputeVertices too (Cry3DEngine +0x83750: particle, vertices, vertex context). It adds the particle's share to three
// of the container's estimates through the context's pointer at +0x48 (the container's +0xE0, the vertex creator's
// +0xD0): every particle's to +0x28, and for one it draws 1 to +0x8 and its share to +0x2C, each a single addition.
constexpr const char kParticleVerts[] =
    "40 53 55 57 48 81 EC 40 01 00 00 F3 0F 10 81 80 00 00 00 49 8B D8 48 8B EA 0F 2F 01 48 8B F9 73 "
    "22 48 8B 81 98 00 00 00 4C 8B 48 18 41 80 B9 A9 00 00 00 00 75 0D 33 C0 48 81 C4 40 01 00 00 5F "
    "5D 5B C3 F3 41 0F 10 40 2C F3 41 0F 10 58 28 4D 8B 00 48 8D 54 24 40 44 0F 29 8C 24 00 01 00 00 "
    "44 0F 29 BC 24 A0 00 00 00 F3 0F 11 44 24 20 E8 ?? ?? ?? ?? 48 8B 43 48 F3 0F 10 4C 24 54 F3 0F "
    "10 5C 24 58 45 0F 57 C9 0F 28 C3 0F 28 D1 F3 0F 59 43 34 F3 44 0F 10 3D ?? ?? ?? ?? F3 0F 58 40 "
    "28 F3 0F 11 40 28 F3 0F 59 53 40 0F 28 C1 F3 0F 59 43 44 F3 0F 58 53 3C F3 0F 59 C1 F3 0F 58 D0 "
    "44 0F 2F CA 76 06 41 0F 28 D1 EB 0A 44 0F 2F FA 77 04 41 0F 28 D7 F3 0F 10 4C 24 4C F3 0F 10 2D "
    "?? ?? ?? ?? F3 0F 59 CA 0F 2F E9 77 38 F3 0F 10 25 ?? ?? ?? ?? 0F 28 D3 F3 0F 59 53 30 F3 0F 58 "
    "53 38 0F 2F D4 F3 0F 11 53 38 76 20 41 0F 28 C7 F3 0F 5C C2 F3 0F 59 05 ?? ?? ?? ?? F3 0F 59 C8 "
    "0F 2F E9 76 07 33 C0 E9 ?? ?? ?? ?? 48 8B 43 48 48 89 B4 24 68 01 00 00 4C 89 A4 24 70 01 00 00 "
    "0F 29 B4 24 30 01 00 00 0F 29 BC 24 20 01 00 00 44 0F 29 94 24 F0 00 00 00 F3 0F 10 40 08 F3 41 "
    "0F 58 C7 44 0F 29 A4 24 D0 00 00 00 F3 0F 11 40 08 48 8B 43 48 F3 0F 59 5B 34 F3 0F 58 58 2C F3 "
    "0F 11 58 2C";
constexpr int kVctxStats = 0x48;
constexpr int kStatDrawn = 0x8, kStatAll = 0x28, kStatDrawnShare = 0x2C;  // in that block (the creator's +0xD8, +0xF8, +0xFC)
constexpr size_t kSetVerticesBytes = 3270;  // SetVertices' size: where its call of the per-particle function is looked for
// renderer slots a pre-run's SetVertices is expected to call: GetFeatures, GetCamera, EF_GetShaderQuality and
// EF_GetTextureByID (when it emits new particles), EF_ADDFillLight (recorded), EF_Query; any other is logged
constexpr int kExpectedSlots[] = {7, 44, 98, 103, 119, 121};
constexpr int kMaxSlots = 400;
// the refraction list (EFSLIST 14, before and after the water: lists 14 and 29): the list processing (+0xAA200) draws
// it at recursion level 1 only, so a reflection's items there are never prepared
constexpr int kListsPerSide = 15, kRefractList = 14;
#endif  // _WIN64

// mfPrepare calls the pre-runs didn't answer, by recursion level and calling function (for the report)
struct Origin {
    const void* caller;
    int level;
    uint64_t calls;
};
struct Origins {
    Origin o[16] = {};
    int n = 0;
    uint64_t other = 0;
    void Add(const void* caller, int level) {
        for (int i = 0; i < n; ++i)
            if (o[i].caller == caller && o[i].level == level) {
                ++o[i].calls;
                return;
            }
        if (n < 16) {
            o[n++] = Origin{caller, level, 1};
            return;
        }
        ++other;
    }
};

struct PStats {
    uint64_t flushes = 0, items = 0, elements = 0, duplicates = 0, unknownCreator = 0;
    uint64_t preruns = 0, nestedElements = 0, calls = 0, vertices = 0, indices = 0, lights = 0, bytes = 0;
    uint64_t prerunCycles = 0, unusedNested = 0;
    uint64_t prerunCopied = 0, flushCopied = 0, flushCalls = 0;  // calls that copied the particle thread's vertices
    uint64_t prepares = 0, replayed = 0, again = 0, notRecorded = 0, contextDiffer = 0, grantMismatch = 0;
    uint64_t unused = 0, replayCycles = 0;
    uint64_t prepareReplayedCycles = 0, prepareOtherCycles = 0;  // whole mfPrepare calls in the flush
    uint64_t unexpected = 0;
    int unexpectedSlot = -1, maxElements = 0;
    size_t maxBytes = 0;
    Origins replayedFrom, againFrom, notRecordedFrom;
    uint64_t listPreruns[2][30] = {}, listUnused[2][30] = {};  // per recursion level (1, 2) and render list
    // verify: the flush's own SetVertices call against the pre-run's
    uint64_t vCompared = 0, vSame = 0, vDiffer = 0, vContextDiffer = 0, vNestedLights = 0;
    uint64_t replayedAgain = 0;  // repeated prepares answered from the record (their counter updates after the flush)
    uint64_t againComputed = 0;  // repeated prepares left to vanilla: the pre-run computed the vertices itself
    uint64_t replayedAgainShares = 0;  // ... of those replayed, with the shares the pre-run's own computation added
    uint64_t againChanged = 0;   // elements whose updates after the flush found the counters changed since the pre-run
    uint64_t againFreed = 0;     // ... whose container was freed meanwhile (FlushThread=overlap: skipped)
    uint64_t againLocked = 0;    // ... whose container's lock stayed taken (skipped)
    uint64_t repeatCycles = 0;   // those updates
    uint64_t shareParticles = 0;  // particles whose shares the pre-runs recorded
    uint64_t vAgainCompared = 0, vAgainSame = 0, vAgainOutput = 0, vAgainCounters = 0, vAgainContextDiffer = 0;
    uint64_t vAgainChanged = 0, vAgainSharesCompared = 0, vAgainSharesSame = 0;  // (repeated prepares with shares)
    uint64_t vWhat[5] = {};
};

#ifdef _WIN64
const char* const kWhatNames[5] = {"calls", "vertices", "indices", "lights", "result"};

struct Context {  // SParticleRenderContext, as mfPrepare makes it
    const char* camera;
    uint8_t noFeature;  // !(GetFeatures() bit 29)
    uint8_t flag;       // the element's flags bit 11
    uint8_t pad[6];
};
static_assert(sizeof(Context) == 16, "the context is 16 bytes");

using PrepareFn = void (*)(char* elem);
using SetVerticesFn = uint64_t (*)(char* creator, void* alloc, const Context* ctx);
using ParticleVertsFn = uint64_t (*)(char* particle, char* verts, char* vctx);
using AllocFn = void (*)(void* alloc, pp::RenderVerts* rv, int nVerts, int nInds);
using GetFeaturesFn = uint32_t (*)(char* renderer);
using GetCameraFn = const char* (*)(char* renderer);
using AddFillLightFn = void (*)(char* renderer, const void* light);

enum : uint8_t { kRecorded, kReplayed };
struct Entry {
    char* elem;
    char* creator;  // its vertex creator, which the replay swaps out for a while
    uint8_t flag;
    uint8_t state;
    uint8_t level;  // the recursion level its items are in: 1, or 2 for a reflection's
    uint16_t cam;   // the camera it was pre-run with (PState::cams)
    uint8_t list;   // the render list its item is in
    bool copied;    // the pre-run copied the particle thread's vertices (else it computed them, adding to the estimates)
    uint16_t repeats;  // its repeated prepares replayed in this flush, whose counter updates are applied after it
    pp::Record rec;
    // computed by the pre-run: the shares it added to the estimates, per particle (3 floats each in PState::shares);
    // and the per-frame counters as the element's last call left them, which a repeated prepare starts from
    uint32_t sharesFirst, sharesCount;
    bool sharesOk;  // recorded (the per-particle function hooked), and into this creator's estimates
    uint32_t expect[4];
};

struct Camera {  // the renderer's camera and feature flag at a pre-run: the context it gave
    const char* ptr;
    uint8_t bytes[kCamBytes];
    uint8_t noFeature;
};

// a small cache of vtables seen: CREParticle's or not; the vertex creators' SetVertices or not
struct VtCache {
    void** key[128] = {};
    uint8_t value[128] = {};
    int Find(void** vt) const {
        for (uint32_t h = uint32_t(uintptr_t(vt) >> 4) & 127, n = 0; n < 128; h = (h + 1) & 127, ++n) {
            if (key[h] == vt) return value[h];
            if (!key[h]) return -1;
        }
        return -1;
    }
    void Put(void** vt, uint8_t v) {
        for (uint32_t h = uint32_t(uintptr_t(vt) >> 4) & 127, n = 0; n < 128; h = (h + 1) & 127, ++n)
            if (!key[h] || key[h] == vt) {
                key[h] = vt;
                value[h] = v;
                return;
            }
    }
};

// element -> entry, cleared per flush by a generation count (no allocations once grown)
class ElemIndex {
public:
    void Clear() {
        size_ = 0;
        if (++gen_ == 0) {
            for (Slot& x : slots_) x = Slot{};
            gen_ = 1;
        }
    }
    const uint32_t* Find(const char* key) const {
        if (slots_.empty()) return nullptr;
        for (size_t h = Hash(key) & (slots_.size() - 1);; h = (h + 1) & (slots_.size() - 1)) {
            const Slot& x = slots_[h];
            if (x.gen != gen_) return nullptr;
            if (x.key == key) return &x.value;
        }
    }
    void Insert(const char* key, uint32_t value) {  // (key not present)
        if ((size_ + 1) * 2 > slots_.size()) Grow();
        Put(key, value);
        ++size_;
    }

private:
    struct Slot {
        const char* key = nullptr;
        uint32_t value = 0, gen = 0;
    };
    static size_t Hash(const char* key) { return size_t((uintptr_t(key) >> 4) * 0x9E3779B97F4A7C15ull >> 20); }
    void Put(const char* key, uint32_t value) {
        for (size_t h = Hash(key) & (slots_.size() - 1);; h = (h + 1) & (slots_.size() - 1)) {
            Slot& x = slots_[h];
            if (x.gen != gen_) {
                x = Slot{key, value, gen_};
                return;
            }
        }
    }
    void Grow() {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.empty() ? 4096 : old.size() * 2, Slot{});
        const uint32_t gen = gen_;
        for (const Slot& x : old)
            if (x.gen == gen) Put(x.key, x.value);
    }
    std::vector<Slot> slots_;
    size_t size_ = 0;
    uint32_t gen_ = 1;
};

struct VtObject {  // an object of ours that engine code calls through slot 0
    void* const* vtable;
};
#endif  // _WIN64

struct PState {
    PMode mode = PMode::Off;
    bool installed = false, setupTried = false, ready = false, verify = false, inFlush = false;
#ifdef _WIN64
    ParticleTarget t;
    const ShadowPrepassTarget* shadow = nullptr;
    char* base = nullptr;
    size_t size = 0;
    char* e3dBase = nullptr;
    char* renderer = nullptr;
    DWORD mainThread = 0;
    VtableCounter counter;  // the renderer's: its fill-light slot recorded while the pre-runs (and verify's calls) run
    PrepareFn prepare = nullptr;
    GetFeaturesFn getFeatures = nullptr;
    GetCameraFn getCamera = nullptr;
    AddFillLightFn addFillLight = nullptr;
    char* setVertices = nullptr;
    VtCache elements, creators;
    pp::Recorder rec;        // this flush's pre-runs
    pp::Recorder verifyRec;  // verify: the flush's own call
    pp::Recorder* recording = nullptr;  // where fill lights go while a recorded SetVertices runs (main thread)
    std::vector<Entry> entries;
    ElemIndex index;
    Entry* current = nullptr;
    bool again = false;  // the current mfPrepare is the element's second (or later) in this flush
    std::vector<Camera> cams;  // this flush's pre-runs' contexts
    bool inEntry = false;      // between the flush's entry and BeforeFlush: a reflection's level may be pre-run
    ParticleVertsFn particleVerts = nullptr;  // the per-particle vertex function's trampoline (null: not hooked)
    bool capturing = false;    // a pre-run's SetVertices is running: the per-particle shares are recorded
    std::vector<float> shares; // this flush's recorded shares: +0x28, +0x8, +0x2C per particle
    char* sharesStats = nullptr;  // the estimates the last recorded share went to (a pre-run checks it is its creator's)
    std::vector<uint64_t> slotCalls;
    int loggedContext = 0, loggedVerify = 0, loggedUnused = 0, loggedUnexpected = 0, loggedMismatch = 0;
    int loggedShares = 0, loggedChanged = 0, loggedLocked = 0;
#endif
    PStats st;
};
PState g;

#ifdef _WIN64
template <class T>
T& At(char* p, int offset) {
    return *reinterpret_cast<T*>(p + offset);
}

char* Rip(char* field, int tail) { return field + 4 + tail + int32_t(ReadU32(field)); }

char* Unique(char* base, const char* text, const char** why, const char* what) {
    const Pattern p = ParsePattern(text);
    const auto hits = FindInCode(base, p, 2);
    if (hits.size() != 1) *why = what;
    return hits.size() == 1 ? hits[0] : nullptr;
}

bool Matches(char* base, char* at, const char* text) {
    const Pattern p = ParsePattern(text);
    return IsInCode(base, at, p.size()) && MatchesAt(at, p);
}

// A vtable entry of a module mapped at base: the address as relocated, or, when the image wasn't (a test mapping it
// unrelocated), rebased from the image's preferred base.
char* Entry0(char* base, void* entry) {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint64_t e = reinterpret_cast<uint64_t>(entry), size = nt->OptionalHeader.SizeOfImage;
    if (e - reinterpret_cast<uint64_t>(base) < size) return static_cast<char*>(entry);
    if (e - nt->OptionalHeader.ImageBase < size) return base + (e - nt->OptionalHeader.ImageBase);
    return nullptr;
}

void Append(char* line, size_t size, size_t& n, const char* fmt, ...) {
    if (n >= size) return;
    va_list a;
    va_start(a, fmt);
    const int k = vsnprintf(line + n, size - n, fmt, a);
    va_end(a);
    if (k > 0) n = (std::min)(size - 1, n + size_t(k));
}

void* Slot0(char* object) { return (*reinterpret_cast<void***>(object))[0]; }

// --- the recording allocator, the replayer, the fill-light recorder ----------------------------------------------------
void RecordAlloc(void* /*self*/, pp::RenderVerts* rv, int nVerts, int nInds) { g.rec.Alloc(rv, nVerts, nInds); }
void VerifyAlloc(void* /*self*/, pp::RenderVerts* rv, int nVerts, int nInds) { g.verifyRec.Alloc(rv, nVerts, nInds); }
uint64_t ReplaySetVertices(void* self, void* alloc, const Context* ctx);

void* const kRecordAllocVt[1] = {reinterpret_cast<void*>(&RecordAlloc)};
void* const kVerifyAllocVt[1] = {reinterpret_cast<void*>(&VerifyAlloc)};
void* const kReplayerVt[1] = {reinterpret_cast<void*>(&ReplaySetVertices)};
VtObject g_recordAlloc = {kRecordAllocVt};
VtObject g_verifyAlloc = {kVerifyAllocVt};
VtObject g_replayer = {kReplayerVt};

// the renderer's slot 119 in the vtable copy: recorded while a recorded SetVertices runs, else passed on (the pre-runs
// record on the main thread before the flush, verify's calls in the flush on its thread)
void RecordFillLight(char* r, const void* light) {
    if (g.recording && GetCurrentThreadId() == (g.inFlush ? FlushThread() : g.mainThread))
        return g.recording->Light(light);
    g.addFillLight(r, light);
}

// The per-particle vertex function, while a pre-run's SetVertices runs on the main thread: its shares go into a block
// of -0.0 instead of the container's estimates, which keeps each one exactly (-0 + x is x for every x, and a share not
// written stays -0, which added changes nothing). They are recorded, then added to the estimates as the function adds
// them. Every other call (the particle thread's, the flush's) as it was.
uint64_t ParticleVertsHook(char* particle, char* verts, char* vctx) {
    if (!g.capturing || GetCurrentThreadId() != g.mainThread) return g.particleVerts(particle, verts, vctx);
    char*& field = At<char*>(vctx, kVctxStats);
    char* const stats = field;
    alignas(16) float scratch[16];
    std::fill(std::begin(scratch), std::end(scratch), -0.0f);
    field = reinterpret_cast<char*>(scratch);
    const uint64_t result = g.particleVerts(particle, verts, vctx);
    field = stats;
    g.sharesStats = stats;
    const float all = scratch[kStatAll / 4], drawn = scratch[kStatDrawn / 4], share = scratch[kStatDrawnShare / 4];
    g.shares.push_back(all);
    g.shares.push_back(drawn);
    g.shares.push_back(share);
    At<float>(stats, kStatAll) = all + At<float>(stats, kStatAll);
    At<float>(stats, kStatDrawn) = drawn + At<float>(stats, kStatDrawn);
    At<float>(stats, kStatDrawnShare) = share + At<float>(stats, kStatDrawnShare);
    return result;
}

// The shares a recorded call added, added again in the same order: what a repeated SetVertices adds when it computes
// the vertices too (the creator's +0xD0 is the block the function adds to)
void ApplyShares(char* vc, const Entry& e) {
    char* stats = vc + 0xD0;
    const float* s = g.shares.data() + e.sharesFirst;
    for (uint32_t i = 0; i < e.sharesCount; ++i, s += 3) {
        At<float>(stats, kStatAll) = s[0] + At<float>(stats, kStatAll);
        At<float>(stats, kStatDrawn) = s[1] + At<float>(stats, kStatDrawn);
        At<float>(stats, kStatDrawnShare) = s[2] + At<float>(stats, kStatDrawnShare);
    }
}

// the per-frame counters (+0xD8, +0xF8, +0xFC, +0x108) as the element's last call left them, and whether they still
// are: else something else (the particle thread computing the vertices meanwhile) changed them
void SaveExpected(Entry& e) {
    for (int i = 0; i < 4; ++i) e.expect[i] = At<uint32_t>(e.creator, kVcFloats[i]);
}
bool SameCounters(const Entry& e) {
    for (int i = 0; i < 4; ++i)
        if (At<uint32_t>(e.creator, kVcFloats[i]) != e.expect[i]) return false;
    return true;
}

// SetVertices' call of the per-particle function: found in a mapped Cry3DEngine.dll from both functions' code
char* LocateParticleVerts(char* e3d, const char** why) {
    char* sv = Unique(e3d, kSetVertices, why, "SetVertices not found");
    char* pv = sv ? Unique(e3d, kParticleVerts, why, "the per-particle vertex function is not the expected code") : nullptr;
    if (!pv) return nullptr;
    for (size_t i = 0; i + 5 <= kSetVerticesBytes; ++i)
        if (uint8_t(sv[i]) == 0xE8 && RelTarget(sv + i) == pv) return pv;
    *why = "SetVertices doesn't call the per-particle vertex function";
    return nullptr;
}

bool IsParticleElement(char* elem) {
    void** vt = *reinterpret_cast<void***>(elem);
    const int known = g.elements.Find(vt);
    if (known >= 0) return known != 0;
    const bool yes = IsInImage(g.base, reinterpret_cast<const char*>(vt), 3 * sizeof(void*)) &&
                     CallTrace_Original(vt[2]) == g.t.prepare;
    g.elements.Put(vt, yes ? 1 : 0);
    return yes;
}

bool IsKnownCreator(char* creator) {
    void** vt = *reinterpret_cast<void***>(creator);
    const int known = g.creators.Find(vt);
    if (known >= 0) return known != 0;
    char* fn = static_cast<char*>(CallTrace_Original(vt[0]));
    bool yes = fn == g.setVertices;
    if (!yes && !g.setVertices && ParticlePrepass_IsSetVertices(g.e3dBase, fn)) {
        g.setVertices = fn;
        yes = true;
        Log("particles: vertex creators' SetVertices at Cry3DEngine.dll+0x%X", unsigned(fn - g.e3dBase));
    }
    g.creators.Put(vt, yes ? 1 : 0);
    if (!yes) Log("particles: a vertex creator whose SetVertices is not the expected code (vtable %p): left in the flush", vt);
    return yes;
}

// --- right before the flush: SetVertices for the main pass's particle elements ----------------------------------------
void Setup(char* r) {
    g.setupTried = true;
    g.mainThread = GetCurrentThreadId();
    g.renderer = r;
    HMODULE e3d = GetModuleHandleW(L"Cry3DEngine.dll");
    if (!e3d || !g.counter.Init(r, g.base, g.size, kMaxSlots) || g.counter.Slots() <= kVfAddFillLight + 2) {
        Log("particles: left in the flush: %s", e3d ? "could not copy the renderer's vtable" : "no Cry3DEngine.dll");
        return;
    }
    if (CallTrace_Original(g.counter.Original(kVfAddFillLight)) != g.t.addFillLight) {
        Log("particles: left in the flush: EF_ADDFillLight is not at its vtable slot");
        return;
    }
    g.e3dBase = reinterpret_cast<char*>(e3d);
    g.getFeatures = reinterpret_cast<GetFeaturesFn>(g.counter.Original(kVfGetFeatures));
    g.getCamera = reinterpret_cast<GetCameraFn>(g.counter.Original(kVfGetCamera));
    g.addFillLight = reinterpret_cast<AddFillLightFn>(g.counter.Original(kVfAddFillLight));
    g.counter.SetTarget(kVfAddFillLight, reinterpret_cast<void*>(&RecordFillLight));
    g.slotCalls.assign(size_t(g.counter.Slots()), 0);
    // the per-particle vertex function: its shares recorded in the pre-runs that compute their vertices, so that a
    // repeated prepare can add them again (without it those repeated prepares stay with vanilla's call)
    const char* why = nullptr;
    char* pv = LocateParticleVerts(g.e3dBase, &why);
    if (pv) {
        const HookSpec hook[] = {{"the per-particle vertex function", pv, reinterpret_cast<void*>(&ParticleVertsHook),
                                  reinterpret_cast<void**>(&g.particleVerts)}};
        if (!InstallHooks(hook, 1)) {
            g.particleVerts = nullptr;
            why = "could not write its hook";
        }
    }
    g.ready = true;
    Log("particles: produced before the flush (mode=%s); the renderer's vtable has %d slots", ModeName(g.mode),
        g.counter.Slots());
    if (g.particleVerts)
        Log("particles: the per-particle vertex function (Cry3DEngine.dll+0x%X) hooked: the shares of the pre-runs that "
            "compute their vertices are recorded",
            unsigned(pv - g.e3dBase));
    else
        Log("particles: the per-particle shares are not recorded (%s): repeated prepares of containers whose pre-run "
            "computed the vertices stay with vanilla's call",
            why);
}

bool Expected(int slot) {
    for (int s : kExpectedSlots)
        if (s == slot) return true;
    return false;
}

// SetVertices for the particle elements of one recursion level's items, with the context mfPrepare will make for them:
// level 1 right before the flush (counted: the renderer's vtable copy records fill lights and counts its other
// calls); a reflection's level 2 where the dry run takes it (not counted: the reflection part's own copy of the
// renderer's vtable is in place then, and at the 3D engine's level there SetVertices adds no fill lights).
void Prerun(char* r, int level, bool counted) {
    const uint64_t t0 = __rdtsc();
    Camera cam;
    cam.ptr = g.getCamera(r);
    std::memcpy(cam.bytes, cam.ptr, kCamBytes);
    cam.noFeature = uint8_t(~(g.getFeatures(r) >> kFeatureBit) & 1);
    const uint16_t camIndex = uint16_t(g.cams.size());
    g.cams.push_back(cam);
    const int maxVerts = At<int>(r, kRMaxVerts), maxInds = 3 * At<int>(r, kRMaxInds);
    auto* lists = reinterpret_cast<sp::ItemList*>(g.shadow->lists);
    const uint32_t* starts = g.shadow->starts + (level - 1) * sp::kLists;
    const size_t first = g.entries.size();
    for (int k = 0; k < sp::kLists; ++k) {
        const sp::ItemList& l = lists[k];
        if (level > 1 && k % kListsPerSide == kRefractList) continue;  // (drawn at level 1 only)
        for (uint32_t i = starts[k]; i < l.count; ++i) {
            char* elem = l.elems[i].elem;
            ++g.st.items;
            if (!elem || !IsParticleElement(elem)) continue;
            if (g.index.Find(elem)) {
                ++g.st.duplicates;
                continue;
            }
            char* creator = At<char*>(elem, kElemCreator);
            if (!creator || !IsKnownCreator(creator)) {
                ++g.st.unknownCreator;
                continue;
            }
            g.index.Insert(elem, uint32_t(g.entries.size()));
            Entry e{};
            e.elem = elem;
            e.creator = creator;
            e.flag = uint8_t((At<uint32_t>(elem, kElemFlags) >> kElemFlagBit) & 1);
            e.state = kRecorded;
            e.cam = camIndex;
            e.level = uint8_t(level);
            e.list = uint8_t(k);
            ++g.st.listPreruns[level > 1 ? 1 : 0][k];
            g.entries.push_back(e);
            Allocator_Pin(creator);  // (FlushThread=overlap: the container kept for its counters' update at the join)
        }
    }
    const size_t n = g.entries.size() - first;
    g.st.elements += n;
    if (level > 1) g.st.nestedElements += n;
    g.st.maxElements = (std::max)(g.st.maxElements, int(g.entries.size()));
    if (!n) return;
    if (counted) {
        for (int i = 0; i < g.counter.Slots(); ++i) g.slotCalls[size_t(i)] = g.counter.Inside(i) + g.counter.Outside(i);
        g.recording = &g.rec;
        g.counter.Begin();
    }
    for (size_t x = first; x < g.entries.size(); ++x) {
        Entry& e = g.entries[x];
        const Context ctx{cam.ptr, cam.noFeature, e.flag, {}};
        g.rec.Begin(e.rec, maxVerts, maxInds);
        e.sharesFirst = uint32_t(g.shares.size());
        g.sharesStats = nullptr;
        g.capturing = g.particleVerts != nullptr;
        const uint64_t result = reinterpret_cast<SetVerticesFn>(Slot0(e.creator))(e.creator, &g_recordAlloc, &ctx);
        g.capturing = false;
        g.rec.End(e.rec, result);
        e.sharesCount = uint32_t((g.shares.size() - e.sharesFirst) / 3);
        e.copied = At<uint8_t>(e.creator, kVcComputed) != 0;
        if (e.copied) ++g.st.prerunCopied;
        // the shares can be added again only if they went to this creator's estimates
        e.sharesOk = g.particleVerts && (!e.sharesCount || g.sharesStats == e.creator + 0xD0);
        if (!e.sharesOk && g.particleVerts && ++g.loggedShares <= 10)
            Log("particles: flush %llu: element %p's per-particle shares went to %p, not its creator's estimates (%p)",
                g.st.flushes, e.elem, g.sharesStats, e.creator + 0xD0);
        g.st.shareParticles += e.sharesCount;
        SaveExpected(e);
        g.st.calls += e.rec.calls;
        g.st.lights += e.rec.lights;
        for (uint32_t k = 0; k < e.rec.calls; ++k) {
            int v, i;
            g.rec.Written(e.rec, k, &v, &i);
            g.st.vertices += uint64_t(v);
            g.st.indices += uint64_t(i);
        }
    }
    g.st.preruns += n;
    if (counted) {
        g.counter.End();
        g.recording = nullptr;
        for (int i = 0; i < g.counter.Slots(); ++i) {
            const uint64_t calls = g.counter.Inside(i) + g.counter.Outside(i) - g.slotCalls[size_t(i)];
            if (!calls || Expected(i)) continue;
            g.st.unexpected += calls;
            g.st.unexpectedSlot = i;
            if (++g.loggedUnexpected <= 10)
                Log("particles: flush %llu: the pre-runs called renderer slot %d (%llu times)", g.st.flushes, i, calls);
        }
    }
    g.st.prerunCycles += __rdtsc() - t0;
}

// --- in the flush ------------------------------------------------------------------------------------------------------
// SetVertices' last step, per call: the renders this frame (+0x108) counted, and once there are two or more, three
// estimates scaled by renders / (renders - 1); single precision, as its code does it (Cry3DEngine +0x856A8)
void ApplyRenderCounter(char* vc) {
    float& renders = At<float>(vc, 0x108);
    const float f = renders + 1.0f;
    renders = f;
    if (f > 1.0f) {
        const float s = f / (f - 1.0f);
        At<float>(vc, 0xF8) = s * At<float>(vc, 0xF8);
        At<float>(vc, 0xD8) = s * At<float>(vc, 0xD8);
        At<float>(vc, 0xFC) = s * At<float>(vc, 0xFC);
    }
}

// what one repeated SetVertices call does to the container: its shares again (when it computes the vertices: none
// were recorded otherwise), then the render counter (when it ends normally)
void ApplyRepeat(char* vc, const Entry& e) {
    ApplyShares(vc, e);
    if (g.rec.Committed(e.rec)) ApplyRenderCounter(vc);
}

void SaveCounters(char* vc, uint32_t* floats, uint8_t* setFlag) {
    for (int i = 0; i < 4; ++i) floats[i] = At<uint32_t>(vc, kVcFloats[i]);
    *setFlag = At<uint8_t>(vc, kVcSetFlag);
}

void RestoreCounters(char* vc, const uint32_t* floats, uint8_t setFlag) {
    for (int i = 0; i < 4; ++i) At<uint32_t>(vc, kVcFloats[i]) = floats[i];
    At<uint8_t>(vc, kVcSetFlag) = setFlag;
}

// verify: SetVertices at the flush's call into a recording allocator (under the container's lock, which it takes
// itself too); vr gets its record, after[] the per-frame counters as it left them
void RecordFlushCall(char* vc, const Context* ctx, pp::Record& vr, uint32_t* after) {
    char* r = g.renderer;
    g.verifyRec.Reset();
    g.verifyRec.Begin(vr, At<int>(r, kRMaxVerts), 3 * At<int>(r, kRMaxInds));
    g.recording = &g.verifyRec;
    g.counter.Begin();
    const uint64_t result = reinterpret_cast<SetVerticesFn>(Slot0(vc))(vc, &g_verifyAlloc, ctx);
    g.counter.End();
    g.recording = nullptr;
    g.verifyRec.End(vr, result);
    for (int i = 0; i < 4; ++i) after[i] = At<uint32_t>(vc, kVcFloats[i]);
    ++g.st.flushCalls;
    if (At<uint8_t>(vc, kVcComputed)) ++g.st.flushCopied;
}

// whether the flush's call made what the record holds. A reflection's element may add fill lights in verify's own
// call only: vanilla calls it inside the reflection's traversal, where the 3D engine adds none (nor did the pre-run).
bool SameOutput(const Entry& e, const pp::Record& vr, const char** what, bool* nestedLights) {
    *nestedLights = false;
    if (g.rec.Same(e.rec, g.verifyRec, vr, what)) return true;
    if (e.level > 1 && *what && std::strcmp(*what, "lights") == 0 && e.rec.lights == 0) {
        *nestedLights = true;
        return true;
    }
    return false;
}

// verify, the element's first prepare: the flush's SetVertices against the pre-run's record (its counters put back)
void Verify(Entry& e, const Context* ctx, bool sameContext) {
    char* vc = e.creator;
    auto* lock = At<CRITICAL_SECTION*>(vc, kVcLock);
    EnterCriticalSection(lock);
    uint32_t floats[4], after[4];
    uint8_t setFlag = 0;
    SaveCounters(vc, floats, &setFlag);
    pp::Record vr;
    RecordFlushCall(vc, ctx, vr, after);
    RestoreCounters(vc, floats, setFlag);
    LeaveCriticalSection(lock);
    if (!sameContext) {
        ++g.st.vContextDiffer;
        return;
    }
    ++g.st.vCompared;
    const char* what = nullptr;
    bool nestedLights = false;
    if (SameOutput(e, vr, &what, &nestedLights)) {
        ++g.st.vSame;
        if (nestedLights) ++g.st.vNestedLights;
        return;
    }
    ++g.st.vDiffer;
    for (int i = 0; i < 5; ++i)
        if (what && std::strcmp(what, kWhatNames[i]) == 0) ++g.st.vWhat[i];
    if (++g.loggedVerify <= 20)
        Log("particles: VERIFY flush %llu: element %p (creator %p): the flush's SetVertices differs from the pre-run's in "
            "its %s (calls %u/%u, lights %u/%u, result %llu/%llu)",
            g.st.flushes, e.elem, vc, what ? what : "?", e.rec.calls, vr.calls, e.rec.lights, vr.lights, e.rec.result,
            vr.result);
}

// verify, a repeated prepare: vanilla's repeated call against the record, and the per-frame counters it leaves
// against the update applied after the flush (ApplyRepeat); both from where the element's earlier repeated prepares,
// with their updates, leave the counters (in the flush they are still as the pre-run left them)
void VerifyAgain(Entry& e, const Context* ctx, bool sameContext) {
    char* vc = e.creator;
    auto* lock = At<CRITICAL_SECTION*>(vc, kVcLock);
    EnterCriticalSection(lock);
    if (!SameCounters(e)) {  // changed since the pre-run (not compared)
        LeaveCriticalSection(lock);
        ++g.st.vAgainChanged;
        return;
    }
    uint32_t floats[4], start[4], vanilla[4], emulated[4];
    uint8_t setFlag = 0, startFlag = 0;
    SaveCounters(vc, floats, &setFlag);
    for (uint16_t i = 0; i < e.repeats; ++i) ApplyRepeat(vc, e);  // (the earlier repeated prepares' updates)
    SaveCounters(vc, start, &startFlag);
    pp::Record vr;
    RecordFlushCall(vc, ctx, vr, vanilla);
    RestoreCounters(vc, start, startFlag);
    ApplyRepeat(vc, e);
    for (int i = 0; i < 4; ++i) emulated[i] = At<uint32_t>(vc, kVcFloats[i]);
    RestoreCounters(vc, floats, setFlag);
    LeaveCriticalSection(lock);
    if (!sameContext) {
        ++g.st.vAgainContextDiffer;
        return;
    }
    ++g.st.vAgainCompared;
    const char* what = nullptr;
    bool nestedLights = false;
    const bool output = SameOutput(e, vr, &what, &nestedLights);
    const bool counters = std::memcmp(vanilla, emulated, sizeof vanilla) == 0;
    if (e.sharesCount) {
        ++g.st.vAgainSharesCompared;
        if (output && counters) ++g.st.vAgainSharesSame;
    }
    if (output && counters) {
        ++g.st.vAgainSame;
        return;
    }
    if (!output) ++g.st.vAgainOutput;
    if (!counters) ++g.st.vAgainCounters;
    if (++g.loggedVerify <= 20)
        Log("particles: VERIFY flush %llu: element %p prepared again: %s%s%s (renders %08X/%08X, estimates %08X %08X "
            "%08X / %08X %08X %08X)",
            g.st.flushes, e.elem, output ? "output as recorded" : "its output differs from the record in its ",
            output ? "" : (what ? what : "?"), counters ? "" : ", the render counters differ from vanilla's", vanilla[3],
            emulated[3], vanilla[0], vanilla[1], vanilla[2], emulated[0], emulated[1], emulated[2]);
}

uint64_t ReplaySetVertices(void* /*self*/, void* alloc, const Context* ctx) {
    Entry& e = *g.current;
    char* r = g.renderer;
    const uint64_t t0 = __rdtsc();
    const Camera& cam = g.cams[e.cam];
    const bool same = ctx->camera == cam.ptr && std::memcmp(ctx->camera, cam.bytes, kCamBytes) == 0 &&
                      ctx->noFeature == cam.noFeature && ctx->flag == e.flag && At<int>(r, kRMaxVerts) == e.rec.maxVerts &&
                      3 * At<int>(r, kRMaxInds) == e.rec.maxInds;
    if (g.verify) {
        if (g.again)
            VerifyAgain(e, ctx, same);
        else
            Verify(e, ctx, same);
    }
    if (!same) {  // not the context it was recorded in: vanilla's call after all
        ++g.st.contextDiffer;
        if (++g.loggedContext <= 10)
            Log("particles: flush %llu: element %p (level %d) prepared in another context than its pre-run's (camera "
                "%s, flags %u/%u %u/%u): SetVertices runs in the flush",
                g.st.flushes, e.elem, e.level, ctx->camera != cam.ptr ? "pointer" : "contents", ctx->noFeature,
                cam.noFeature, ctx->flag, e.flag);
        return reinterpret_cast<SetVerticesFn>(Slot0(e.creator))(e.creator, alloc, ctx);
    }
    // as SetVertices made them: its fill lights, then its allocator calls (for a repeated call too: what it does to
    // the container's counters is applied after the flush, ApplyRepeats)
    for (uint32_t k = 0; k < e.rec.lights; ++k) g.addFillLight(r, g.rec.Light(e.rec, k));
    const auto allocFn = reinterpret_cast<AllocFn>(Slot0(static_cast<char*>(alloc)));
    const int mismatches =
        g.rec.Replay(e.rec, [&](pp::RenderVerts* rv, int nVerts, int nInds) { allocFn(alloc, rv, nVerts, nInds); });
    if (mismatches) {
        g.st.grantMismatch += uint64_t(mismatches);
        if (++g.loggedMismatch <= 10)
            Log("particles: flush %llu: element %p: %d grants differed from the recorded ones (cut to fit)", g.st.flushes,
                e.elem, mismatches);
    }
    if (g.again) {
        ++e.repeats;
        ++g.st.replayedAgain;
        if (e.sharesCount) ++g.st.replayedAgainShares;
    } else {
        ++g.st.replayed;
    }
    g.st.replayCycles += __rdtsc() - t0;
    return e.rec.result;
}

// a recorded element's mfPrepare: as vanilla, with its vertex creator swapped for the replayer meanwhile
void PrepareRecorded(char* elem, Entry& e, bool again) {
    e.state = kReplayed;
    g.current = &e;
    g.again = again;
    char*& creator = At<char*>(elem, kElemCreator);
    creator = reinterpret_cast<char*>(&g_replayer);
    g.prepare(elem);
    creator = e.creator;
    g.current = nullptr;
    g.again = false;
}

void PrepareHook(char* elem) {
    if (!g.inFlush || !OnFlushThread()) return g.prepare(elem);
    const uint64_t t0 = __rdtsc();
    ++g.st.prepares;
    const uint32_t* at = g.index.Find(elem);
    Entry* e = at ? &g.entries[*at] : nullptr;
    if (!e) {  // not recorded: as vanilla
        ++g.st.notRecorded;
        g.st.notRecordedFrom.Add(_ReturnAddress(), *g.shadow->level);
        g.prepare(elem);
        g.st.prepareOtherCycles += __rdtsc() - t0;
        return;
    }
    const bool again = e->state != kRecorded;  // prepared again (in the shadow pass first, then in the main pass)
    if (again) {
        ++g.st.again;
        g.st.againFrom.Add(_ReturnAddress(), *g.shadow->level);
        if (!e->copied && !e->sharesOk) {
            // its pre-run computed the vertices itself, which also adds each particle's share to the container's
            // estimates, and those shares weren't recorded: only vanilla's call adds them again exactly
            ++g.st.againComputed;
            g.prepare(elem);
            g.st.prepareOtherCycles += __rdtsc() - t0;
            return;
        }
    } else {
        g.st.replayedFrom.Add(_ReturnAddress(), *g.shadow->level);
    }
    PrepareRecorded(elem, *e, again);
    g.st.prepareReplayedCycles += __rdtsc() - t0;
}

// Right after the flush, on the main thread: what each repeated prepare's SetVertices call does to the container's
// counters, under its lock as SetVertices takes it, from where the pre-run left them; if the particle thread has
// changed them since, onto those (counted: vanilla's calls, inside the flush, would have started from wherever they
// were then). With FlushThread=overlap this runs at the join, wherever the main thread is then, perhaps inside the
// game's code with its locks held: a container freed meanwhile is skipped (its block was kept: allocator.h's pins),
// and a lock is tried for a while rather than waited for (a lock held by a thread that waits for the main thread
// would wait for good), its updates skipped if it stays taken (counted).
constexpr int kLockTries = 2000;  // ~0.1 ms of yields
bool TryLock(CRITICAL_SECTION* lock) {
    for (int i = 0; i < kLockTries; ++i) {
        if (TryEnterCriticalSection(lock)) return true;
        YieldProcessor();
        if (i % 64 == 63) SwitchToThread();
    }
    return false;
}

void ApplyRepeats() {
    const uint64_t t0 = __rdtsc();
    for (Entry& e : g.entries) {
        if (!e.repeats) continue;
        if (Allocator_Dying(e.creator)) {  // (freed beside the flush)
            ++g.st.againFreed;
            e.repeats = 0;
            continue;
        }
        auto* lock = At<CRITICAL_SECTION*>(e.creator, kVcLock);
        if (!TryLock(lock)) {
            ++g.st.againLocked;
            if (++g.loggedLocked <= 10)
                Log("particles: flush %llu: element %p's container stayed locked: its %u repeated prepares' updates "
                    "skipped",
                    g.st.flushes, e.elem, unsigned(e.repeats));
            e.repeats = 0;
            continue;
        }
        if (!SameCounters(e)) {
            ++g.st.againChanged;
            if (++g.loggedChanged <= 10)
                Log("particles: flush %llu: element %p's counters changed between its pre-run and the end of the flush: "
                    "its %u repeated prepares' updates applied to them all the same",
                    g.st.flushes, e.elem, unsigned(e.repeats));
        }
        for (uint16_t i = 0; i < e.repeats; ++i) ApplyRepeat(e.creator, e);
        LeaveCriticalSection(lock);
        e.repeats = 0;
    }
    g.st.repeatCycles += __rdtsc() - t0;
}

void Report(const char* when) {
    const PStats& s = g.st;
    const double f = double(s.flushes ? s.flushes : 1);
    Log("particles [%s]: mode=%s | flushes=%llu | items scanned=%llu: particle elements=%llu (of them in reflections "
        "%llu; most in a flush %d), duplicates=%llu, unknown vertex creators=%llu | pre-runs=%llu: allocator "
        "calls=%llu, vertices=%llu, indices=%llu, fill lights=%llu, copying the particle thread's vertices=%llu, %.2f MB "
        "per flush (most %.2f MB), %.3f ms per flush",
        when, ModeName(g.mode), s.flushes, s.items, s.elements, s.nestedElements, s.maxElements, s.duplicates,
        s.unknownCreator, s.preruns, s.calls, s.vertices, s.indices, s.lights, s.prerunCopied,
        double(s.bytes) / f / 1048576.0, double(s.maxBytes) / 1048576.0, double(s.prerunCycles) / 3.0e6 / f);
    Log("particles [%s]: in the flush: mfPrepare calls=%llu: replayed=%llu, prepared again=%llu (replayed again=%llu, "
        "of them with the shares of a pre-run that computed the vertices=%llu, their counter updates applied after the "
        "flush in %.4f ms per flush, onto counters changed since the pre-run for %llu elements, skipped for %llu whose "
        "container was freed and %llu whose lock stayed taken; vanilla's call: the "
        "pre-run computed the vertices without recorded shares=%llu) (%.3f ms per flush in the replays, %.3f ms in their "
        "whole mfPrepare calls), not recorded=%llu (%.3f ms per flush in their mfPrepare calls), other context=%llu, "
        "grants differing=%llu | pre-runs not used=%llu (of them in reflections %llu) | unexpected renderer calls in "
        "pre-runs=%llu (last slot %d) | particles whose shares were recorded=%llu",
        when, s.prepares, s.replayed, s.again, s.replayedAgain, s.replayedAgainShares, double(s.repeatCycles) / 3.0e6 / f,
        s.againChanged, s.againFreed, s.againLocked, s.againComputed, double(s.replayCycles) / 3.0e6 / f,
        double(s.prepareReplayedCycles) / 3.0e6 / f,
        s.notRecorded,
        double(s.prepareOtherCycles) / 3.0e6 / f, s.contextDiffer, s.grantMismatch, s.unused, s.unusedNested,
        s.unexpected, s.unexpectedSlot, s.shareParticles);
    const struct {
        const char* what;
        const Origins* o;
    } origins[] = {{"replayed", &s.replayedFrom}, {"prepared again", &s.againFrom}, {"not recorded", &s.notRecordedFrom}};
    for (const auto& x : origins) {
        if (!x.o->n) continue;
        char line[1024] = "";
        size_t n = 0;
        for (int i = 0; i < x.o->n; ++i) {
            char where[128] = "";
            DescribeAddress(x.o->o[i].caller, where, sizeof where);
            Append(line, sizeof line, n, " %s level %d: %llu;", where, x.o->o[i].level, x.o->o[i].calls);
        }
        if (x.o->other) Append(line, sizeof line, n, " others: %llu", x.o->other);
        Log("particles [%s]: mfPrepare calls %s, by caller and recursion level:%s", when, x.what, line);
    }
    for (int lv = 0; lv < 2; ++lv) {
        char line[1024] = "";
        size_t n = 0;
        for (int k = 0; k < 30; ++k)
            if (s.listPreruns[lv][k])
                Append(line, sizeof line, n, " %d:%llu/%llu", k, s.listPreruns[lv][k], s.listUnused[lv][k]);
        if (n) Log("particles [%s]: level %d pre-runs per render list (list:pre-runs/not used):%s", when, lv + 1, line);
    }
    if (g.verify) {
        char line[256] = "";
        size_t n = 0;
        for (int i = 0; i < 5; ++i)
            if (s.vWhat[i]) Append(line, sizeof line, n, " %s %llu", kWhatNames[i], s.vWhat[i]);
        Log("particles [%s]: VERIFY flush calls compared=%llu: same=%llu (of them reflections' with fill lights only in "
            "verify's own call %llu) differ=%llu%s%s | other context (not compared)=%llu | the flush's calls copying the "
            "particle thread's vertices: %llu of %llu",
            when, s.vCompared, s.vSame, s.vNestedLights, s.vDiffer, n ? ", in:" : "", line, s.vContextDiffer,
            s.flushCopied, s.flushCalls);
        Log("particles [%s]: VERIFY repeated prepares compared=%llu: same=%llu, output differing=%llu, render counters "
            "differing=%llu; of them with the shares of a pre-run that computed the vertices %llu, same %llu | other "
            "context (not compared)=%llu, counters changed since the pre-run (not compared)=%llu",
            when, s.vAgainCompared, s.vAgainSame, s.vAgainOutput, s.vAgainCounters, s.vAgainSharesCompared,
            s.vAgainSharesSame, s.vAgainContextDiffer, s.vAgainChanged);
    }
}
#endif  // _WIN64

}  // namespace

bool ParticlePrepass_Locate(char* base, ParticleTarget* out, const char** why) {
#ifndef _WIN64
    (void)base;
    (void)out;
    *why = "64-bit renderer only";
    return false;
#else
    ParticleTarget t;
    char* prepare = Unique(base, kPrepare, why, "CREParticle::mfPrepare not found");
    char* alloc = prepare ? Unique(base, kAlloc, why, "CAllocRender::Alloc not found") : nullptr;
    char* fill = alloc ? Unique(base, kAddFillLight, why, "EF_ADDFillLight not found") : nullptr;
    if (!fill) return false;
    auto** vt = reinterpret_cast<void**>(Rip(prepare + kPrepareAllocVt, 0));
    char* reserve = RelTarget(alloc + kAllocReserve);
    if (!IsInImage(base, reinterpret_cast<const char*>(vt), sizeof(void*)) ||
        Entry0(base, CallTrace_Original(vt[0])) != alloc) {  // (through the gate's thunk, with overlap)
        *why = "mfPrepare's allocator is not CAllocRender::Alloc";
        return false;
    }
    if (!Matches(base, reserve, kReserve)) {
        *why = "the particle batch's reservation is not as expected";
        return false;
    }
    t.prepare = prepare;
    t.allocVtable = vt;
    t.alloc = alloc;
    t.reserve = reserve;
    t.addFillLight = fill;
    *out = t;
    return true;
#endif
}

bool ParticlePrepass_IsSetVertices(char* e3dBase, char* fn) {
#ifndef _WIN64
    (void)e3dBase;
    (void)fn;
    return false;
#else
    return e3dBase && fn && Matches(e3dBase, fn, kSetVertices);
#endif
}

char* ParticlePrepass_LocateParticleVerts(char* e3dBase, const char** why) {
#ifndef _WIN64
    (void)e3dBase;
    *why = "64-bit 3D engine only";
    return nullptr;
#else
    return LocateParticleVerts(e3dBase, why);
#endif
}

void ParticlePrepass_Install(const wchar_t* moduleName, char* base, const ShadowPrepassTarget& sp) {
#ifndef _WIN64
    (void)base;
    (void)sp;
    if (ReadMode() != PMode::Off) Log("%ls: particles not moved: 64-bit game only", moduleName);
#else
    if (g.installed) return;
    g.mode = ReadMode();
    if (g.mode == PMode::Off) return;
    const char* why = nullptr;
    if (!ParticlePrepass_Locate(base, &g.t, &why)) {
        Log("%ls: particles left in the flush: %s", moduleName, why);
        return;
    }
    g.shadow = &sp;
    g.base = base;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    g.size = nt->OptionalHeader.SizeOfImage;
    g.verify = g.mode == PMode::Verify;
    HookSpec hooks[] = {
        {"CREParticle::mfPrepare", g.t.prepare, reinterpret_cast<void*>(&PrepareHook), reinterpret_cast<void**>(&g.prepare)},
    };
    if (!InstallHooks(hooks, 1)) {
        Log("%ls: particles left in the flush: could not write the hook", moduleName);
        return;
    }
    g.entries.reserve(1024);
    g.installed = true;
    Log("%ls: particles: mode=%s, CREParticle::mfPrepare +0x%X, CAllocRender::Alloc +0x%X (vtable +0x%X), reservation "
        "+0x%X, EF_ADDFillLight +0x%X",
        moduleName, ModeName(g.mode), unsigned(g.t.prepare - base), unsigned(g.t.alloc - base),
        unsigned(reinterpret_cast<char*>(g.t.allocVtable) - base), unsigned(g.t.reserve - base),
        unsigned(g.t.addFillLight - base));
#endif
}

void ParticlePrepass_OnFlushEntry(char* renderer) {
#ifdef _WIN64
    if (!g.installed) return;
    if (!g.setupTried) Setup(renderer);
    if (!g.ready || GetCurrentThreadId() != g.mainThread || renderer != g.renderer) return;
    ++g.st.flushes;
    g.rec.Reset();
    g.entries.clear();
    g.index.Clear();
    g.cams.clear();
    g.shares.clear();
    g.inEntry = true;
#else
    (void)renderer;
#endif
}

void ParticlePrepass_OnNestedLevel(char* renderer, int level) {
#ifdef _WIN64
    if (!g.inEntry || GetCurrentThreadId() != g.mainThread || renderer != g.renderer) return;
    Prerun(renderer, level, false);
#else
    (void)renderer;
    (void)level;
#endif
}

void ParticlePrepass_BeforeFlush(char* renderer) {
#ifdef _WIN64
    if (!g.inEntry || GetCurrentThreadId() != g.mainThread || renderer != g.renderer) return;
    g.inEntry = false;
    Prerun(renderer, 1, true);
    g.inFlush = true;
#else
    (void)renderer;
#endif
}

void ParticlePrepass_AfterFlush() {
#ifdef _WIN64
    if (!g.inFlush) return;
    g.inFlush = false;
    ApplyRepeats();  // (the repeated prepares' counter updates, which the flush leaves to here)
    g.st.bytes += g.rec.ArenaBytes();
    g.st.maxBytes = (std::max)(g.st.maxBytes, g.rec.ArenaBytes());
    for (const Entry& e : g.entries) {
        if (e.state != kRecorded) continue;
        ++g.st.unused;
        if (e.level > 1) ++g.st.unusedNested;
        ++g.st.listUnused[e.level > 1 ? 1 : 0][e.list];
        if (++g.loggedUnused <= 10)
            Log("particles: flush %llu: element %p (level %d) was pre-run but not prepared in the flush", g.st.flushes,
                e.elem, e.level);
    }
#endif
}

void ParticlePrepass_Report(const char* when) {
#ifdef _WIN64
    if (g.installed && g.ready) Report(when);
#else
    (void)when;
#endif
}

}  // namespace optiperf
