#pragma once
// The thread the renderer's outer flush runs on (the render thread's groundwork, item 2; research/docs/render-thread.md
// §10–§11). With FlushThread=worker it is a thread of optiperf's own, and the main thread waits while it runs the
// flush: the same work in the same order, on another thread. With FlushThread=overlap the main thread goes on beside
// it, until its first call into the renderer (the renderer's gate, render_gate.h, makes that call wait for the flush)
// or the next outer flush. The parts answer the flush's calls on the flush's thread; their hand-off, pre-runs and
// captures stay on the main thread. It needs the shadow pre-pass (its EF_EndEf3D hook) and x64.
//
// [ShadowPrepass] FlushThread = main (default) | worker | overlap     (env OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD)
//                 FlushThreadObserve = off (default) | on    (env OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD_OBSERVE): with
//                 overlap, one join in eight keeps the main thread's renderer calls watched to its frame's end, and the
//                 report lists those that would have joined too
//                 FlushThreadPresent = off (default) | on    (env OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD_PRESENT): with
//                 overlap, the frame's end on the flush's thread: the calls the render queue takes beside the flush up to
//                 the present (EndFrame) are sealed there, and the worker makes them right after the flush, while the
//                 main thread goes on; a join then waits for them too, letting messages sent to the main thread through
//                 (DXGI's, from a present away from the window's thread)
//                 FlushThreadPlacement = default (default) | apart
//                 (env OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD_PLACEMENT): with worker or overlap, default leaves the two
//                 threads where the OS puts them; apart gives the main thread and the worker a physical core each and
//                 keeps the process's other threads off both (the cores: the most performant class, one last-level
//                 cache, not processor 0's, two left for the rest). Either way the report says where the two ran at the
//                 hand-offs (on one physical core, or apart).

namespace optiperf {

// Reads the setting (from the shadow pre-pass's install, with the renderer's base: the CRT is the one it imports, and
// with overlap its vtables get the gate; endEf3D, located by the pre-pass, checks the render queue's vtable slots).
void FlushThread_Install(const wchar_t* moduleName, char* rendererBase, char* endEf3D);

// At the outer flush's entry, on the main thread, before the parts' hand-off: a flush still running beside the main
// thread is joined (overlap), and from the first one on the flush's thread is known (the worker starts then), so that
// the hand-off can tell its counters (vtable_counter.h) which thread marks.
void FlushThread_OnEntry();

// The flush's thread, from the first outer flush on: the main thread, or the worker with FlushThread=worker|overlap.
// 0 before.
unsigned long FlushThread();
bool OnFlushThread();  // whether the calling thread is it
// Whether the calling thread is the main thread while a flush runs beside it (overlap: until it joins the flush).
bool FlushThread_Beside();
// Overlap, on the main thread beside the flush: join it now (the render queue, for a call it can't take after all).
void FlushThread_JoinNow();
// Whether the worker is done with the flush handed off last, and with the frame's end it makes after it (a join would
// not wait for them).
bool FlushThread_Ended();
// Whether the flush handed off last has ended (its marks are all set; the frame's end may still be on the worker).
bool FlushThread_FlushEnded();
// FlushThreadPresent=on, on the main thread beside the flush, from the render queue's taken EndFrame: whether this
// window's frame's end is still open (the setting on, not sealed yet); then the calls it has taken so far (moved where
// the worker reads them) are the frame's end, for the worker to make after the flush. False if they aren't the
// worker's after all: the join makes them.
bool FlushThread_SealOpen();
bool FlushThread_Seal();
// FlushThreadPresent=on, from the render queue at the first flush: the game window's procedure, hooked if it is in one
// of the game's modules. A join asked for from inside it while the worker makes the frame's end waits a bounded time:
// the message may come from inside DXGI's own handling of the window, which holds DXGI's lock that the worker's present
// waits for.
void FlushThread_WatchWindowProc(void* proc);
// When the last join of a flush beside the main thread began (the time stamp counter's; 0: none yet).
unsigned long long FlushThread_JoinStartedAt();
// FlushThreadPlacement=apart, once applied (the first outer flush): the processors the process's other threads are kept
// on (group 0), else 0.
unsigned long long FlushThread_OthersMask();

// The outer flush, called on the main thread: fn(ctx) on the flush's thread, inside the call tracer's flush mark, then
// after(afterCtx) on the main thread. With main and worker it returns when both are done. With overlap it returns
// once the worker has the flush, and after() runs when the main thread joins it (ctx and afterCtx must outlive the
// call). On the worker, fn gets the main thread's floating-point control (MXCSR), the C runtime's random seed (msvcr80
// keeps rand's state per thread) and the tracer's phase; with worker, the first two come back with it.
void FlushThread_Run(void (*fn)(void*), void* ctx, void (*after)(void*), void* afterCtx);

void FlushThread_Report(const char* when);

}  // namespace optiperf
