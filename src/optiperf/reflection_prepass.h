#pragma once
// Reflections and the flush's preprocess stage (Phase 2, after the shadow pre-pass). x64 DX10 renderer only; driven by
// the shadow pre-pass's EF_EndEf3D hook (shadow_prepass.cpp). See reflection_prepass.cpp.
//
// [ShadowPrepass] Reflections = off (default) | observe | on | verify     (env OPTIPERF_SHADOW_PREPASS_REFLECTIONS)
//   needs Mode other than off (the hooks are the shadow pre-pass's)
// [ShadowPrepass] FarTrees    = off (default) | on | verify               (env OPTIPERF_SHADOW_PREPASS_FARTREES)
//                 DynTextures = off (default) | on | verify               (env OPTIPERF_SHADOW_PREPASS_DYNTEXTURES)
//   need Reflections=on|verify (both run in its dry run)

#include <cstddef>
#include <cstdint>

namespace optiperf {

struct ShadowPrepassTarget;

// What the reflection part uses in a loaded CryRenderD3D10.dll (x64), as read from the code itself.
struct ReflectionTarget {
    char* preprocess = nullptr;      // the preprocess stage's handler (renderer, items, start, end): "Start preprocess frame"
    char* drawToRT = nullptr;        // FX_DrawToRenderTarget: updates a render target, a reflection through RenderWorld
    char* panorama = nullptr;        // CREPanoramaCluster::UpdateImposter (element): also renders the scene
    char* sortPreprocess = nullptr;  // the preprocess list's sort (items, count): std::sort with the game's comparator
    char* renderCallback = nullptr;  // the flush's render callback, which the preprocess handler requires at +0xB28
    char* renderWorldReturn = nullptr;  // return address of FX_DrawToRenderTarget's I3DEngine::RenderWorld call
    char* oceanCamera = nullptr;     // the cached camera it passes for the ocean's reflection (a static CCamera)
    void** system = nullptr;         // ISystem*, for I3DEngine (GetI3DEngine, RenderWorld, GenerateFarTrees)

    // Reflections=on|verify (rendering them early). Null if that code didn't match (fullWhy says why): only observe then.
    char* startBatch = nullptr;      // the per-list setup (renderer, list) and teardown (renderer) around the handler
    char* endBatch = nullptr;
    // what a dry run of FX_DrawToRenderTarget changes and has to be put back: a water reflection's update decision and
    // counter (the update state itself is at fixed offsets, +0x1810..0x1857); the cvars it sets around RenderWorld
    uint8_t* updateFlag = nullptr;
    int* updateCounter = nullptr;
    int* noDrawNear = nullptr;       // r_NoDrawNear
    int* cvar980c = nullptr;         // a cvar it zeroes for reflections below the water
    // the renderer setters a reflection's traversal calls for its flush (checked against the vtable at the first flush)
    char* enableFog = nullptr;
    char* setFog = nullptr;
    char* setSkyLight = nullptr;     // SetSkyLightRenderParams
    // what EF_EndEf3D does before its list processing, which the dry runs come before: the frame time from the timer;
    // with flag 0x400, two cvars set to 0 and 1 for the flush; the level's polygons turned into render items
    void** timer = nullptr;          // ITimer*
    int* noAsync[2] = {};
    char* polyItems = nullptr;
    const char* fullWhy = nullptr;   // why only observe works; null if the pieces above were found

