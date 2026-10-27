// Environment snapshot: Phase 2 of research/docs/cpu-multithreading-plan.md, after the shadow casters, reflections and
// far trees (details in research/docs/environment-snapshot.md). x64 only.
//
// With those out of the flush, most of what the flush still calls in the 3D engine is its environment: while drawing,
// the renderer reads I3DEngine getters (the shader parameters' sun, sky and fog colours and the sun's direction, the
// view distance, the water level, fog settings, GetGlobalParameter), about 1,300 calls per frame. They return fields
// the 3D engine sets in its update and while traversing the scene, all of it before the flush; a render thread can't
// call them while the main thread runs the next frame's update.
//   - on: right before the flush (after the shadow pre-pass and the reflections' dry run) each getter is called once
//     (GetWaterLevel without a position; GetGlobalParameter for each parameter, noting which components it writes),
//     and TraceFogVolumes, which takes a position, gets a snapshot of what it reads: whether any fog volume can
//     contribute this frame (its cvars, and the list of fog volumes the outer traversal has filled by then) and if
//     so copies of those fog volumes, the camera box and the 3D engine's camera position (fog_snapshot.h).
//     The 3D engine's vtable pointer is swapped for a counting copy (vtable_counter.h) whose getter slots answer
//     from that capture until the flush returns: TraceFogVolumes with no fog volume none of their colour, all of the
//     light through; else the 3D engine's own computation, transcribed, over the copies (inside a replayed
//     reflection's flush from the reflection's camera, where vanilla's traversal leaves the 3D engine's camera).
//     The positional calls (GetWaterLevel and GetOceanWaterLevel at a position, GetVisAreaFromPos, and the sun shafts'
//     IsConnectedToOutdoor and IsAffectedByOutLights on that vis area, CVisArea's slots patched for those two calls)
//     are captured at the renderer's camera position (+0x28F44, which all of them pass) and answered there only. The
//     ocean's height there moves inside the flush with the FFT ocean (the renderer steps the grid it is sampled from
//     when it draws the ocean), so on that branch it is computed when asked for, from the grid as it is then
//     (ocean_snapshot.h); GetWaterLevel there is the higher of that and the capture's water volumes (the same call
//     with the ocean's height a NaN, which its fold drops). Both are checked against the 3D engine's own answers at
//     each capture. DebugDraw_PushFrustrum is answered as the no-op it is while its debug cvar is neither 8 nor 9.
//     Calls from other threads, positional calls elsewhere, and every call after something that could change these
//     fields ran inside the flush go to the 3D engine: its update, a level load or one of its setters (their slots
//     set a marker), or a scene traversal (the reflection part counts those it doesn't replay; without it, any
//     RenderWorld call).
//   - verify: as on, but each call it would answer also goes to the 3D engine, whose result is compared and returned
//     (TraceFogVolumes inside a replayed reflection's flush: with the reflection's camera position put in the 3D
//     engine's camera for the call, as vanilla has it there).
// Each getter's code is checked by pattern at the first flush (the whole function for the small ones, the start of
// the others); one whose code differs is left to the 3D engine.
//
// [ShadowPrepass] Environment = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_ENVIRONMENT)

#include "env_snapshot.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <utility>

#include "calltrace.h"
#include "config.h"
#include "diag.h"
#include "flush_thread.h"
#include "fog_snapshot.h"
#include "log.h"
#include "ocean_snapshot.h"
#include "reflection_prepass.h"
#include "scan.h"
#include "vtable_counter.h"

