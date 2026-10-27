#pragma once
// Call tracer (developer tool, off by default): counts calls through the vtables of engine classes per method, caller
// and thread. Used to map the interfaces between modules for the render thread (Phase 2 of the plan).
//
// [Trace] Modules = (empty: off) | renderer | all | comma list of DLL names     (env OPTIPERF_TRACE)

namespace optiperf {

void CallTrace_Init();  // reads the settings; must run before the first CallTrace_OnModuleLoaded
void CallTrace_OnModuleLoaded(const wchar_t* baseName, char* base, size_t size);
void CallTrace_Report(const char* when);

// Whether the calling thread is inside the renderer's outer flush (EF_EndEf3D at level 1): calls it makes then are
// counted apart per call site (trace_report.py --flush). Set on the flush's thread around the shadow pre-pass's
// outer flush (flush_thread.cpp). x64 only.
void CallTrace_SetFlush(bool inFlush);
// The frame phase (0-15) the calling thread is in: calls it makes are counted apart per phase (trace_report.py --phase).
// Set on the main thread by the frame phases (frame_phases.h). x64 only.
void CallTrace_SetPhase(int phase);
// The calling thread's phase, as set last (for the flush's thread to take the main thread's: flush_thread.h).
int CallTrace_Phase();
// The function a vtable entry calls: the original behind the tracer's thunks, the renderer gate's (render_gate.h) and
// the hooks registered below, else the entry itself.
void* CallTrace_Original(void* entry);
// A hook another part put in a vtable slot, forwarding to the entry it replaced: seen through by CallTrace_Original
// (the vtables' lengths are measured through it: vtable_counter.h, FindRttiVtables). From the install, before the
// other threads call.
void CallTrace_SeeThrough(void* hook, void* original);

}  // namespace optiperf
