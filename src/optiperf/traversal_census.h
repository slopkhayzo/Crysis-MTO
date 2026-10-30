#pragma once
// Traversal census (a diagnostic for the parallel traversal, Phase 4: research/docs/main-thread.md §4). What the main
// thread reads and writes of the engine's memory in two windows of the 3D engine's scene drawing: the shadow casters'
// walk (C3DEngine::InitShadowFrustums), then the rest of the drawing after it, up to C3DEngine::UpdateRNTmpDataPool.
// Memory one window writes and the other reads or writes is what the walk could not do beside the rest. On a few
// frames (one in N, at most M a run) the pages of mimalloc's arenas and of the Cry* modules' writable sections are
// guarded; each page's first access in a window is logged with its code and call chain, and a page first read is made
// read-only so that a later write is logged too. Other threads' accesses are logged apart (a page they touch is guarded
// again after them, twice at most a window). The records go to traversal_census.tsv next to optiperf.log, with the heap
// blocks' allocating code (research/profiling/jobs/tools/census_conflicts.py reads it).
//
// With TraversalCensus=fields each such page census is followed, the next frame, by a field census of the pages both
// windows touched with one writing them: every access to them logged (the page guarded again after each instruction,
// 2000 accesses a page and window at most), for the words they share.
//
// TraversalCensus=particles watches CPartManager::Update instead (one window), each emitter's update in it a unit
// (CParticleEmitter vtable slot 12): records carry the unit, for the words one emitter writes and another touches.
//
// [Debug] TraversalCensus = off (default) | casters | fields | particles  (env OPTIPERF_TRAVERSAL_CENSUS); x64 DX10
//         TraversalCensusEvery = 400                 (env OPTIPERF_TRAVERSAL_CENSUS_EVERY: one frame in N)
//         TraversalCensusMax = 3                     (env OPTIPERF_TRAVERSAL_CENSUS_MAX: censuses a run)
// Needs [Allocator] Owners=on for the blocks' owners. Run it with the traversal after the flush
// ([ShadowPrepass] FlushThreadRecord=off) and the frame's end on the main thread (FlushThreadPresent=off): the render
// thread is idle in the windows then. A census frame takes a few hundred ms.

namespace optiperf {

struct ShadowPrepassTarget;

void TraversalCensus_Install(const wchar_t* moduleName, const ShadowPrepassTarget& sp);
void TraversalCensus_FlushEntry();  // from the shadow pre-pass's outer EF_EndEf3D, on the main thread
void TraversalCensus_Report(const char* when);

}  // namespace optiperf