namespace optiperf {
namespace {

enum class EMode { Off, On, Verify };

EMode ReadMode() {
    const std::wstring v =
        ConfigString(L"ShadowPrepass", L"Environment", L"OPTIPERF_SHADOW_PREPASS_ENVIRONMENT", L"off");
    if (_wcsicmp(v.c_str(), L"on") == 0 || v == L"1") return EMode::On;
    if (_wcsicmp(v.c_str(), L"verify") == 0) return EMode::Verify;
    return EMode::Off;
}

#ifdef _WIN64
const char* ModeName(EMode m) { return m == EMode::On ? "on" : m == EMode::Verify ? "verify" : "off"; }

// How a getter returns its answer (x64; `self` is the I3DEngine).
enum class Kind : uint8_t {
    Byte,      // uint8 fn(self)
    Int,       // int fn(self)
    Float,     // float fn(self)
    Vec3,      // Vec3 fn(self): into a hidden pointer, which it returns
    Vec4,      // Vec4 fn(self): the same
    Out2,      // void fn(self, float&, float&)
    Out4,      // void fn(self, float&, float&, float&, float&)
    Vec4Pair,  // void fn(self, Vec4&, Vec4&)
    Water,     // float fn(self, const Vec3* pos, bool accurate): answered without a position only
    Global,    // void fn(self, E3DEngineParameter, Vec3&): per parameter, the components it writes
    Fog,       // void fn(self, const Vec3& pos, ColorF&): from a snapshot of the fog volumes
    Ptr,       // void* fn(self)
    OceanAt,   // float fn(self, const Vec3& pos): answered at the renderer's camera position
    VisAreaAt, // IVisArea* fn(self, const Vec3& pos): the same
    DebugPush, // void fn(self, name, frustum, colour, float): a no-op unless a debug cvar is 8 or 9
};

struct Def {
    int slot;
    const char* name;
    Kind kind;
    const char* code;    // its code: the whole function, or its start
    const char* jumpTo;  // for a jmp thunk: the code it jumps to
    int jumpAt;          // ... and where in `code` its jmp is
};

// --- patterns (x64 Cry3DEngine.dll, build 6156): the I3DEngine slots (Mod SDK order) and C3DEngine's functions there ----
constexpr Def kDefs[kEnvGetters] = {
    // the ocean's render flags (+0x741); its visible pixel count (a static the ocean sets while it renders)
    {22, "GetOceanRenderFlags", Kind::Byte, "8A 81 41 07 00 00 C3", nullptr},
    {23, "GetOceanVisiblePixelsCount", Kind::Int, "E9 ?? ?? ?? ??", "8B 05 ?? ?? ?? ?? C3"},
    // without a position the ocean's level (the terrain's +0x1E0); with one, the highest of the ocean's height there
    // (outside vis areas: slot 28, through the vtable) and the water volumes physics finds there (answered at the
    // renderer's camera position)
    {27, "GetWaterLevel", Kind::Water,
     "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 48 81 EC C0 01 00 00 8B 05 ?? ?? ?? ?? 33 DB", nullptr},
    // the ocean's height at a position: the terrain's water level and the waves (ocean_snapshot.h)
    {28, "GetOceanWaterLevel", Kind::OceanAt,
     "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 8B 05 ?? ?? ?? ?? 33 FF 0F 29 74 24 40 A8 01 48 8B DA 48 8D 35 ?? ?? "
     "?? ??",
     nullptr},
    {29, "GetCausticsParams", Kind::Vec4,
     "8B 81 3C 01 00 00 C7 02 00 00 80 3F 89 42 04 8B 81 40 01 00 00 89 42 08 8B 81 44 01 00 00 89 42 0C 48 8B C2 C3",
     nullptr},
    // the ocean's wave parameters and the water level, worked out once per 3D-engine frame
    {30, "GetOceanAnimationParams", Kind::Vec4Pair,
     "48 89 5C 24 10 48 89 6C 24 18 57 41 54 41 55 48 83 EC 40 8B 05 ?? ?? ?? ?? 8B 2D ?? ?? ?? ?? 0F 57 C0", nullptr},
    // from cvars, the view camera's height above the water, and two of the 3D engine's distances
    {45, "GetMaxViewDistance", Kind::Float,
     "40 53 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 0F 29 74 24 50 0F 29 7C 24 40 0F 57 FF 48 8B D9 44 0F 29 44 24 30",
     nullptr},
    {47, "GetFogColor", Kind::Vec3, "8B 41 3C 89 02 8B 41 40 89 42 04 8B 41 44 89 42 08 48 8B C2 C3", nullptr},
    {48, "GetVolumetricFogSettings", Kind::Out4,
     "8B 41 1C 89 02 8B 51 20 48 8B 44 24 28 89 10 8B 41 24 41 89 00 8B 41 28 41 89 01 C3", nullptr},
    {50, "GetVolumetricFogModifiers", Kind::Out2, "8B 41 2C 89 02 8B 41 30 41 89 00 C3", nullptr},
    {54, "GetCloudShadingMultiplier", Kind::Out2, "8B 41 34 89 02 8B 41 38 41 89 00 C3", nullptr},
    {57, "GetHDRDynamicMultiplier", Kind::Float, "F3 0F 10 81 0C 04 00 00 C3", nullptr},
    // without `this`, CFogVolumeRenderNode::TraceFogVolumes(pos, colour): the fog volumes near the camera that the ray
    // from the camera box's centre towards the position hits (fog_snapshot.h, which checks its code); with none,
    // (0, 0, 0, 1)
    {59, "TraceFogVolumes", Kind::Fog, "48 8B CA 49 8B D0 E9 ?? ?? ?? ??",
     "48 8B C4 48 81 EC C8 00 00 00 48 89 58 08 48 89 68 10 48 89 78 20 4C 89 68 F0 4C 89 70 E8 45 33 F6 0F 29 70 D8 "
     "8B 05 ?? ?? ?? ?? 48 8B DA 48 8B E9 A8 01",
     6},
    {64, "GetTerrainSize", Kind::Int, "8B 05 ?? ?? ?? ?? C3", nullptr},
    {86, "GetSunDir", Kind::Vec3, "8B 41 54 89 02 8B 41 58 89 42 04 8B 41 5C 89 42 08 48 8B C2 C3", nullptr},
    {87, "GetSunDirNormalized", Kind::Vec3, "8B 41 60 89 02 8B 41 64 89 42 04 8B 41 68 89 42 08 48 8B C2 C3", nullptr},
    {88, "GetRealtimeSunDirNormalized", Kind::Vec3,
     "8B 41 70 89 02 8B 41 74 89 42 04 8B 41 78 89 42 08 48 8B C2 C3", nullptr},
    // from the 3D engine's camera, the terrain's sectors and the water level
    {90, "GetDistanceToSectorWithWater", Kind::Float,
     "40 53 48 83 EC 30 4C 8B 15 ?? ?? ?? ?? 48 8B D9 4D 85 D2 74 77 F3 0F 2C 05 ?? ?? ?? ?? F3 44 0F 2C 1D ?? ?? ?? ??",
     nullptr},
    // the object manager's sky and sun colours, sky brightness and SSAO amount (which the time of day sets)
    {91, "GetSkyColor", Kind::Vec3,
     "48 83 EC 18 48 8B 05 ?? ?? ?? ?? 48 85 C0 74 09 48 8D 88 B8 00 00 00 EB 18 0F 57 C0 48 8D 0C 24", nullptr},
    {92, "GetSunColor", Kind::Vec3,
     "48 83 EC 18 48 8B 05 ?? ?? ?? ?? 48 85 C0 74 09 48 8D 88 C8 00 00 00 EB 18 0F 57 C0 48 8D 0C 24", nullptr},
    {93, "GetSkyBrightness", Kind::Float,
     "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 09 F3 0F 10 80 DC 00 00 00 C3 F3 0F 10 05 ?? ?? ?? ?? C3", nullptr},
    {94, "GetSSAOAmount", Kind::Float,
     "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 09 F3 0F 10 80 E0 00 00 00 C3 F3 0F 10 05 ?? ?? ?? ?? C3", nullptr},
    // the vis area a position is in: the vis-area manager's (the 3D engine's +0x22B910), if there is one
    {112, "GetVisAreaFromPos", Kind::VisAreaAt,
     "48 83 3D ?? ?? ?? ?? 00 74 11 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 05 E9 ?? ?? ?? ?? 33 C0 C3", nullptr},
    // a static: the material manager (the layer passes ask it for the default layers material)
    {116, "GetMaterialManager", Kind::Ptr, "48 8B 05 ?? ?? ?? ?? C3", nullptr},
    // a switch over parameters 1..0x2B (its jump table: param - 1 <= 0x2A): fields of the 3D engine, 1 and 3 through
    // GetSunColor and GetSkyColor; the others write nothing
    {163, "GetGlobalParameter", Kind::Global,
     "40 53 48 83 EC 40 83 C2 FF 49 8B D8 83 FA 2A 0F 87 ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 48 63 C2 41 8B 94 80 ?? ?? ?? "
     "??",
     nullptr},
    // a frustum for the debug view: does nothing unless the cvars' +0xE4 is 8 or 9 (its check, and the cvars pointer at
    // +0x10)
    {167, "DebugDraw_PushFrustrum", Kind::DebugPush,
     "48 89 5C 24 08 57 48 81 EC 10 01 00 00 48 8B 05 ?? ?? ?? ?? 41 8B D9 48 8B FA 8B 88 E4 00 00 00 83 F9 08 74 09 83 "
     "F9 09 0F 85 ?? ?? ?? ??",
     nullptr},
};
constexpr int IndexOf(Kind k) {
    for (int i = 0; i < kEnvGetters; ++i)
        if (kDefs[i].kind == k) return i;
    return -1;
}
constexpr int kWater = IndexOf(Kind::Water), kOcean = IndexOf(Kind::OceanAt);  // (GetWaterLevel, GetOceanWaterLevel)
static_assert(kWater >= 0 && kOcean >= 0, "the positional water getters");
constexpr int kPushCvars = 0x10, kPushCvar = 0xE4;  // in DebugDraw_PushFrustrum: the cvars pointer, the cvar it checks
constexpr int kRCamPos = 0x28F44;  // the renderer's camera position, which the positional calls pass (kSunShafts)
constexpr int kParamFirst = 1, kParamLast = 0x2B;
constexpr size_t kMaxFogVolumes = 1024;  // more visible fog volumes than this in a frame: left to the 3D engine
// Slots whose call inside the flush, on its thread, makes the snapshot out of date (another thread's call, such as the
// main thread's next frame beside a render thread's flush, leaves it): the 3D engine's update and frame start, level
// loading and saving, its flags, the terrain, and the setters of what the getters return (the time of day calls them
// too); after I3DEngine's 178, two of C3DEngine's own: SetCamera (the camera GetDistanceToSectorWithWater reads) and
// the object manager's SSAO amount. The scene traversal, RenderWorld, is counted by the reflection part, which
// replays most of them; without it, its slot is one of these.
constexpr int kStaleSlots[] = {1,  4,  7,  8,  9,  10, 11,  21,  41,  42,  43,  46,  49,  51,
                               53, 55, 58, 81, 82, 83, 115, 148, 149, 154, 155, 162, 178, 181};
constexpr int kStaleCount = int(sizeof kStaleSlots / sizeof kStaleSlots[0]);
constexpr int kSlotRenderWorld = 2;
constexpr int kMaxSlots = 256;         // C3DEngine's vtable: I3DEngine's 178 slots, and a few of its own
constexpr int kVfGetI3DEngine = 0xE0;  // ISystem

// for the report: the other slots the flush calls (renderer-boundary.md §10), and the ones above
struct SlotName {
    int slot;
    const char* name;
};
constexpr SlotName kSlotNames[] = {
    {1, "Update"},          {2, "RenderWorld"},        {4, "SetFlags"},
    {11, "OnFrameStart"},   {20, "IsUnderWater"},      {21, "SetOceanRenderFlags"},
    {28, "GetOceanWaterLevel"}, {39, "DrawFarTrees"},  {40, "GenerateFarTrees"},
    {41, "SetSkyColor"},    {42, "SetSunColor"},       {43, "SetSkyBrightness"},
    {46, "SetFogColor"},    {49, "SetVolumetricFogSettings"}, {51, "SetVolumetricFogModifiers"},
    {52, "GetSkyLightParameters"}, {53, "SetSkyLightParameters"}, {55, "SetCloudShadingMultiplier"},
    {56, "GetClosestAutoCubeMap"}, {58, "SetHDRDynamicMultiplier"}, {59, "TraceFogVolumes"},
    {60, "GetTerrainElevation"}, {61, "GetTerrainZ"},  {81, "SetupDistanceFog"},
    {89, "GetAmbientColorFromPosition"}, {95, "GetSunShadowIntensity"}, {111, "GetWind"},
    {112, "GetVisAreaFromPos"}, {115, "EnableOceanRendering"}, {116, "GetMaterialManager"},
    {127, "GetPostEffectParam"}, {161, "GetTimeOfDay"}, {162, "SetGlobalParameter"},
    {167, "DebugDraw_PushFrustrum"}, {168, "DebugDraw_PushFrustrum"}, {178, "SetCamera"},
    {181, "SetSSAOAmount"},
};

const char* SlotLabel(int slot) {
    for (const Def& d : kDefs)
        if (d.slot == slot) return d.name;
    for (const SlotName& s : kSlotNames)
        if (s.slot == slot) return s.name;
    return "";
}

struct Getter {
    void* orig = nullptr;  // the vtable's entry: the 3D engine's function (or the tracer's thunk in front of it)
    bool hooked = false;
    uint32_t v[8] = {};    // the captured answer, as raw words
    // main-thread calls inside the flush: answered; to the 3D engine as out of date, or with a position (or a
    // parameter out of range); verify: compared
    uint64_t answered = 0, passed = 0, aside = 0, same = 0, differ = 0;
    uint64_t nested = 0;  // ... of those answered or compared, inside a replayed reflection's flush
};

struct EState {
    EMode mode = EMode::Off;
    bool installed = false, setupTried = false, ready = false, verify = false;
    bool swapped = false;  // the vtable copy is in (from the capture to the flush's end)
    bool live = false;     // ... and the snapshot is still up to date
    void** system = nullptr;
    void* engine = nullptr;
    DWORD mainThread = 0;
    VtableCounter counter;
    Getter get[kEnvGetters];
    uint32_t global[kParamLast + 1][3] = {};  // GetGlobalParameter's answers, and the components each one writes
    uint8_t globalMask[kParamLast + 1] = {};
    uint64_t globalCalls[kParamLast + 1] = {};
    fog::Target fog;          // TraceFogVolumes' inputs and code (fog_snapshot.h)
    fog::Snapshot fogSnap;    // this flush's fog volumes (while some can contribute)
    bool fogSnapOk = false;
    uint64_t fogFrames = 0, fogVolumes = 0, fogBad = 0, fogTraced = 0, fogReplayCam = 0;
    size_t fogMax = 0;
    int loggedFog = 0;
    const char* fogWhy = nullptr;  // why TraceFogVolumes' code is not the expected one (EnvSnapshot_Locate)
    // the positional calls: the renderer's camera position at the capture (they are answered there only)
    float camPos[3] = {};
    bool camPosOk = false, camOffsetOk = false;
    // the water there (ocean_snapshot.h): the ocean's code; this flush's branch and inputs, whether both water getters
    // can be answered there (the capture's checks passed), whether GetWaterLevel takes the ocean's height (outside vis
    // areas) and its water volumes' share (the same call with the ocean's height a NaN: `sentinel`, which slot 28's
    // hook answers on the main thread; those calls through the vtable copy are left out of the report's counts)
    ocean::Target ocean;
    bool oceanFound = false;
    ocean::Inputs oceanIn;
    bool waterAt = false, waterOcean = false, sentinel = false;
    uint32_t waterVolumes = 0;
    uint64_t sentinelCalls = 0, oceanBranch[3] = {}, oceanHeights = 0, oceanBad = 0, waterBad = 0, waterOceanFlushes = 0;
    int loggedWater = 0;
    char* const* pushCvars = nullptr;  // DebugDraw_PushFrustrum's cvars pointer
    // the vis area at the camera position, and its two answers the sun shafts ask for (CVisArea's slots 2 and 7,
    // patched; answered for those two calls only, by their return addresses)
    void* visArea = nullptr;
    bool visOutdoor = false, visOutLights = false, visReady = false;
    void* visOrig[2] = {};
    char* visRet[2] = {};
    uint64_t visAnswered[2] = {}, visSame = 0, visDiffer = 0;  // (answered per slot: in verify, compared)
    const uint64_t* sceneRuns = nullptr;  // the reflection part's count of the scene traversals run in the flush
    uint64_t scenesAtCapture = 0;
    uint64_t setterCalls[kStaleCount] = {};  // their calls by the capture (to name the one that ended a snapshot)
    uint64_t worldsAtCapture = 0;
    std::atomic<uint64_t> otherThread{0};
    uint64_t flushes = 0, captureCycles = 0, stale = 0, staleSetter = 0, staleScene = 0;
    int logged = 0, loggedStale = 0;
    bool loggedFirst = false;
};
EState g;

inline float* F(uint32_t* w) { return reinterpret_cast<float*>(w); }
inline uint32_t Bits(float f) {
    uint32_t w;
    std::memcpy(&w, &f, 4);
    return w;
}
inline float AsFloat(uint32_t w) {
    float f;
    std::memcpy(&f, &w, 4);
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

int Words(Kind k) {
    switch (k) {
    case Kind::Vec3: return 3;
    case Kind::Vec4: case Kind::Out4: return 4;
    case Kind::Out2: case Kind::Ptr: case Kind::Water: case Kind::VisAreaAt: return 2;
    case Kind::Vec4Pair: return 8;
    case Kind::Global: return 0;
    default: return 1;  // (Fog: whether no fog volume can contribute)
    }
}

// TraceFogVolumes' answer with no fog volume: none of their colour, all of the light through (its alpha: 1 - 0)
constexpr uint32_t kNoFog[4] = {0, 0, 0, 0x3F800000};

// Whether no fog volume can contribute this frame: TraceFogVolumes then skips its loop (a cvar off) or walks nothing.
bool NoFogVolumes() {
    const char* cvars = *g.fog.cvars;
    return !cvars || !*reinterpret_cast<const int*>(cvars + fog::kCvarFog) ||
           !*reinterpret_cast<const int*>(cvars + fog::kCvarFogVolumes) || *g.fog.begin == *g.fog.end;
}

// The frame's visible fog volumes, the camera box and the camera position, as TraceFogVolumes reads them.
void TakeFogSnapshot() {
    fog::Snapshot& s = g.fogSnap;
    g.fogSnapOk = false;
    const char* b = *g.fog.begin;
    const char* e = *g.fog.end;
    const size_t bytes = size_t(e - b);
    if (!b || e < b || bytes % fog::kEntryBytes || bytes / fog::kEntryBytes > kMaxFogVolumes) {
        ++g.fogBad;
        return;
    }
    const size_t count = bytes / fog::kEntryBytes;
    s.nodes.resize(count * fog::kNodeBytes);
    for (size_t i = 0; i < count; ++i) {
        const char* node = *reinterpret_cast<char* const*>(b + i * fog::kEntryBytes);
        if (!node) {
            ++g.fogBad;
            return;
        }
        std::memcpy(s.nodes.data() + i * fog::kNodeBytes, node, fog::kNodeBytes);
    }
    std::memcpy(s.box, g.fog.box, sizeof s.box);
    for (int i = 0; i < 3; ++i) s.cam[i] = *g.fog.cam[i];
    s.count = count;
    g.fogSnapOk = true;
    ++g.fogFrames;
    g.fogVolumes += count;
    g.fogMax = (std::max)(g.fogMax, count);
}

void AppendWords(char* line, size_t size, size_t& n, Kind k, const uint32_t* w, int words) {
    if (k == Kind::Ptr || k == Kind::VisAreaAt) {
        Append(line, size, n, " (0x%08X%08X)", w[1], w[0]);
        return;
    }
    Append(line, size, n, " (");
    for (int i = 0; i < words; ++i) {
        if (k == Kind::Byte || k == Kind::Int || k == Kind::Fog || k == Kind::DebugPush)
            Append(line, size, n, "%s%d", i ? ", " : "", int(w[i]));
        else
            Append(line, size, n, "%s%.9g [%08X]", i ? ", " : "", double(AsFloat(w[i])), w[i]);
    }
    Append(line, size, n, ")");
}

uint64_t SetterCalls(int k) { return g.counter.Inside(kStaleSlots[k]) + g.counter.Outside(kStaleSlots[k]); }
uint64_t WorldCalls() { return g.counter.Inside(kSlotRenderWorld) + g.counter.Outside(kSlotRenderWorld); }

// The snapshot just went out of date: the rest of this flush asks the 3D engine.
void GoneStale() {
    g.live = false;
    ++g.stale;
    int slot = -1;
    const bool setter = g.counter.Marked();
    if (setter) {
        ++g.staleSetter;
        for (int k = 0; k < kStaleCount && slot < 0; ++k)
            if (SetterCalls(k) != g.setterCalls[k]) slot = kStaleSlots[k];
        if (slot < 0 && WorldCalls() != g.worldsAtCapture) slot = kSlotRenderWorld;
    } else {
        ++g.staleScene;
    }
    if (++g.loggedStale <= 10)
        Log("environment: flush %llu: out of date after %s%s (slot %d): the rest of the flush asks the 3D engine",
            g.flushes, setter ? "a call of I3DEngine::" : "a scene traversal in the flush", setter ? SlotLabel(slot) : "",
            slot);
}

// the main thread or the flush's (they differ with FlushThread=worker); other threads' calls go to the 3D engine,
// counted
bool OnMain() {
    const DWORD t = GetCurrentThreadId();
    if (t == g.mainThread || t == FlushThread()) return true;
    g.otherThread.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// Whether a call is answered from the snapshot: on the flush's thread while it is live (else it goes to the 3D engine,
// counted).
bool Answers(Getter& x) {
    if (g.live && (g.counter.Marked() || (g.sceneRuns && *g.sceneRuns != g.scenesAtCapture))) GoneStale();
    if (g.live && OnFlushThread()) {
        if (ReflectionPrepass_ReplayCamera()) ++x.nested;
        return true;
    }
    ++x.passed;
    return false;
}

// verify: the 3D engine's answer against the snapshot's
void Check(Getter& x, int i, const uint32_t* live, const uint32_t* snap, int words, int param, void* from) {
    if (std::memcmp(live, snap, size_t(words) * 4) == 0) {
        ++x.same;
        return;
    }
    ++x.differ;
    if (++g.logged > 20) return;
    const Kind k = kDefs[i].kind == Kind::Global || kDefs[i].kind == Kind::Fog ? Kind::Vec3 : kDefs[i].kind;  // floats
    char line[768] = "";
    size_t n = 0;
    Append(line, sizeof line, n, " snapshot");
    AppendWords(line, sizeof line, n, k, snap, words);
    Append(line, sizeof line, n, " | 3D engine");
    AppendWords(line, sizeof line, n, k, live, words);
    char where[160] = "";
    DescribeAddress(from, where, sizeof where);
    char what[64] = "";
    if (param >= 0) snprintf(what, sizeof what, "(0x%X)", param);
    Log("environment: VERIFY flush %llu: I3DEngine::%s%s differs (called from %s):%s", g.flushes, kDefs[i].name, what,
        where, line);
}

// --- the getters' slots in the vtable copy --------------------------------------------------------------------------
template <int I>
uint8_t ByteHook(void* self) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<uint8_t (*)(void*)>(x.orig);
    if (!OnMain() || !Answers(x)) return fn(self);
    if (g.verify) {
        const uint8_t r = fn(self);
        const uint32_t w = r;
        Check(x, I, &w, x.v, 1, -1, _ReturnAddress());
        return r;
    }
    ++x.answered;
    return uint8_t(x.v[0]);
}

template <int I>
int IntHook(void* self) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<int (*)(void*)>(x.orig);
    if (!OnMain() || !Answers(x)) return fn(self);
    if (g.verify) {
        const int r = fn(self);
        const uint32_t w = uint32_t(r);
        Check(x, I, &w, x.v, 1, -1, _ReturnAddress());
        return r;
    }
    ++x.answered;
    return int(x.v[0]);
}

template <int I>
float FloatHook(void* self) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<float (*)(void*)>(x.orig);
    if (!OnMain() || !Answers(x)) return fn(self);
    if (g.verify) {
        const float r = fn(self);
        const uint32_t w = Bits(r);
        Check(x, I, &w, x.v, 1, -1, _ReturnAddress());
        return r;
    }
    ++x.answered;
    return AsFloat(x.v[0]);
}

template <int I, int N>
float* VecHook(void* self, float* out) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<float* (*)(void*, float*)>(x.orig);
    if (!OnMain() || !Answers(x)) return fn(self, out);
    if (g.verify) {
        float* r = fn(self, out);
        uint32_t w[N];
        std::memcpy(w, out, sizeof w);
        Check(x, I, w, x.v, N, -1, _ReturnAddress());
        return r;
    }
    ++x.answered;
    std::memcpy(out, x.v, N * 4);
    return out;
}

