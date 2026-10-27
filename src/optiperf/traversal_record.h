#pragma once
// The traversal's producer calls recorded (the double-buffered frame, research/docs/render-thread.md §12.8): from the
// main traversal's EF_StartEf to its outer EF_EndEf3D, each outermost call of the renderer's producers on the main
// thread, with what it points to on the stack and the render objects it passes, as they are at the call. x64, with
// FlushThread=overlap (the gate's redirects).
//   - verify: the calls are made as in vanilla, and recorded. At the outer flush's entry the record is replayed after
//     the traversal's items, at its level (the recorded objects mapped to the replay's), what each replayed call added
//     to the render lists and left in its object compared with what the recorded call did, and the lists cut back
//     (the flush gets vanilla's). What step 5 would answer instead of each call is predicted and checked too.
//   - on (step 5.2, the barrier still before the traversal): the calls are recorded and not made. The 3D engine gets
//     stand-ins (render objects, their data and wave forms, a polygon's storage) and the predicted answers. At the
//     flush's entry the record is made for real, EF_StartEf first: what the 3D engine wrote into each stand-in merged
//     into its real object, the answers it was given checked. A call that can't be answered (a level inside the
//     traversal, a new permanent object, ...) joins: the record so far is made, and the traversal's calls are made from
//     there on, its stand-ins mapped to their real objects. The first frame's calls are made, to learn what
//     EF_StartEf leaves and a temporary object's template.
// Recorded so far: EF_StartEf, EF_GetObject, EF_AddEf, IRenderMesh's Render, AddRenderElements and AddRE,
// PushFogVolumeContribution, CRenderObject's AddWaves and GetObjData, EF_AddParticlesToScene, EF_AddPolygonToScene
// (both forms), EF_ADDDlight.
//
//   - beside (step 5.3): as on, and the traversal runs beside the flush (the 3D engine's RenderWorld, the barrier,
//     passes the gate once the record can answer it); its first call that has to wait for the flush joins it there,
//     and the record is made then. EF_IsFakeDLight and EF_UpdateDLight are made beside the flush; a render mesh's
//     SetMaterial or SetRECustomData that would change nothing isn't made. A traversal that starts with no flush beside
//     it (the update joined it, or it has ended) is made, not recorded, and one stops being answered once the flush
//     beside it has ended. With FlushThreadRecordObserve=on, one joined traversal in eight is observed for the rest of
//     it: the calls that would have joined too are logged.
//
// [ShadowPrepass] FlushThreadRecord = off (default) | verify | on | beside    (env OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD_RECORD)
//                 FlushThreadRecordObserve = off (default) | on        (env OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD_RECORD_OBSERVE)

#include "shadow_prepass.h"

namespace optiperf {

// From the shadow pre-pass's install, after the flush thread's (overlap: the renderer's gate in place, the render
// queue's redirects made: EF_AddEf's slot then goes to the recorder, which tells the queue of each call).
void TraversalRecord_Install(const wchar_t* moduleName, char* base, const ShadowPrepassTarget& t);
// At the outer EF_EndEf3D's entry, on the main thread, before the parts' entry work (the previous flush joined).
void TraversalRecord_OnFlushEntry(char* renderer);
void TraversalRecord_Report(const char* when);
// beside (step 5.3), for the render queue's gate: the record's slots pass while it answers the traversal's calls, and
// the next traversal (the 3D engine's RenderWorld, the barrier) may start beside the flush once it can answer them
bool TraversalRecord_Passes(unsigned gateIndex);
bool TraversalRecord_TakesTraversal();
// At the entry of any EF_EndEf3D on the main thread, before its level is read: a traversal still answered (its
// EF_StartEf not made, so the renderer's level isn't the traversal's yet) is made now, the flush beside it joined first.
void TraversalRecord_OnEndEf3D();
// From the flush's join on the main thread (after the parts' steps after it and the queue's taken calls): a traversal
// it was answering is made now (the record so far), and its calls are made from there on. The gate's thunk it joined
// at (the reports'), or ~0u; nextFlush: the join at the next flush's entry (the whole traversal ran beside the flush).
void TraversalRecord_OnJoined(unsigned gateIndex, bool nextFlush);
// From the gate's callback with no flush beside the main thread: after a join, one traversal in eight keeps the gate
// on for the rest of it, and each of its calls that would have joined is counted (the gate's thunk; the call's return
// address, into its caller).
void TraversalRecord_OnWouldJoin(unsigned gateIndex, void* caller);

}  // namespace optiperf
