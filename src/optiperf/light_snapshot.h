#pragma once
// Light snapshot (the render thread's groundwork, item 1): the 3D engine's dynamic lights (CDLight) the flush reads,
// copied right before it. While it runs, the renderer's pointers to them (its recursion levels' light lists, the
// reflections' captured lists, the coronas' render objects) point at the copies. x64 DX10 renderer only; driven by the
// shadow pre-pass's EF_EndEf3D hook (shadow_prepass.cpp). See light_snapshot.cpp, light_record.h and
// research/docs/render-thread.md.
//
// [ShadowPrepass] Lights = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_LIGHTS)
//   needs Mode other than off (the hook is the shadow pre-pass's)

#include <cstddef>

namespace optiperf {

struct ShadowPrepassTarget;

// What the part uses in a loaded CryRenderD3D10.dll (x64), checked against the code.
struct LightTarget {
    char* addLight = nullptr;   // CD3D9Renderer::EF_ADDDlight (IRenderer slot 117): fills the lists, makes the coronas'
                                // objects (m_pObject at +0x118, their m_pLight at +0x128)
    char* lightCtor = nullptr;  // the renderer's CDLight constructor: the light's size (600 bytes) and layout
};

// Finds and checks them in a mapped CryRenderD3D10.dll; on failure returns false and sets *why.
bool LightSnapshot_Locate(char* rendererBase, LightTarget* out, const char** why);

// From shadow_prepass.cpp: once the shadow pre-pass is installed (finds the code, hooks EF_ADDDlight; recording starts
// after the first flush); right before the outer EF_EndEf3D runs (after the other parts' captures, before the
// census) and right after it; for its reports.
void LightSnapshot_Install(const wchar_t* moduleName, char* rendererBase, size_t rendererSize,
                           const ShadowPrepassTarget& sp);
void LightSnapshot_BeforeFlush(char* renderer);
void LightSnapshot_AfterFlush();
void LightSnapshot_Report(const char* when);

}  // namespace optiperf