template <int I, int W>  // two outputs of W floats each
void PairHook(void* self, float* a, float* b) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<void (*)(void*, float*, float*)>(x.orig);
    if (!OnMain() || !Answers(x)) return fn(self, a, b);
    if (g.verify) {
        fn(self, a, b);
        uint32_t w[2 * W];
        std::memcpy(w, a, W * 4);
        std::memcpy(w + W, b, W * 4);
        Check(x, I, w, x.v, 2 * W, -1, _ReturnAddress());
        return;
    }
    ++x.answered;
    std::memcpy(a, x.v, W * 4);
    std::memcpy(b, x.v + W, W * 4);
}

template <int I>
void Out4Hook(void* self, float* a, float* b, float* c, float* d) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<void (*)(void*, float*, float*, float*, float*)>(x.orig);
    if (!OnMain() || !Answers(x)) return fn(self, a, b, c, d);
    if (g.verify) {
        fn(self, a, b, c, d);
        const uint32_t w[4] = {Bits(*a), Bits(*b), Bits(*c), Bits(*d)};
        Check(x, I, w, x.v, 4, -1, _ReturnAddress());
        return;
    }
    ++x.answered;
    *a = AsFloat(x.v[0]);
    *b = AsFloat(x.v[1]);
    *c = AsFloat(x.v[2]);
    *d = AsFloat(x.v[3]);
}

