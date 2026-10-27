#pragma once
// Skinning poses before the flush (Phase 2, after the particles): the bone poses the flush asks the animation system
// (and the 3D engine's touch-bendable plants) for while drawing skinned objects, asked for right before it and answered
// from those records where the flush draws them. x64 DX10 renderer only; driven by the shadow pre-pass's EF_EndEf3D
// hook (shadow_prepass.cpp). See skin_prepass.cpp and research/docs/skinning.md.
//
// [ShadowPrepass] Skinning = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_SKINNING)
//   needs Mode other than off (the hook is the shadow pre-pass's)

#include <cstddef>
#include <cstdint>

namespace optiperf {

struct ShadowPrepassTarget;

enum SkinImpl { kSkinChar, kSkinSkin, kSkinFoliage, kSkinImpls };

// What the skinning part uses, as read from the code of the mapped modules.
struct SkinTarget {
    char* callReturn = nullptr;       // CryRenderD3D10: where its skinned draw's GetSkeletonPose call returns
    char* isHWSkinned = nullptr;      // CREMesh::mfIsHWSkinned (render element slot 8): whether the draw asks for a pose
    void** vtables[kSkinImpls] = {};  // CCharInstance, CSkinInstance (CryAnimation); CStatObjFoliage (Cry3DEngine)
    char* poses[kSkinImpls] = {};     // their GetSkeletonPose (ISkinnable slot 2)
    // CryAnimation's MorphTargetCopy, the morph targets' copy into the render mesh that each GetSkeletonPose call
    // makes: its two paths (the morph targets' blend, a linear morph sequence), and the renderer pointer it uses
    char* morphCopy = nullptr;
    char* morphSequence = nullptr;
    void** animRenderer = nullptr;
};

// The renderer's part (the call site, the elements' skinned test), from a mapped CryRenderD3D10.dll. On failure
// returns false and sets *why.
bool SkinPrepass_LocateRenderer(char* renderBase, SkinTarget* out, const char** why);
// What a render element's slot 8 (mfIsHWSkinned) is, given the renderer's part: 0 = returns false, 1 = CREMesh's
// (its chunk's bone count), 2 = other code.
int SkinPrepass_ElementKind(char* renderBase, const SkinTarget& t, void* const* elementVtable);
// The skinnables' part: anim from CryAnimation.dll, e3d from Cry3DEngine.dll. On failure returns false and sets *why.
bool SkinPrepass_LocateSkinnables(char* animBase, char* e3dBase, SkinTarget* out, const char** why);
// The morph targets' copy in CryAnimation.dll. Without it, calls that apply morph targets keep vanilla's call.
bool SkinPrepass_LocateMorphCopy(char* animBase, SkinTarget* out, const char** why);

// From shadow_prepass.cpp: once the shadow pre-pass is installed; right before the outer EF_EndEf3D runs (after the
// shadow pre-pass, the reflections' dry run and the particles; before the environment snapshot's capture) and right
// after; for its reports.
void SkinPrepass_Install(const wchar_t* moduleName, char* base, const ShadowPrepassTarget& t);
void SkinPrepass_BeforeFlush(char* renderer);
void SkinPrepass_AfterFlush();
void SkinPrepass_Report(const char* when);

}  // namespace optiperf
