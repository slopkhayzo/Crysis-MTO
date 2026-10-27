#pragma once
// Cvar snapshot (Phase 2, after the material snapshot): the console variables the flush reads through ICVar (the
// renderer's cached pointers to other modules' cvars), answered during the flush from a capture made right before it.
// x64 DX10 renderer only; driven by the shadow pre-pass's EF_EndEf3D hook (shadow_prepass.cpp). See cvar_snapshot.cpp
// and research/docs/cvar-snapshot.md.
//
// [ShadowPrepass] Cvars = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_CVARS)
//   needs Mode other than off (the hook is the shadow pre-pass's)

#include <cstddef>

namespace optiperf {

// What the part uses in a loaded CrySystem.dll (x64): the two cvar classes the flush reads, by RTTI, and their getters
// and setters (ICVar slots 1 GetIVal, 2 GetFVal, 3 GetString; 4 Set(int), 5 Set(float), 6 Set(const char*), 7
// ForceSet).
enum CvClass { kCvIntRef, kCvString, kCvClasses };
constexpr int kCvGetters = 3, kCvSetters = 4;
struct CvarTarget {
    void** vtables[kCvClasses] = {};
    char* getters[kCvClasses][kCvGetters] = {};
};

// Finds and checks them in a mapped CrySystem.dll; on failure returns false and sets *why.
bool CvarSnapshot_Locate(char* sysBase, CvarTarget* out, const char** why);

// From shadow_prepass.cpp: once the shadow pre-pass is installed (reads the setting; the rest is set up at the first
// flush); right before the outer EF_EndEf3D runs and right after it; for its reports.
void CvarSnapshot_Install(const wchar_t* moduleName, char* rendererBase, size_t rendererSize);
void CvarSnapshot_BeforeFlush();
void CvarSnapshot_AfterFlush();
void CvarSnapshot_Report(const char* when);

}  // namespace optiperf