// whether a position is the renderer's camera position as captured (the positional calls are answered there only)
bool AtCamPos(const float* pos) { return g.camPosOk && pos && std::memcmp(pos, g.camPos, sizeof g.camPos) == 0; }

// The ocean's height at the camera position: on the FFT branch from the render element's grid as it is now (the
// renderer steps it inside the flush), else the capture's.
float OceanHeight() {
    if (g.oceanIn.branch != ocean::Branch::Fft) return AsFloat(g.get[kOcean].v[0]);
    ++g.oceanHeights;
    return ocean::Height(g.oceanIn, g.camPos, g.ocean.floor);
}

// GetWaterLevel's fold over its candidates, the ocean's height first: a later one wins a tie (and over a NaN). Its
// floor (WATER_LEVEL_UNKNOWN) is already in the water volumes' share.
inline float Higher(float ocean, float volumes) { return ocean > volumes ? ocean : volumes; }

template <int I>
float WaterHook(void* self, const float* pos, bool accurate) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<float (*)(void*, const float*, bool)>(x.orig);
    if (!OnMain()) return fn(self, pos, accurate);
    if (pos) {  // at a position (water volumes too): answered at the camera position
        if (accurate || !AtCamPos(pos) || !g.waterAt) {
            ++x.aside;
            return fn(self, pos, accurate);
        }
        if (!Answers(x)) return fn(self, pos, accurate);
        const float mine = g.waterOcean ? Higher(OceanHeight(), AsFloat(g.waterVolumes)) : AsFloat(x.v[1]);
        if (g.verify) {
            const float r = fn(self, pos, accurate);
            const uint32_t w = Bits(r), s = Bits(mine);
            Check(x, I, &w, &s, 1, -1, _ReturnAddress());
            return r;
        }
        ++x.answered;
        return mine;
    }
    if (!Answers(x)) return fn(self, pos, accurate);
    if (g.verify) {
        const float r = fn(self, pos, accurate);
        const uint32_t w = Bits(r);
        Check(x, I, &w, x.v, 1, -1, _ReturnAddress());
        return r;
    }
    ++x.answered;
    return AsFloat(x.v[0]);
}

template <int I>
void GlobalHook(void* self, int param, float* out) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<void (*)(void*, int, float*)>(x.orig);
    if (!OnMain()) return fn(self, param, out);
    if (param < kParamFirst || param > kParamLast) {
        ++x.aside;
        return fn(self, param, out);
    }
    ++g.globalCalls[param];
    if (!Answers(x)) return fn(self, param, out);
    const uint8_t m = g.globalMask[param];
    if (g.verify) {
        fn(self, param, out);
        uint32_t w[3], s[3];
        int n = 0;
        for (int k = 0; k < 3; ++k)
            if (m & (1 << k)) {
                w[n] = Bits(out[k]);
                s[n++] = g.global[param][k];
            }
        Check(x, I, w, s, n, param, _ReturnAddress());
        return;
    }
    ++x.answered;
    for (int k = 0; k < 3; ++k)
        if (m & (1 << k)) out[k] = AsFloat(g.global[param][k]);
}

template <int I>
float OceanHook(void* self, const float* pos) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<float (*)(void*, const float*)>(x.orig);
    if (g.sentinel && GetCurrentThreadId() == g.mainThread) {  // the capture's water volumes alone (CaptureWaterAt)
        ++g.sentinelCalls;
        return std::numeric_limits<float>::quiet_NaN();
    }
    if (!OnMain()) return fn(self, pos);
    if (!AtCamPos(pos) || !g.waterAt) {
        ++x.aside;
        return fn(self, pos);
    }
    if (!Answers(x)) return fn(self, pos);
    const float mine = OceanHeight();
    if (g.verify) {
        const float r = fn(self, pos);
        const uint32_t w = Bits(r), s = Bits(mine);
        Check(x, I, &w, &s, 1, -1, _ReturnAddress());
        return r;
    }
    ++x.answered;
    return mine;
}

