#pragma once
// Material snapshot (Phase 2, after TraceFogVolumes): the material getters the flush's layer passes call (CryRenderD3D10's
// Renderer:SetupMultiLayers and Renderer:DrawShader_MultiLayers: frozen, wet, cloaked objects), answered during the
// flush from copies of the materials taken right before it. x64 DX10 renderer only; driven by the shadow pre-pass's
// EF_EndEf3D hook (shadow_prepass.cpp). See material_snapshot.cpp and research/docs/material-snapshot.md.
//
// [ShadowPrepass] Materials = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_MATERIALS)
//   needs Mode other than off (the hook is the shadow pre-pass's)

namespace optiperf {

struct ShadowPrepassTarget;

// The getters answered: IMaterial's (CMatInfo), IMaterialLayer's (CMaterialLayer) and IMaterialManager's (CMatMan)
enum MtlSlot {
    kMtlFlags,          // CMatInfo 8: GetFlags
    kMtlItem,           // CMatInfo 16: GetShaderItem()
    kMtlSubCount,       // CMatInfo 20: GetSubMtlCount
    kMtlSub,            // CMatInfo 21: GetSubMtl(i)
    kMtlLayerCount,     // CMatInfo 24: GetLayerCount
    kMtlLayerAt,        // CMatInfo 26: GetLayer(slot)
    kMtlLayerMask,      // CMatInfo 27: GetLayer(mask, usage)
    kMtlEnabled,        // CMaterialLayer 3: IsEnabled
    kMtlLayerItem,      // CMaterialLayer 5: GetShaderItem
    kMtlDefaultLayers,  // CMatMan 9: GetDefaultLayersMaterial
    kMtlSlots
};
constexpr int kMtlSites = 2;  // the calls each getter answers, by return address (at most two per getter)

// What the part uses in a loaded CryRenderD3D10.dll (x64): the two functions' calls of those getters.
struct MaterialTarget {
    char* setupLayers = nullptr;     // Renderer:SetupMultiLayers(renderer, set)
    char* drawLayers = nullptr;      // Renderer:DrawShader_MultiLayers(renderer)
    char* sites[kMtlSlots][kMtlSites] = {};  // the return addresses of their calls, per getter (null: none)
    void* const* defaultLayers = nullptr;    // SetupMultiLayers' static: the default layers material (its first call)
};
// ... and in Cry3DEngine.dll: the classes' vtables (by RTTI), the getters' code, and the material manager.
struct MaterialEngine {
    void** vtables[3] = {};          // CMatInfo, CMaterialLayer, CMatMan
    char* fns[kMtlSlots] = {};
    void* const* matMan = nullptr;   // the material manager (the static C3DEngine::GetMaterialManager returns)
};

// Find and check the code in a mapped CryRenderD3D10.dll / Cry3DEngine.dll; on failure return false and set *why.
bool MaterialSnapshot_LocateRenderer(char* base, MaterialTarget* out, const char** why);
bool MaterialSnapshot_LocateEngine(char* e3dBase, MaterialEngine* out, const char** why);

// From shadow_prepass.cpp: once the shadow pre-pass is installed (reads the setting, locates the renderer's code; the
// 3D engine's is found at the first flush); right before the outer EF_EndEf3D runs and right after it; its reports.
void MaterialSnapshot_Install(const wchar_t* moduleName, char* base, const ShadowPrepassTarget& sp);
void MaterialSnapshot_BeforeFlush(char* renderer);
void MaterialSnapshot_AfterFlush();
void MaterialSnapshot_Report(const char* when);

}  // namespace optiperf
