#pragma once
// Environment snapshot (Phase 2, after the reflections): the I3DEngine getters the renderer calls while drawing (sun,
// sky, fog, water, view distance, 3D-engine parameters), answered during the flush from what they returned right
// before it. x64 DX10 renderer only; driven by the shadow pre-pass's EF_EndEf3D hook (shadow_prepass.cpp). See
// env_snapshot.cpp and research/docs/environment-snapshot.md.
//
// [ShadowPrepass] Environment = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_ENVIRONMENT)
//   needs Mode other than off (the hook is the shadow pre-pass's); meant for Reflections=on|verify (else the
//   reflections' scene traversals run inside the flush, and the snapshot is out of date after the first one)

namespace optiperf {

// One getter the snapshot answers, as found in Cry3DEngine.dll: its I3DEngine vtable slot, name, and function (null if
// its code is not the expected one: that getter is then left to the 3D engine).
struct EnvGetter {
    int slot = 0;
    const char* name = nullptr;
    char* fn = nullptr;
};
constexpr int kEnvGetters = 26;

// Checks the getters in the C3DEngine vtable `vtable` (its first `slots` entries point into the module's code): fills
// out[kEnvGetters] and returns how many are as expected, or -1 if the vtable is too short (the snapshot also needs the
// setters' slots, which make it out of date when called).
int EnvSnapshot_Locate(char* e3dBase, void* const* vtable, int slots, EnvGetter* out);

// The sun shafts' test in CryRenderD3D10.dll (null if its code is not the expected one): the vis area at the renderer's
// camera position, where the positional calls are answered, then that vis area's IsConnectedToOutdoor and
// IsAffectedByOutLights; and CVisArea's vtable in Cry3DEngine.dll (by RTTI; null unless those two slots are the
// expected code).
char* EnvSnapshot_LocateSunShafts(char* rendererBase);
void** EnvSnapshot_LocateVisArea(char* e3dBase);

// From shadow_prepass.cpp: once the shadow pre-pass is installed (reads the setting; the rest is set up at the first
// flush, when the 3D engine exists); right before the outer EF_EndEf3D runs (after the shadow pre-pass and the
// reflections' dry run) and right after it; for its reports.
void EnvSnapshot_Install(const wchar_t* moduleName, void** system);
void EnvSnapshot_BeforeFlush(char* renderer);
void EnvSnapshot_AfterFlush();
void EnvSnapshot_Report(const char* when);

}  // namespace optiperf