template <int I>
void* VisAreaHook(void* self, const float* pos) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<void* (*)(void*, const float*)>(x.orig);
    if (!OnMain()) return fn(self, pos);
    if (!AtCamPos(pos)) {
        ++x.aside;
        return fn(self, pos);
    }
    if (!Answers(x)) return fn(self, pos);
    void* mine;
    std::memcpy(&mine, x.v, sizeof mine);
    if (g.verify) {
        void* r = fn(self, pos);
        uint32_t w[2];
        std::memcpy(w, &r, sizeof w);
        Check(x, I, w, x.v, 2, -1, _ReturnAddress());
        return r;
    }
    ++x.answered;
    return mine;
}

// the debug cvar DebugDraw_PushFrustrum checks: with it at neither 8 nor 9, the call does nothing
bool PushDoesNothing() {
    const char* cvars = g.pushCvars ? *g.pushCvars : nullptr;
    int c = 0;
    if (cvars) std::memcpy(&c, cvars + kPushCvar, sizeof c);
    return c != 8 && c != 9;
}

template <int I>
void PushHook(void* self, const char* name, const void* frustum, uint32_t colour, float f) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<void (*)(void*, const char*, const void*, uint32_t, float)>(x.orig);
    if (!OnMain() || !x.v[0]) return fn(self, name, frustum, colour, f);  // (v[0]: it does nothing this frame)
    if (!Answers(x)) return fn(self, name, frustum, colour, f);
    if (g.verify) {
        const uint32_t w = PushDoesNothing() ? 1 : 0;
        Check(x, I, &w, x.v, 1, -1, _ReturnAddress());
        return fn(self, name, frustum, colour, f);
    }
    ++x.answered;  // as the 3D engine's call: nothing
}

template <int I>
void* PtrHook(void* self) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<void* (*)(void*)>(x.orig);
    if (!OnMain() || !Answers(x)) return fn(self);
    void* mine;
    std::memcpy(&mine, x.v, sizeof mine);
    if (g.verify) {
        void* r = fn(self);
        uint32_t w[2];
        std::memcpy(w, &r, sizeof w);
        Check(x, I, w, x.v, 2, -1, _ReturnAddress());
        return r;
    }
    ++x.answered;
    return mine;
}

using FogFn = void (*)(void*, const float*, float*);

// verify: why TraceFogVolumes differs (the first few): the snapshot's inputs against the 3D engine's now
void LogFogInputs(const float* pos, const float* cam, bool reflection) {
    const fog::Snapshot& s = g.fogSnap;
    const char* b = *g.fog.begin;
    const char* e = *g.fog.end;
    const size_t now = b && e >= b ? size_t(e - b) / fog::kEntryBytes : 0;
    size_t differ = 0, first = 0;
    int at = -1;
    for (size_t i = 0; i < s.count && i < now; ++i) {
        const auto* node = *reinterpret_cast<const uint8_t* const*>(b + i * fog::kEntryBytes);
        const uint8_t* copy = s.nodes.data() + i * fog::kNodeBytes;
        if (!node || std::memcmp(node, copy, fog::kNodeBytes) == 0) continue;
        if (differ++) continue;
        first = i;
        for (size_t k = 0; k < fog::kNodeBytes && at < 0; ++k)
            if (node[k] != copy[k]) at = int(k);
    }
    Log("environment: VERIFY flush %llu: TraceFogVolumes at (%.9g %.9g %.9g) from the %s camera (%.9g %.9g %.9g; the 3D "
        "engine's now: %.9g %.9g %.9g): fog volumes %zu (now %zu), camera box %s, nodes differing %zu (first: %zu at "
        "+0x%X)",
        g.flushes, double(pos[0]), double(pos[1]), double(pos[2]), reflection ? "replayed reflection's" : "snapshot's",
        double(cam[0]), double(cam[1]), double(cam[2]), double(*g.fog.cam[0]), double(*g.fog.cam[1]),
        double(*g.fog.cam[2]), s.count, now, std::memcmp(s.box, g.fog.box, sizeof s.box) ? "differs" : "same", differ,
        first, unsigned(at));
}

// verify, inside a replayed reflection's flush: the 3D engine's answer with its camera where vanilla's traversal left
// it for that flush (the reflection's: TraceFogVolumes reads only its position), then the frame's camera put back
void LiveFogAt(FogFn fn, void* self, const float* pos, float* out, const float* cam) {
    float* c[3] = {const_cast<float*>(g.fog.cam[0]), const_cast<float*>(g.fog.cam[1]), const_cast<float*>(g.fog.cam[2])};
    const float was[3] = {*c[0], *c[1], *c[2]};
    for (int i = 0; i < 3; ++i) *c[i] = cam[i];
    fn(self, pos, out);
    for (int i = 0; i < 3; ++i) *c[i] = was[i];
}

// TraceFogVolumes: with no fog volume that can contribute (the capture's v[0]), the constant; else from the snapshot
// of the frame's fog volumes (fog_snapshot.h), seen from the 3D engine's camera as vanilla has it at this call: the
// frame's, or inside a replayed reflection's flush the reflection's (its traversal, which the replay skips, sets it)
template <int I>
void FogHook(void* self, const float* pos, float* out) {
    Getter& x = g.get[I];
    const auto fn = reinterpret_cast<FogFn>(x.orig);
    if (!OnMain()) return fn(self, pos, out);
    if (!x.v[0] && !g.fogSnapOk) {  // fog volumes in view, but no snapshot of them
        ++x.aside;
        return fn(self, pos, out);
    }
    if (!Answers(x)) return fn(self, pos, out);
    float mine[4], cam[3];
    const uint8_t* reflection = ReflectionPrepass_ReplayCamera();
    if (reflection) {
        std::memcpy(&cam[0], reflection + 0xC, 4);
        std::memcpy(&cam[1], reflection + 0x1C, 4);
        std::memcpy(&cam[2], reflection + 0x2C, 4);
    } else {
        std::memcpy(cam, g.fogSnap.cam, sizeof cam);
    }
    if (x.v[0]) {
        std::memcpy(mine, kNoFog, sizeof mine);
    } else {
        fog::Trace(g.fogSnap, cam, pos, mine, g.fog.math);
        ++g.fogTraced;
        if (reflection) ++g.fogReplayCam;
    }
    if (g.verify) {
        if (reflection && !x.v[0])
            LiveFogAt(fn, self, pos, out, cam);
        else
            fn(self, pos, out);
        uint32_t w[4], s[4];
        std::memcpy(w, out, sizeof w);
        std::memcpy(s, mine, sizeof s);
        if (std::memcmp(w, s, sizeof w) && !x.v[0] && ++g.loggedFog <= 10) LogFogInputs(pos, cam, reflection);
        Check(x, I, w, s, 4, -1, _ReturnAddress());
        return;
    }
    ++x.answered;
    std::memcpy(out, mine, sizeof mine);
}

// CVisArea's IsConnectedToOutdoor (slot 2) and IsAffectedByOutLights (slot 7), called by the sun shafts on the vis
// area at the camera position: answered from the capture
template <int S>
bool VisFlagHook(char* self) {
    const auto fn = reinterpret_cast<bool (*)(char*)>(g.visOrig[S]);
    if (_ReturnAddress() != g.visRet[S] || !g.live || self != g.visArea || !OnFlushThread())
        return fn(self);
    const bool mine = S == 0 ? g.visOutdoor : g.visOutLights;
    ++g.visAnswered[S];  // (verify: compared)
    if (g.verify) {
        const bool live = fn(self);
        ++(live == mine ? g.visSame : g.visDiffer);
        if (live != mine && ++g.logged <= 20)
            Log("environment: VERIFY flush %llu: IVisArea::%s on %p differs: snapshot %d, 3D engine %d", g.flushes,
                S == 0 ? "IsConnectedToOutdoor" : "IsAffectedByOutLights", self, int(mine), int(live));
        return live;
    }
    return mine;
}

