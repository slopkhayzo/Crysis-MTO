#pragma once
// The producers' time (a diagnostic for the double-buffered frame, research/docs/render-thread.md §12): the calls the
// main thread makes into the renderer's producer methods (EF_GetObject, EF_AddEf, a render mesh's Render and its
// updates, a render object's waves, ...), timed (the outermost call: what a replay of them would cost, their callbacks
// into the 3D engine included), per frame phase (with FramePhases=on) and apart at the flush's entry (optiperf's own
// work there: the shadow casters, the reflections). Their slots are patched in the renderer's vtables: not with the
// call tracer, nor with FlushThread=overlap (the gate's thunks are in those slots). x64.
//
// [Debug] ProducerTiming = off (default) | on     (env OPTIPERF_PRODUCER_TIMING)

namespace optiperf {

void ProducerTiming_Install(const wchar_t* moduleName, char* rendererBase);
// From the shadow pre-pass's outer EF_EndEf3D hook, on the main thread: its entry, and the flush's start (a frame).
void ProducerTiming_OnFlushEntry();
void ProducerTiming_OnFlushStart();
void ProducerTiming_Report(const char* when);

}  // namespace optiperf
