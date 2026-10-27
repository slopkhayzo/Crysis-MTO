#pragma once
// Engine data (the render thread's groundwork, item 1): the 3D engine's other per-frame data the flush reads through
// pointers, copied right before it: the sky's parameters and texel data, the ocean's (its grid's and its fog volume's,
// with the fog's vertices), the water volumes' parameters, and the terrain texture sets the far-tree sprites point at.
// While the flush runs, the renderer's pointers to them point at the copies. x64 DX10 renderer only; driven by the
// shadow pre-pass's EF_EndEf3D hook (shadow_prepass.cpp). See engine_data.cpp, range_copies.h and
// research/docs/render-thread.md.
//
// [ShadowPrepass] EngineData = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_ENGINE_DATA)
//   needs Mode other than off (the hook is the shadow pre-pass's); the texture sets need FarTrees=on (the sprites are
//   made before the flush then)

#include <cstddef>

namespace optiperf {

struct ShadowPrepassTarget;

// From shadow_prepass.cpp: once the shadow pre-pass is installed (reads the setting; the renderer's sky field is found
// at the first flush); right before the outer EF_EndEf3D runs (after the other parts' captures and the lights, before
// the census) and right after it; for its reports.
void EngineData_Install(const wchar_t* moduleName, char* rendererBase, size_t rendererSize,
                        const ShadowPrepassTarget& sp);
void EngineData_BeforeFlush(char* renderer);
void EngineData_AfterFlush();
void EngineData_Report(const char* when);
// While a flush whose pointers the hand-off pointed at the copies runs (until its after-flush step): a pointer into
// one of its copies, the original's same byte; else p (beside the flush, a field compared with what it held before the
// hand-off, which the after-flush step puts back).
const char* EngineData_Original(const char* p);

}  // namespace optiperf