template <int I>
void* HookFor() {
    constexpr Kind k = kDefs[I].kind;
    if constexpr (k == Kind::Byte) return reinterpret_cast<void*>(&ByteHook<I>);
    else if constexpr (k == Kind::Int) return reinterpret_cast<void*>(&IntHook<I>);
    else if constexpr (k == Kind::Float) return reinterpret_cast<void*>(&FloatHook<I>);
    else if constexpr (k == Kind::Vec3) return reinterpret_cast<void*>(&VecHook<I, 3>);
    else if constexpr (k == Kind::Vec4) return reinterpret_cast<void*>(&VecHook<I, 4>);
    else if constexpr (k == Kind::Out2) return reinterpret_cast<void*>(&PairHook<I, 1>);
    else if constexpr (k == Kind::Vec4Pair) return reinterpret_cast<void*>(&PairHook<I, 4>);
    else if constexpr (k == Kind::Out4) return reinterpret_cast<void*>(&Out4Hook<I>);
    else if constexpr (k == Kind::Water) return reinterpret_cast<void*>(&WaterHook<I>);
    else if constexpr (k == Kind::Fog) return reinterpret_cast<void*>(&FogHook<I>);
    else if constexpr (k == Kind::Ptr) return reinterpret_cast<void*>(&PtrHook<I>);
    else if constexpr (k == Kind::OceanAt) return reinterpret_cast<void*>(&OceanHook<I>);
    else if constexpr (k == Kind::VisAreaAt) return reinterpret_cast<void*>(&VisAreaHook<I>);
    else if constexpr (k == Kind::DebugPush) return reinterpret_cast<void*>(&PushHook<I>);
    else return reinterpret_cast<void*>(&GlobalHook<I>);
}

template <size_t... I>
void HookTable(void** out, std::index_sequence<I...>) {
    ((out[I] = HookFor<int(I)>()), ...);
}

// --- the capture, right before the flush ----------------------------------------------------------------------------
// The positional water calls at the camera position, after the getters' capture: GetWave's branch and inputs, and on
// the FFT branch the height from the grid against the 3D engine's (the capture's GetOceanWaterLevel); then
// GetWaterLevel's water volumes alone, the same call with the vtable copy in and slot 28 answering a NaN (which its
// fold drops; whether slot 28 is called at all tells whether it takes the ocean's height there), and the higher of the
// two against its answer. A check failing leaves both getters' positional calls to the 3D engine for this flush.
void CaptureWaterAt() {
    g.waterAt = false;
    const Getter& w = g.get[kWater];
    const Getter& o = g.get[kOcean];
    if (!g.camPosOk || !g.oceanFound || !w.hooked || !o.hooked) return;
    g.oceanIn = ocean::Capture(g.ocean);
    ++g.oceanBranch[int(g.oceanIn.branch)];
    const float height = AsFloat(o.v[0]);
    if (g.oceanIn.branch == ocean::Branch::Fft) {
        const float mine = ocean::Height(g.oceanIn, g.camPos, g.ocean.floor);
        if (Bits(mine) != o.v[0]) {
            ++g.oceanBad;
            if (++g.loggedWater <= 5)
                Log("environment: flush %llu: the ocean's height at the camera position from its grid (%.9g) is not the "
                    "3D engine's (%.9g): the positional water calls go to the 3D engine in this flush",
                    g.flushes, double(mine), double(height));
            return;
        }
    }
    const uint64_t before = g.sentinelCalls;
    g.counter.Begin();
    g.sentinel = true;
    const float volumes = reinterpret_cast<float (*)(void*, const float*, bool)>(w.orig)(g.engine, g.camPos, false);
    g.sentinel = false;
    g.counter.End();
    g.waterOcean = g.sentinelCalls != before;
    g.waterVolumes = Bits(volumes);
    const float level = g.waterOcean ? Higher(height, volumes) : volumes;
    if (Bits(level) != w.v[1]) {
        ++g.waterBad;
        if (++g.loggedWater <= 5)
            Log("environment: flush %llu: the water level at the camera position from the ocean's height (%.9g, %s) and "
                "the water volumes' (%.9g) is %.9g, not the 3D engine's %.9g: the positional water calls go to the 3D "
                "engine in this flush",
                g.flushes, double(height), g.waterOcean ? "taken" : "not taken", double(volumes), double(level),
                double(AsFloat(w.v[1])));
        return;
    }
    if (g.waterOcean) ++g.waterOceanFlushes;
    g.waterAt = true;
}

void Capture() {
    const uint64_t t0 = __rdtsc();
    void* e = g.engine;
    for (int i = 0; i < kEnvGetters; ++i) {
        Getter& x = g.get[i];
        if (!x.hooked) continue;
        uint32_t* v = x.v;
        void* fn = x.orig;
        switch (kDefs[i].kind) {
        case Kind::Byte: v[0] = reinterpret_cast<uint8_t (*)(void*)>(fn)(e); break;
        case Kind::Int: v[0] = uint32_t(reinterpret_cast<int (*)(void*)>(fn)(e)); break;
        case Kind::Float: v[0] = Bits(reinterpret_cast<float (*)(void*)>(fn)(e)); break;
        case Kind::Vec3:
        case Kind::Vec4: reinterpret_cast<float* (*)(void*, float*)>(fn)(e, F(v)); break;
        case Kind::Out2: reinterpret_cast<void (*)(void*, float*, float*)>(fn)(e, F(v), F(v + 1)); break;
        case Kind::Vec4Pair: reinterpret_cast<void (*)(void*, float*, float*)>(fn)(e, F(v), F(v + 4)); break;
        case Kind::Out4:
            reinterpret_cast<void (*)(void*, float*, float*, float*, float*)>(fn)(e, F(v), F(v + 1), F(v + 2), F(v + 3));
            break;
        case Kind::Water: {
            const auto get = reinterpret_cast<float (*)(void*, const float*, bool)>(fn);
            v[0] = Bits(get(e, nullptr, false));
            if (g.camPosOk) v[1] = Bits(get(e, g.camPos, false));
            break;
        }
        case Kind::OceanAt:
            if (g.camPosOk) v[0] = Bits(reinterpret_cast<float (*)(void*, const float*)>(fn)(e, g.camPos));
            break;
        case Kind::VisAreaAt: {
            void* va = g.camPosOk ? reinterpret_cast<void* (*)(void*, const float*)>(fn)(e, g.camPos) : nullptr;
            std::memcpy(v, &va, sizeof va);
            g.visArea = va;
            if (va && g.visReady) {
                g.visOutdoor = reinterpret_cast<bool (*)(void*)>(g.visOrig[0])(va);
                g.visOutLights = reinterpret_cast<bool (*)(void*)>(g.visOrig[1])(va);
            }
            break;
        }
        case Kind::DebugPush: v[0] = PushDoesNothing() ? 1 : 0; break;
        case Kind::Fog:
            v[0] = NoFogVolumes() ? 1 : 0;
            if (!v[0]) TakeFogSnapshot();
            break;
        case Kind::Ptr: {
            void* p = reinterpret_cast<void* (*)(void*)>(fn)(e);
            std::memcpy(v, &p, sizeof p);
            break;
        }
        case Kind::Global: {
            // each parameter twice, into different fillings: a component is written where both calls agree
            const auto get = reinterpret_cast<void (*)(void*, int, float*)>(fn);
            for (int p = kParamFirst; p <= kParamLast; ++p) {
                uint32_t a[3] = {0x7FC1A5A5u, 0x7FC2A5A5u, 0x7FC3A5A5u}, b[3] = {0xFFC15A5Au, 0xFFC25A5Au, 0xFFC35A5Au};
                get(e, p, F(a));
                get(e, p, F(b));
                uint8_t m = 0;
                for (int k = 0; k < 3; ++k)
                    if (a[k] == b[k]) m |= uint8_t(1 << k);
                g.globalMask[p] = m;
                std::memcpy(g.global[p], a, sizeof a);
            }
            break;
        }
        }
    }
    CaptureWaterAt();
    g.captureCycles += __rdtsc() - t0;
    if (g.loggedFirst) return;
    g.loggedFirst = true;
    char line[3072] = "";
    size_t n = 0;
    for (int i = 0; i < kEnvGetters; ++i) {
        const Kind k = kDefs[i].kind;
        if (!g.get[i].hooked || k == Kind::Global) continue;
        Append(line, sizeof line, n, " %s", kDefs[i].name);
        if (k == Kind::Ptr) {
            Append(line, sizeof line, n, " (0x%08X%08X)", g.get[i].v[1], g.get[i].v[0]);
            continue;
        }
        Append(line, sizeof line, n, " (");
        for (int w = 0; w < Words(k); ++w) {
            const uint32_t u = g.get[i].v[w];
            if (k == Kind::Byte || k == Kind::Int || k == Kind::Fog || k == Kind::DebugPush)
                Append(line, sizeof line, n, "%s%d", w ? " " : "", int(u));
            else
                Append(line, sizeof line, n, "%s%g", w ? " " : "", double(AsFloat(u)));
        }
        Append(line, sizeof line, n, ")");
    }
    Log("environment: first capture:%s", line);
    n = 0;
    line[0] = 0;
    for (int p = kParamFirst; p <= kParamLast; ++p) {
        if (!g.globalMask[p]) continue;
        Append(line, sizeof line, n, " 0x%X (", p);
        for (int k = 0; k < 3; ++k)
            if (g.globalMask[p] & (1 << k)) Append(line, sizeof line, n, "%s%g", k ? " " : "", double(AsFloat(g.global[p][k])));
        Append(line, sizeof line, n, ")");
    }
    Log("environment: first capture, GetGlobalParameter:%s", line);
}