    // FarTrees=on|verify: the far-tree sprites' quads, which the renderer's GenerateObjSprites builds (0x38 bytes each)
    // and DrawObjSprites draws; verify compares them. Null if not found (farWhy says why): far trees stay in the flush.
    char** spriteQuads = nullptr;
    int* spriteCount = nullptr;
    const char* farWhy = nullptr;
};

// Finds and validates the code in a mapped CryRenderD3D10.dll; t: what the shadow pre-pass found there. On failure
// returns false and sets *why.
bool ReflectionPrepass_Locate(char* moduleBase, const ShadowPrepassTarget& t, ReflectionTarget* out, const char** why);

// From shadow_prepass.cpp: once the shadow pre-pass is installed (reads the setting, locates, hooks); at each outer
// EF_EndEf3D, before the shadow pre-pass, right before the original runs, and after it; for the nested EF_EndEf3D of
// a reflection rendered early (true: taken, don't run it); for its reports.
void ReflectionPrepass_Install(const wchar_t* moduleName, char* base, const ShadowPrepassTarget& t);
void ReflectionPrepass_OnFlushEntry(char* renderer, int flags);
void ReflectionPrepass_BeforeFlush(char* renderer);
void ReflectionPrepass_OnFlushEnd(char* renderer);
bool ReflectionPrepass_OnNestedEndEf3D(char* renderer, int flags);
void ReflectionPrepass_Report(const char* when);

// For the environment snapshot (env_snapshot.cpp): how many times the 3D engine's scene traversal
// (I3DEngine::RenderWorld) has run inside the outer flush on the main thread so far (reflections left there, or not
// replayed after all; scenes rendered elsewhere in the flush). Null while this part doesn't hook RenderWorld (off, not
// located, or before its first flush).
const uint64_t* ReflectionPrepass_SceneRuns();

// Whether the reflections' scenes are traversed before the flush (Reflections=on|verify, from its first flush on); if
// not, their traversals run inside the flush, and nothing may run beside it (the render queue checks).
bool ReflectionPrepass_Early();
// For the render queue, at a hand-off (after the flush's entry): whether the flush may run a scene traversal beside
// the main thread: a capture of the dry run's may fall back to one, or what it left to the flush may hold one; without
// the dry run, any. In the flush it tells the render queue when the last capture is replayed with nothing left to the
// flush (RenderQueue_OnScenesFree), and at each traversal it runs (RenderQueue_OnSceneRun).
bool ReflectionPrepass_MayRunScenes();
// For the environment snapshot: while the flush replays a reflection's nested EF_EndEf3D (on its thread), the
// camera its RenderWorld call passed (a CCamera's first 0x214 bytes: its matrix, translation at +0xC, +0x1C, +0x2C);
// else null. In vanilla the traversal sets the 3D engine's camera to it before that flush and puts the previous one
// back after it; the replay doesn't run the traversal, so the 3D engine's camera stays the frame's.
const uint8_t* ReflectionPrepass_ReplayCamera();

// For the particle part (particle_prepass.cpp): whether the nested EF_EndEf3D now coming (at this recursion level) is a
// reflection's that the dry run takes into a capture (its items are drawn later, when the flush replays it).
bool ReflectionPrepass_WillTake(int level);

namespace sp {
struct Stash;
}
// For the skinning part (skin_prepass.cpp), right before the flush: each reflection rendered early whose capture the
// flush is to replay, with the items its traversal left (drawn at the reflection's recursion level then).
void ReflectionPrepass_ForEachCapture(void (*fn)(const sp::Stash& items, void* ctx), void* ctx);
// For the light part (light_snapshot.cpp), right before the flush: the same captures' lights, the CDLight pointers
// each puts back into its level's list when the flush replays it. They may be changed (used up by this flush).
void ReflectionPrepass_ForEachCapturedLights(void (*fn)(void** lights, size_t count, void* ctx), void* ctx);
// For the engine-data part (engine_data.cpp), right before the flush: the far-tree sprites' quads (the renderer's list,
// 0x38 bytes each), when they were made before the flush this frame and the flush won't make them again (FarTrees=on);
// else false. A quad's +8 is the terrain texture set DrawObjSprites' outdoor ambient occlusion reads.
bool ReflectionPrepass_FarQuads(char** quads, int* count);
// For the engine-data part, right before the flush: the sky light parameters that the pending captures' recorded
// SetSkyLightRenderParams calls pass when the flush replays them; they may be changed (used up by this flush).
void ReflectionPrepass_ForEachRecordedSkyLight(void (*fn)(const void** params, void* ctx), void* ctx);

}  // namespace optiperf
