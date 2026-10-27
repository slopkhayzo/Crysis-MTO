#pragma once
// System snapshot (Phase 2, after the skinning poses): the ISystem and ITimer getters the renderer calls while drawing
// (the 3D engine and timer pointers, the editor flags, the view camera; the frame's time, its length and the time
// scale), answered during the flush from what they returned right before it; the real-time clock from the timer's
// base and frequency then. x64 DX10 renderer only; driven by the
// shadow pre-pass's EF_EndEf3D hook (shadow_prepass.cpp). See sys_snapshot.cpp and research/docs/system-snapshot.md.
//
// [ShadowPrepass] System = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_SYSTEM)
//   needs Mode other than off (the hook is the shadow pre-pass's)

namespace optiperf {

enum SysObject { kSysSystem, kSysTimer, kSysObjects };

// One getter the snapshot answers, as found in CrySystem.dll: its object (ISystem or ITimer), vtable slot, name, and
// function (null if its code is not the expected one: that getter is then left to CrySystem).
struct SysGetter {
    int object = 0;
    int slot = 0;
    const char* name = nullptr;
    char* fn = nullptr;
};
constexpr int kSysGetters = 11;

// Checks the getters in the CSystem and CTimer vtables (their first `slots` entries point into the module's code):
// fills out[kSysGetters] and returns how many are as expected, or -1 if a vtable is too short (the snapshot also needs
// the setters' slots, which make it out of date when called).
int SysSnapshot_Locate(char* sysBase, void* const* systemVtable, int systemSlots, void* const* timerVtable,
                       int timerSlots, SysGetter* out);

// From shadow_prepass.cpp: once the shadow pre-pass is installed (reads the setting; the rest is set up at the first
// flush); right before the outer EF_EndEf3D runs (last, after the environment snapshot's capture) and right after
// it; for its reports.
void SysSnapshot_Install(const wchar_t* moduleName, void** system);
void SysSnapshot_BeforeFlush();
void SysSnapshot_AfterFlush();
void SysSnapshot_Report(const char* when);

}  // namespace optiperf
