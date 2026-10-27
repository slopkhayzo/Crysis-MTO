#pragma once
// Shadow pre-pass (Phase 2, step 1): renders the shadow casters before the renderer's flush instead of inside it.
// x64 DX10 renderer only. See shadow_prepass.cpp and research/docs/shadow-prepass.md.
//
// [ShadowPrepass] Mode = off (default) | on | verify | compare     (env OPTIPERF_SHADOW_PREPASS)
//                 At   = flush (default) | shadows          (env OPTIPERF_SHADOW_PREPASS_AT)

#include <cstddef>
#include <cstdint>

namespace optiperf {

// What the pre-pass uses in a loaded CryRenderD3D10.dll (x64), as read from the code itself.
struct ShadowPrepassTarget {
    char* endEf3D = nullptr;          // CD3D9Renderer::EF_EndEf3D (IRenderer slot 115)
    char* prepareAll = nullptr;       // the forward shadow pass: for each shadow-casting light and frustum, PrepareDepthMap
    char* prepareDepthMap = nullptr;  // PrepareDepthMap(frustum, light): CD3D9Renderer vtable +0x678
    char* realloc = nullptr;          // the module's CryModuleRealloc, used when a list grows
    int* level = nullptr;             // SRendItem::m_RecurseLevel
    char* lists = nullptr;            // SRendItem::m_RendItems[2][15]: TArray<SRendItem>
    uint32_t* starts = nullptr;       // per recursion level: each list's count when the level began (30 x uint32)
    char* polyLists = nullptr;        // per recursion level: TArray of EF_AddPolygonToScene polygons (diagnostics)
    void** system = nullptr;          // the module's ISystem* global
    int* useShadowsPool = nullptr;    // r_UseShadowsPool
    int* shadowGenGS = nullptr;       // r_ShadowGenGS
    int* forwardPass = nullptr;       // r_ShadowsForwardPass
    int* shadowGen = nullptr;         // r_ShadowGen
    int* noDrawShaders = nullptr;     // r_NoDrawShaders
    int* hdrRendering = nullptr;      // r_HDRRendering, r_MeasureOverdraw, r_HDRAllowNonFP: whether the flush renders
    int* measureOverdraw = nullptr;   // an HDR scene (m_PersFlags2 0x200), which At=flush has to predict
    int* hdrAllowNonFP = nullptr;
    int* shadowTexFormat = nullptr;   // r_ShadowTexFormat: picks the map's format, which decides whether it is re-created
    // Omni-directional (point light) maps: each side culls the casters against its own camera, built by PrepareDepthMap
    // with the game's CCamera functions. Null if that code didn't match (omniWhy says why): such maps then keep the
    // original path.
    char* camInit = nullptr;          // CCamera::CCamera
    char* camUpdate = nullptr;        // CCamera::UpdateFrustum
    char* camSetFrustum = nullptr;    // CCamera::SetFrustum(w, h, fov, near, far)
    char* cullBox = nullptr;          // box against a camera's planes (the caster loop's test)
    char* sinf = nullptr;             // the CRT functions the side rotation uses (MSVCR80), through the module's thunks
    char* cosf = nullptr;
    int* shadowsDeferred = nullptr;   // r_ShadowsDeferredMode: a point light's six sides share one 3x2 texture
    float* omniFov = nullptr;         // field of view (degrees) of the sides in deferred mode
    const char* omniWhy = nullptr;    // why point-light maps keep the original path; null if the code above was found
};

// Finds and validates the code in a mapped CryRenderD3D10.dll. On failure returns false and sets *why.
bool ShadowPrepass_Locate(char* moduleBase, ShadowPrepassTarget* out, const char** why);

void ShadowPrepass_OnModuleLoaded(const wchar_t* baseName, char* base, size_t size);
void ShadowPrepass_Report(const char* when);
// The parts' step after the outer flush in progress (its name), or null: for a stall's log.
const char* ShadowPrepass_AfterStep();

namespace sp {
struct Stash;
}
// For the skinning part (skin_prepass.cpp), right before the flush: each shadow-map pass the pre-pass rendered early,
// with the items its casters added (put back where the flush renders that map).
void ShadowPrepass_ForEachStash(void (*fn)(const sp::Stash& items, void* ctx), void* ctx);
// For the light part (light_snapshot.cpp), right before the flush: each such stash's frustum and caster list, by which
// PrepareDepthMap finds it in the flush, to be replaced by their copies (the stashes are used up by this flush).
void ShadowPrepass_MapStashes(void (*fn)(char** frustum, void** casterList, void* ctx), void* ctx);

}  // namespace optiperf
