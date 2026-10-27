#pragma once
// Particles before the flush (Phase 2, after the environment snapshot): the particle containers' vertices, which the
// flush asks the 3D engine for while drawing, produced right before it and replayed where the flush draws them. x64
// DX10 renderer only; driven by the shadow pre-pass's EF_EndEf3D hook (shadow_prepass.cpp). See particle_prepass.cpp
// and research/docs/particles.md.
//
// [ShadowPrepass] Particles = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_PARTICLES)
//   needs Mode other than off (the hook is the shadow pre-pass's)

#include <cstddef>
#include <cstdint>

namespace optiperf {

struct ShadowPrepassTarget;

// What the particle part uses in a loaded CryRenderD3D10.dll (x64), as read from the code itself.
struct ParticleTarget {
    char* prepare = nullptr;       // CREParticle::mfPrepare: makes the allocator and the context, calls SetVertices
    void** allocVtable = nullptr;  // CAllocRender's vtable (the allocator mfPrepare makes on its stack)
    char* alloc = nullptr;         // CAllocRender::Alloc: commits what was written, reserves in the particle batch
    char* reserve = nullptr;       // the reservation it calls: grants min(request, buffer size)
    char* addFillLight = nullptr;  // EF_ADDFillLight (IRenderer slot 119, checked at the first flush)
};

// Finds and validates the code in a mapped CryRenderD3D10.dll. On failure returns false and sets *why.
bool ParticlePrepass_Locate(char* moduleBase, ParticleTarget* out, const char** why);
// Whether fn is the vertex creators' SetVertices in a mapped Cry3DEngine.dll (CParticleContainer's).
bool ParticlePrepass_IsSetVertices(char* e3dBase, char* fn);
// The per-particle vertex function SetVertices calls when it computes the vertices itself (whose shares of the
// container's estimates the pre-runs record), in a mapped Cry3DEngine.dll; null with *why set if it isn't as expected.
char* ParticlePrepass_LocateParticleVerts(char* e3dBase, const char** why);

// From shadow_prepass.cpp: once the shadow pre-pass is installed (reads the setting, locates, hooks); at the outer
// EF_EndEf3D's entry (before the reflections' dry run); at a reflection's nested EF_EndEf3D that the dry run takes
// (its particle elements, pre-run with the reflection's camera, are drawn when the flush replays it); right before the
// outer EF_EndEf3D runs (the main pass's elements; after the reflections' dry run, before the environment snapshot's
// capture) and right after; for its reports.
void ParticlePrepass_Install(const wchar_t* moduleName, char* base, const ShadowPrepassTarget& t);
void ParticlePrepass_OnFlushEntry(char* renderer);
void ParticlePrepass_OnNestedLevel(char* renderer, int level);
void ParticlePrepass_BeforeFlush(char* renderer);
void ParticlePrepass_AfterFlush();
void ParticlePrepass_Report(const char* when);

}  // namespace optiperf
