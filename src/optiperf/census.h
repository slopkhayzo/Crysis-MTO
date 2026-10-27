#pragma once
// Pointer census (a diagnostic for the render thread, Phase 2): what the objects the flush works from point at in
// memory other modules own. Off by default; x64 DX10 renderer only; driven by the shadow pre-pass's EF_EndEf3D hook
// (shadow_prepass.cpp). Needs the allocator's owner tags ([Allocator] Owners=on). See census.cpp and
// research/docs/render-thread.md.
//
// [ShadowPrepass] Census = off (default) | on     (env OPTIPERF_SHADOW_PREPASS_CENSUS)
//                 CensusEvery = 64                (env OPTIPERF_SHADOW_PREPASS_CENSUS_EVERY: one flush in N)
//                 CensusChanges = off (default) | on   (env OPTIPERF_SHADOW_PREPASS_CENSUS_CHANGES: also what of the
//                 targets the flush and then the main thread's next frame write)
//                 CensusReads = off (default) | on     (env OPTIPERF_SHADOW_PREPASS_CENSUS_READS: every access in the
//                 census flush, not just each page's first; turns CensusChanges on)

#include <cstddef>

namespace optiperf {

struct ShadowPrepassTarget;

// From shadow_prepass.cpp: once the renderer is known; right before the outer EF_EndEf3D runs, after the other parts'
// captures (the items are all in place then); for its reports.
void Census_Install(const wchar_t* moduleName, char* rendererBase, size_t rendererSize, const ShadowPrepassTarget& sp);
void Census_BeforeFlush(char* renderer);
void Census_AfterFlush();  // (right after the outer EF_EndEf3D: the census flush's pages unguarded)
void Census_Report(const char* when);

}  // namespace optiperf