// The sun shafts' test (CryRenderD3D10 NPostEffects::CSunShafts, +0x1AD8E4): the vis area at the renderer's camera
// position (+0x28F44, which confirms that offset), then its IsConnectedToOutdoor and IsAffectedByOutLights, whose
// calls return at +0x35 and +0x42
constexpr const char kSunShafts[] =
    "48 8B 01 FF 90 E0 00 00 00 48 8B 15 ?? ?? ?? ?? 4C 8B 00 48 81 C2 44 8F 02 00 48 8B C8 41 FF 90 80 03 00 00 48 85 "
    "C0 48 8B D8 74 1A 48 8B 10 48 8B C8 FF 52 10 84 C0 75 0D 48 8B 13 48 8B CB FF 52 38 84 C0 74 08";
constexpr int kSunShaftsOutdoor = 0x35, kSunShaftsOutLights = 0x42;
// CVisArea (Cry3DEngine, RTTI): IsConnectedToOutdoor (slot 2: a portal's +0x80 == 1, else more), IsAffectedByOutLights
// (slot 7: +0x110)
constexpr const char kVisOutdoor[] =
    "40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 50 20 84 C0 74 10 83 BB 80 00 00 00 01 0F 94 C0 48 83 C4 20 5B C3";
constexpr const char kVisOutLights[] = "8A 81 10 01 00 00 C3";
constexpr int kVisSlotOutdoor = 2, kVisSlotOutLights = 7;

// what a rip-relative field points at
char* Target4(char* field) { return field + 4 + int32_t(ReadU32(field)); }

// a vtable entry as a mapped module's address
char* EntryOf(char* base, void* entry) {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint64_t e = reinterpret_cast<uint64_t>(entry), size = nt->OptionalHeader.SizeOfImage;
    if (e - reinterpret_cast<uint64_t>(base) < size) return static_cast<char*>(entry);
    if (e - nt->OptionalHeader.ImageBase < size) return base + (e - nt->OptionalHeader.ImageBase);
    return nullptr;
}

bool CodeIs(char* base, const char* at, const char* text) {
    const Pattern p = ParsePattern(text);
    return at && IsInCode(base, at, p.size()) && MatchesAt(at, p);
}

bool PatchSlot(void** slot, void* fn, void** old) {
    DWORD protect;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
    *old = *slot;
    InterlockedExchangePointer(slot, fn);
    VirtualProtect(slot, sizeof(void*), protect, &protect);
    return true;
}

// the sun shafts' code (which also confirms the camera position's offset) and CVisArea's two slots patched
void SetupVisArea(char* e3d) {
    HMODULE renderer = GetModuleHandleW(L"CryRenderD3D10.dll");
    char* shafts = renderer ? EnvSnapshot_LocateSunShafts(reinterpret_cast<char*>(renderer)) : nullptr;
    if (!shafts) {
        Log("environment snapshot: the positional calls are left to the 3D engine: the sun shafts' code is not the "
            "expected one");
        return;
    }
    g.camOffsetOk = true;  // (the positional getters can be answered at the camera position)
    void** vt = EnvSnapshot_LocateVisArea(e3d);
    if (!vt) {
        Log("environment snapshot: IVisArea's queries left to the 3D engine: CVisArea's code is not the expected one");
        return;
    }
    g.visRet[0] = shafts + kSunShaftsOutdoor;
    g.visRet[1] = shafts + kSunShaftsOutLights;
    if (!PatchSlot(&vt[kVisSlotOutdoor], reinterpret_cast<void*>(&VisFlagHook<0>), &g.visOrig[0])) return;
    if (!PatchSlot(&vt[kVisSlotOutLights], reinterpret_cast<void*>(&VisFlagHook<1>), &g.visOrig[1])) {
        void* ignored;
        PatchSlot(&vt[kVisSlotOutdoor], g.visOrig[0], &ignored);
        return;
    }
    g.visReady = true;
}

// At the first flush: the 3D engine, its vtable's getters checked, the vtable copy set up.
void Setup() {
    g.setupTried = true;
    void* system = *g.system;
    void* engine = system ? reinterpret_cast<void* (*)(void*)>((*static_cast<void***>(system))[kVfGetI3DEngine / 8])(system)
                          : nullptr;
    HMODULE e3d = GetModuleHandleW(L"Cry3DEngine.dll");
    if (!engine || !e3d) {
        Log("environment snapshot: off: no I3DEngine");
        return;
    }
    char* base = reinterpret_cast<char*>(e3d);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    void** vt = *static_cast<void***>(engine);
    int slots = 0;
    while (slots < kMaxSlots && IsInCode(base, static_cast<const char*>(CallTrace_Original(vt[slots])), 1)) ++slots;
    EnvGetter found[kEnvGetters];
    const int n = EnvSnapshot_Locate(base, vt, slots, found);
    if (n <= 0) {
        Log("environment snapshot: off: %s", n < 0 ? "the 3D engine's vtable is shorter than expected"
                                                   : "none of the getters is as expected");
        return;
    }
    if (!g.counter.Init(engine, base, nt->OptionalHeader.SizeOfImage, slots) || g.counter.Slots() != slots) {
        Log("environment snapshot: off: could not copy the 3D engine's vtable");
        return;
    }
    void* hooks[kEnvGetters];
    HookTable(hooks, std::make_index_sequence<kEnvGetters>{});
    char left[1024] = "";
    size_t len = 0;
    int hooked = 0;
    for (int i = 0; i < kEnvGetters; ++i) {
        if (!found[i].fn || (kDefs[i].kind == Kind::Fog &&
                             !fog::Locate(base, RelTarget(found[i].fn + kDefs[i].jumpAt), &g.fog, &g.fogWhy))) {
            Append(left, sizeof left, len, " %s", kDefs[i].name);
            if (kDefs[i].kind == Kind::Fog && g.fogWhy) Log("environment snapshot: TraceFogVolumes: %s", g.fogWhy);
            continue;
        }
        Getter& x = g.get[i];
        x.orig = g.counter.Original(kDefs[i].slot);
        x.hooked = true;
        g.counter.SetTarget(kDefs[i].slot, hooks[i]);
        ++hooked;
    }
    // DebugDraw_PushFrustrum's cvars pointer, from its code; without it, the slot goes back to the 3D engine
    for (int i = 0; i < kEnvGetters; ++i) {
        if (kDefs[i].kind != Kind::DebugPush || !g.get[i].hooked) continue;
        char* cvars = Target4(found[i].fn + kPushCvars);
        if (IsInImage(base, cvars, 8)) {
            g.pushCvars = reinterpret_cast<char* const*>(cvars);
        } else {
            g.get[i].hooked = false;
            g.counter.SetTarget(kDefs[i].slot, g.get[i].orig);
            --hooked;
        }
    }
    // the ocean's code, from GetOceanWaterLevel's (ocean_snapshot.h); without it the positional water calls go to the
    // 3D engine
    if (g.get[kOcean].hooked) {
        const char* why = "";
        g.oceanFound = ocean::Locate(base, found[kOcean].fn, &g.ocean, &why);
        if (!g.oceanFound) Log("environment snapshot: the positional water calls are left to the 3D engine: %s", why);
    }
    SetupVisArea(base);
    for (int s : kStaleSlots) g.counter.Mark(s);
    g.sceneRuns = ReflectionPrepass_SceneRuns();
    if (!g.sceneRuns) g.counter.Mark(kSlotRenderWorld);
    g.engine = engine;
    g.mainThread = GetCurrentThreadId();
    g.ready = true;
    Log("environment snapshot: mode=%s: %d of %d getters answered from it (left to the 3D engine:%s); the 3D engine's "
        "vtable has %d slots (Cry3DEngine.dll+0x%X); scene traversals in the flush: %s",
        ModeName(g.mode), hooked, kEnvGetters, hooked == kEnvGetters ? " none" : left, slots,
        unsigned(reinterpret_cast<char*>(vt) - base),
        g.sceneRuns ? "counted by the reflection part" : "any RenderWorld call ends the snapshot (no reflection part)");
}
#endif  // _WIN64

}  // namespace

