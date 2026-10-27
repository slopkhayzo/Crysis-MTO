#pragma once
// Frame phases (a diagnostic for the render thread, research/docs/render-thread.md): the main thread's frame cut at the
// game loop's calls into CrySystem (ISystem::RenderBegin, Update, Render, RenderEnd) and at the renderer's outer flush.
// Each part is timed, and the call tracer is told which part a call was made in (CallTrace_SetPhase, trace_report.py
// --phase). The time is taken per interval from one outer flush's start to the next one's, so that the report can also
// estimate a frame with the flush on another thread (frame_phases.cpp). x64 only; driven by the shadow pre-pass's
// EF_EndEf3D hook, the other hooks put in at the second flush.
//
// [Debug] FramePhases = off (default) | on     (env OPTIPERF_FRAME_PHASES)

namespace optiperf {

enum FramePhase : int {  // in a frame's order
    kPhaseOther = 0,   // before the first frame
    kPhaseBegin = 1,   // ISystem::RenderBegin (the renderer's BeginFrame)
    kPhaseUpdate = 2,  // ISystem::Update: the systems' update (entities, AI, animation, physics' results, sound, ...)
    kPhaseGame = 3,    // the game's own code from RenderBegin to ISystem::Render, outside Update
    kPhaseWorld = 4,   // ISystem::Render before the outer flush: the 3D engine's scene traversal (RenderWorld is all of
                       // Render but 3 us; the flush is marked apart: CallTrace_SetFlush)
    kPhaseTail = 5,    // ISystem::Render after the outer flush
    kPhasePost = 6,    // the game's code from Render's return to RenderEnd (the HUD, ...)
    kPhaseEnd = 7,     // ISystem::RenderEnd (the renderer's EndFrame: the present)
    kPhaseAfter = 8,   // after RenderEnd, until the next frame's RenderBegin
    kPhases = 9,
};

void FramePhases_Install(const wchar_t* moduleName, void** system);
// From the EF_EndEf3D hook, at level 1: at its entry (optiperf's work before the flush follows: the shadow pre-pass,
// the reflections' dry run, the captures), right before the original runs, and right after it.
void FramePhases_FlushEntry();
void FramePhases_FlushStart();
void FramePhases_FlushEnd();
void FramePhases_Report(const char* when);

}  // namespace optiperf