int EnvSnapshot_Locate(char* base, void* const* vtable, int slots, EnvGetter* out) {
#ifndef _WIN64
    (void)base;
    (void)vtable;
    (void)slots;
    (void)out;
    return -1;
#else
    int need = kSlotRenderWorld;
    for (int s : kStaleSlots) need = (std::max)(need, s);
    for (const Def& d : kDefs) need = (std::max)(need, d.slot);
    if (slots <= need) return -1;
    int n = 0;
    for (int i = 0; i < kEnvGetters; ++i) {
        const Def& d = kDefs[i];
        out[i] = EnvGetter{d.slot, d.name, nullptr};
        char* fn = static_cast<char*>(CallTrace_Original(vtable[d.slot]));
        const Pattern code = ParsePattern(d.code);
        if (!IsInCode(base, fn, code.size()) || !MatchesAt(fn, code)) continue;
        char* body = fn;
        if (d.jumpTo) {
            body = RelTarget(fn + d.jumpAt);
            const Pattern target = ParsePattern(d.jumpTo);
            if (!IsInCode(base, body, target.size()) || !MatchesAt(body, target)) continue;
        }
        fog::Target t;
        if (d.kind == Kind::Fog && !fog::Locate(base, body, &t, &g.fogWhy)) continue;  // the code TraceFogVolumes runs
        out[i].fn = fn;
        ++n;
    }
    return n;
#endif
}

char* EnvSnapshot_LocateSunShafts(char* rendererBase) {
#ifndef _WIN64
    (void)rendererBase;
    return nullptr;
#else
    const auto hits = FindInCode(rendererBase, ParsePattern(kSunShafts), 2);
    return hits.size() == 1 ? hits[0] : nullptr;
#endif
}

void** EnvSnapshot_LocateVisArea(char* e3dBase) {
#ifndef _WIN64
    (void)e3dBase;
    return nullptr;
#else
    void** vt = FindRttiVtable(e3dBase, ".?AUCVisArea@@");  // (declared a struct)
    if (!vt || !CodeIs(e3dBase, EntryOf(e3dBase, CallTrace_Original(vt[kVisSlotOutdoor])), kVisOutdoor) ||
        !CodeIs(e3dBase, EntryOf(e3dBase, CallTrace_Original(vt[kVisSlotOutLights])), kVisOutLights))
        return nullptr;
    return vt;
#endif
}

void EnvSnapshot_Install(const wchar_t* moduleName, void** system) {
#ifndef _WIN64
    (void)system;
    if (ReadMode() != EMode::Off) Log("%ls: environment snapshot not applied: 64-bit game only", moduleName);
#else
    if (g.installed) return;
    g.mode = ReadMode();
    if (g.mode == EMode::Off) return;
    if (!system) {
        Log("%ls: environment snapshot not applied: no ISystem", moduleName);
        return;
    }
    g.system = system;
    g.verify = g.mode == EMode::Verify;
    g.installed = true;
    Log("%ls: environment snapshot: mode=%s (set up at the first flush)", moduleName, ModeName(g.mode));
#endif
}

void EnvSnapshot_BeforeFlush(char* renderer) {
#ifdef _WIN64
    if (!g.installed) return;
    if (!g.setupTried) Setup();
    if (!g.ready || g.swapped || GetCurrentThreadId() != g.mainThread) return;
    ++g.flushes;
    g.camPosOk = g.camOffsetOk && renderer;
    if (g.camPosOk) std::memcpy(g.camPos, renderer + kRCamPos, sizeof g.camPos);
    Capture();
    for (int k = 0; k < kStaleCount; ++k) g.setterCalls[k] = SetterCalls(k);
    g.worldsAtCapture = WorldCalls();
    g.scenesAtCapture = g.sceneRuns ? *g.sceneRuns : 0;
    g.counter.ClearMarker();
    g.counter.SetMarkThread(FlushThread());  // (the flush's own setter calls make it out of date, not another thread's)
    g.live = g.swapped = true;
    g.counter.Begin();
#else
    (void)renderer;
#endif
}

void EnvSnapshot_AfterFlush() {
#ifdef _WIN64
    if (!g.swapped) return;
    g.counter.End();
    g.live = g.swapped = false;
#endif
}

void EnvSnapshot_Report(const char* when) {
#ifndef _WIN64
    (void)when;
#else
    if (!g.installed || !g.ready) return;
    uint64_t answered = 0, passed = 0, aside = 0, same = 0, differ = 0;
    char line[3072] = "";
    size_t n = 0;
    for (int i = 0; i < kEnvGetters; ++i) {
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
    Log("environment [%s]: mode=%s | flushes=%llu, capture %.4f ms each | out of date in %llu (after a setter %llu, a scene "
        "traversal %llu) | calls %s=%llu; to the 3D engine: out of date %llu, with a position, another parameter or fog "
        "volumes not taken %llu, from other threads %llu",
        when, ModeName(g.mode), g.flushes, double(g.captureCycles) / 3.0e6 / double(g.flushes ? g.flushes : 1), g.stale,
        g.staleSetter, g.staleScene, g.verify ? "compared" : "answered", asked, passed, aside,
        g.otherThread.load(std::memory_order_relaxed));
    Log("environment [%s]: per getter (%s):%s", when,
        g.verify ? "same/differ/to the 3D engine" : "answered/to the 3D engine", line);
    if (g.verify)
        Log("environment [%s]: VERIFY calls=%llu: same=%llu differ=%llu", when, same + differ, same, differ);
    n = 0;
    line[0] = 0;
    for (int i = 0; i < kEnvGetters; ++i)
        if (g.get[i].nested) Append(line, sizeof line, n, " %s %llu", kDefs[i].name, g.get[i].nested);
    Log("environment [%s]: %s inside replayed reflections' flushes (where vanilla has the reflection's camera in the 3D "
        "engine):%s",
        when, g.verify ? "compared" : "answered", n ? line : " none");
    Log("environment [%s]: fog volumes: a snapshot in %llu flushes (%.1f fog volumes each, at most %zu; %llu lists not "
        "taken); TraceFogVolumes traced through them %llu times (%llu from a replayed reflection's camera)",
        when, g.fogFrames, double(g.fogVolumes) / double(g.fogFrames ? g.fogFrames : 1), g.fogMax, g.fogBad, g.fogTraced,
        g.fogReplayCam);
    n = 0;
    line[0] = 0;
    for (int p = kParamFirst; p <= kParamLast; ++p)
        if (g.globalCalls[p]) Append(line, sizeof line, n, " 0x%X:%llu", p, g.globalCalls[p]);
    Log("environment [%s]: GetGlobalParameter calls per parameter:%s", when, n ? line : " none");
    Log("environment [%s]: positional calls answered at the renderer's camera position: %s | IVisArea (the sun shafts, "
        "on the vis area there): %s IsConnectedToOutdoor=%llu IsAffectedByOutLights=%llu%s",
        when, g.camOffsetOk ? "yes" : "no (the sun shafts' code not found)", g.verify ? "compared" : "answered",
        g.visAnswered[0], g.visAnswered[1], g.visReady ? "" : " (CVisArea not patched)");
    if (g.verify && g.visReady)
        Log("environment [%s]: VERIFY IVisArea calls=%llu: same=%llu differ=%llu", when, g.visSame + g.visDiffer,
            g.visSame, g.visDiffer);
    Log("environment [%s]: the ocean at the camera position: its waves' branch in flushes: FFT %llu, analytic %llu, none "
        "%llu (heights from the grid when asked for: %llu); GetWaterLevel there took the ocean's height in %llu; the "
        "capture's checks failed: the ocean's height %llu, the water level %llu%s",
        when, g.oceanBranch[int(ocean::Branch::Fft)], g.oceanBranch[int(ocean::Branch::Analytic)],
        g.oceanBranch[int(ocean::Branch::None)], g.oceanHeights, g.waterOceanFlushes, g.oceanBad, g.waterBad,
        g.oceanFound ? "" : " (the ocean's code not found: left to the 3D engine)");
    // every slot called through the copy (in place during the flushes, and for the capture's water volumes, whose
    // calls are left out): from other modules / from the 3D engine
    n = 0;
    line[0] = 0;
    for (int i = 0; i < g.counter.Slots(); ++i) {
        const uint64_t out = g.counter.Outside(i);
        const uint64_t in = g.counter.Inside(i) - (i == kDefs[kOcean].slot ? g.sentinelCalls : 0);
        if (out || in) Append(line, sizeof line, n, " %d %s:%llu/%llu", i, SlotLabel(i), out, in);
    }
    Log("environment [%s]: 3D-engine vtable calls inside the flushes (slot name:from other modules/from the 3D engine):%s",
        when, n ? line : " none");
#endif
}

}  // namespace optiperf
