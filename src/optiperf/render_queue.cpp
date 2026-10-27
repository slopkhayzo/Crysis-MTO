// The renderer calls taken beside the flush: see render_queue.h.
//
// The methods are found behind their IRenderer slots in CD3D9Renderer's vtable (the Mod SDK's layout), checked by
// slot 115 being the EF_EndEf3D the shadow pre-pass located from its code. Their gate thunks go on to the functions
// here (RenderGate_Redirect) rather than the methods' code being patched: other parts find EnableFog by its code. On
// the main thread while a flush runs beside it (FlushThread_Beside), the gate lets those slots through without joining,
// and:
//   - a taken call (no result: the 3D engine's EnableFog and DeleteRenderMesh after the flush; the HUD's 2D state and
//     image draws, the font's state and the renderer's slot 214 it sets; the aux geometry's Flush; the text's vertex
//     locks and draws, through a buffer of the queue's own; the system's RenderDebug at the frame's end; the post
//     effects' parameters set, with their names and strings copied; the first-person objects' field of view, through
//     EF_Query; the system's EndFrame, the present; Draw2dLabel, its text formatted; the per-frame random seed event
//     to the renderer's system event listener; BeginFrame, once the next RenderWorld is the barrier; the clear color;
//     the aux geometry's render flags; a render mesh's AddRef; a shader's and a texture's AddRef, their answers
//     predicted; the data uploaded into a texture that exists, copied; the viewport; the default materials; the
//     camera, copied; the 3D engine's sky light parameters; the terrain's ambient occlusion nodes, copied; the Flash
//     players' draws, the HUD's and the menus' movies, their parameters, vertices, indices and transform copied, with
//     their stencil masks and clears; or, where those can't be taken, a player's whole render through CrySystem's
//     CFlashPlayer vtable) is recorded and returns;
//   - an answered call (queries: the screen's size and scale, the frame id, the features, the aux geometry's object,
//     a texture's id, the window, the render type, a shader type's quality, EF_Query's settings, the multi-GPU mode;
//     ProjectToScreen, GetViewport and GetCamera, from the camera copied at the hand-off (and the taken SetCamera's);
//     the render meshes', the shaders' and the shader resources' getters that only read a field of theirs, and a
//     texture's flags; the meshes' positions and indices read; the ocean's grid, once the flush has drawn the ocean;
//     the post effects' float and Vec4 parameters that don't blend, looked up by the queue; the shader resources'
//     color and float references, through bytes of the queue's own) is made at once, and its arguments and answer
//     recorded; the aux geometry's render flags answer the last taken, and the frame's draw and polygon counts the
//     last flush's (one frame late);
//   - a few methods that only read what the main thread alone writes, or do nothing (kPureSlots) are made at once,
//     with no record.
// At the join the recorded calls go through in their order: each taken call is made, and each answered call made
// again at its place, which has to give the same bits, else the flush changed what it reads (counted, the first
// logged). Any other call goes on as before.
#include "render_queue.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#include "calltrace.h"
#include "config.h"
#include "console_gate.h"
#include "diag.h"
#include "flush_thread.h"
#include "hooks.h"
#include "log.h"
#include "reflection_prepass.h"
#include "render_gate.h"
#include "scan.h"
#include "traversal_record.h"

namespace optiperf {
namespace {

#ifdef _WIN64
constexpr unsigned kSlotEndEf3D = 115;
constexpr int kLoggedDiffer = 8, kLoggedPostJoins = 8;

enum Kind : uint8_t {
    // taken
    kDeleteRenderMesh, kEnableFog, kEnableTMU, kSelectTMU, kSet2DMode, kSetCullMode, kSetColorOp, kSetState,
    kDraw2dImage, kFontSetTexture78, kFontSetTexture79, kFontSetRenderingState, kFontSetBlending,
    kFontRestoreRenderingState, kVf214, kAuxFlush, kGetDynVBPtr, kDrawDynVB, kRenderDebug, kPostSet, kPostSetVec4,
    kPostSetString, kEF_Query, kFlushTextMessages, kEndFrame, kDraw2dLabel, kOnSystemEvent, kBeginFrame,
    kSetClearColor, kAuxSetRenderFlags, kMeshAddRef, kShaderAddRef, kTextureAddRef, kDownLoad, kSetViewport,
    kSetDefaultMaterials, kSetCamera, kShaderRelease, kSetSkyLight, kFlashRender, kSetTerrainAO, kSFConfigMask,
    kSFDrawTriList, kSFDrawLineStrip, kSFDrawGlyphClear, kSFFlush, kClearBuffer,
    // answered
    kGetWidth, kGetHeight, kScaleCoordX, kScaleCoordY, kGetFrameID, kGetFeatures, kGetIRenderAuxGeom, kGetTextureID,
    kProjectToScreen, kGetViewport, kGetHWND, kGetRenderType, kGetShaderQuality, kEF_QuerySetting,
    kIsMultiGPUModeActive, kAuxGetRenderFlags, kFieldGetter, kOceanGrid, kOceanPoint, kPostGet, kGetCamera,
    kPostGetVec4, kResRef, kMeshPosPtr, kMeshIndices, kGetModelView, kGetProjection, kUnProject,
    // answered from the last flush's (one frame late beside the flush: checked, the difference counted, not logged)
    kGetCurrentNumberOfDrawCalls, kGetPolyCount0, kGetPolyCount2, kKinds
};
constexpr Kind kFirstAnswered = kGetWidth, kFirstLagged = kGetCurrentNumberOfDrawCalls;
enum Class : uint8_t {
    kRenderer, kTexture, kAuxGeom, kPostProcess, kSysEvents, kRenderMesh, kShader, kOcean, kClasses
};
const char* const kClassNames[kClasses] = {".?AVCD3D9Renderer@@", ".?AVCTexture@@", ".?AVCD3DRenderAuxGeom@@",
                                           ".?AVCREPostProcess@@", ".?AUCSystemEventListner_Render@@",
                                           ".?AUCRenderMesh@@", ".?AVCShader@@", ".?AVCREWaterOcean@@"};
struct Def {
    uint8_t cls;
    unsigned slot;
    const char* name;
};
constexpr unsigned kDynamicSlot = ~0u;  // (the render meshes' getters: their slots are found at the install)
// (ProjectToScreen, answered from the live renderer, differed after the flush in 44-603 calls per run: it reads the
// camera the flush sets; it answers from the hand-off's copy. FontSetTexture's two overloads, (int, int) and
// (CFBitmap*, int), take only integers and pointers: one hook forwards either. CREPostProcess's slots 22-24 are what
// the 3D engine's SetPostEffectParam{,Vec4,String} forward to, declared void in the Mod SDK's I3DEngine; its getters
// (25-27) go on to the post effects' manager through its vtable, which the gate has too: they join.)
const Def kDefs[] = {
    {kRenderer, 127, "DeleteRenderMesh"},      {kRenderer, 138, "EnableFog"},
    {kRenderer, 147, "EnableTMU"},             {kRenderer, 148, "SelectTMU"},
    {kRenderer, 171, "Set2DMode"},             {kRenderer, 137, "SetCullMode"},
    {kRenderer, 141, "SetColorOp"},            {kRenderer, 136, "SetState"},
    {kRenderer, 54, "Draw2dImage"},            {kRenderer, 78, "FontSetTexture"},
    {kRenderer, 79, "FontSetTexture"},         {kRenderer, 80, "FontSetRenderingState"},
    {kRenderer, 81, "FontSetBlending"},        {kRenderer, 82, "FontRestoreRenderingState"},
    {kRenderer, 214, "CD3D9Renderer::vf214"},  {kAuxGeom, 26, "IRenderAuxGeom::Flush"},
    {kRenderer, 28, "GetDynVBPtr"},            {kRenderer, 30, "DrawDynVB"},
    {kRenderer, 18, "RenderDebug"},            {kPostProcess, 22, "SetPostEffectParam"},
    {kPostProcess, 23, "SetPostEffectParamVec4"}, {kPostProcess, 24, "SetPostEffectParamString"},
    {kRenderer, 121, "EF_Query(EFQ_DrawNearFov)"}, {kRenderer, 160, "FlushTextMessages"},
    {kRenderer, 19, "EndFrame"},               {kRenderer, 133, "Draw2dLabel"},
    {kSysEvents, 0, "OnSystemEvent(ESYSTEM_EVENT_RANDOM_SEED)"}, {kRenderer, 17, "BeginFrame"},
    {kRenderer, 124, "SetClearColor"},         {kAuxGeom, 0, "IRenderAuxGeom::SetRenderFlags"},
    {kRenderMesh, 0, "IRenderMesh::AddRef"},   {kShader, 1, "IShader::AddRef"},
    {kTexture, 0, "ITexture::AddRef"},         {kRenderer, 149, "DownLoadToVideoMemory"},
    {kRenderer, 21, "SetViewport"},           {kRenderer, 198, "SetDefaultMaterials"},
    {kRenderer, 43, "SetCamera"},             {kShader, 2, "IShader::Release"},
    {kRenderer, 193, "SetSkyLightRenderParams"},
    {kRenderer, kDynamicSlot, "IFlashPlayer::Render"},  // (CrySystem's CFlashPlayer, its vtable patched)
    {kRenderer, 142, "SetTerrainAONodes"},
    {kRenderer, 183, "SF_ConfigMask"},         {kRenderer, 184, "SF_DrawIndexedTriList"},
    {kRenderer, 185, "SF_DrawLineStrip"},      {kRenderer, 186, "SF_DrawGlyphClear"},
    {kRenderer, 187, "SF_Flush"},              {kRenderer, 165, "ClearBuffer"},
    {kRenderer, 58, "GetWidth"},               {kRenderer, 59, "GetHeight"},
    {kRenderer, 134, "ScaleCoordX"},           {kRenderer, 135, "ScaleCoordY"},
    {kRenderer, 128, "GetFrameID"},            {kRenderer, 7, "GetFeatures"},
    {kRenderer, 143, "GetIRenderAuxGeom"},     {kTexture, 8, "ITexture::GetTextureID"},
    {kRenderer, 65, "ProjectToScreen"},        {kRenderer, 20, "GetViewport"},
    {kRenderer, 174, "GetHWND"},
    {kRenderer, 2, "GetRenderType"},           {kRenderer, 98, "EF_GetShaderQuality"},
    {kRenderer, 121, "EF_Query(a setting)"},  // (the same slot and hook as the taken EF_Query)
    {kRenderer, 199, "IsMultiGPUModeActive"},  {kAuxGeom, 1, "IRenderAuxGeom::GetRenderFlags"},
    {kRenderMesh, kDynamicSlot, "the field getters"},
    {kOcean, 24, "CREWaterOcean's grid"},      {kOcean, 23, "CREWaterOcean's grid point"},
    {kPostProcess, 25, "GetPostEffectParam"},  {kRenderer, 44, "GetCamera"},
    {kPostProcess, 26, "GetPostEffectParamVec4"},
    {kRenderer, kDynamicSlot, "IRenderShaderResources' references"},
    {kRenderMesh, 9, "IRenderMesh::GetStridedPosPtr"}, {kRenderMesh, 8, "IRenderMesh::GetIndices"},
    {kRenderer, 68, "GetModelViewMatrix"},     {kRenderer, 69, "GetProjectionMatrix"},
    {kRenderer, 67, "UnProjectFromScreen"},
    {kRenderer, 9, "GetCurrentNumberOfDrawCalls"},
    {kRenderer, 123, "GetPolyCount()"},        {kRenderer, 122, "GetPolyCount(int&, int&)"}};
static_assert(std::size(kDefs) == kKinds, "a kind's definition is missing");

using DeleteRenderMeshFn = void (*)(void*, void*);
using BoolFn = bool (*)(void*, bool);  // EnableFog
using VoidBoolFn = void (*)(void*, bool);
using VoidIntFn = void (*)(void*, int);
using Set2DModeFn = void (*)(void*, bool, int, int, float, float);
using SetColorOpFn = void (*)(void*, uint8_t, uint8_t, uint8_t, uint8_t);
using SetStateFn = void (*)(void*, int, int);
using Draw2dImageFn = void (*)(void*, float, float, float, float, int, float, float, float, float, float, float, float,
                               float, float, float);
using IntFn = int (*)(void*);
using IntIntFn = int (*)(void*, int);
using ScaleFn = float (*)(void*, float);
using FrameIdFn = int (*)(void*, bool);
using Ints2Fn = void (*)(void*, uint64_t, uint64_t);  // integer and pointer arguments only
using Ints1Fn = void (*)(void*, uint64_t);
using GetDynVBPtrFn = void* (*)(void*, int nVerts, int& nOffs, int pool);
using DrawDynVBFn = void (*)(void*, int nOffs, int pool, int nVerts);
using Ints0Fn = void (*)(void*);
using PtrFn = void* (*)(void*);
using ProjectFn = uint8_t (*)(void*, float, float, float, float*, float*, float*);
using ViewportFn = void (*)(void*, int*, int*, int*, int*);
using ClearColorFn = void (*)(void*, const float*);  // (const Vec3&)
// the post effects' setters (the 4th argument, a value the forwarders pass on, goes through as it came)
using PostSetFn = int (*)(void*, const char*, float, uint64_t);
using PostSetPtrFn = int (*)(void*, const char*, const void*, uint64_t);  // the Vec4 and the string
using QueryFn = void* (*)(void*, int, intptr_t);
// EF_Query's queries: EFQ_DrawNearFov puts the float its argument points at into the renderer's globals (the
// first-person objects' field of view, which the flush draws with); CryGame's four calls don't read the answer: taken.
// EFQ_Pointer2FrameID (20, an address), EFQ_HDRModeEnabled (32) and EFQ_MultiGPUEnabled (35, through
// IsMultiGPUModeActive, answered too), both from the 3D engine's DisplayInfo every frame, EFQ_TextureStreamingEnabled
// (37) and EFQ_FSAAEnabled (38) only read settings: answered. So is 23, which the Flash player (CFlashPlayer::Render,
// the HUD and the menus) asks first every frame: it keeps what renderer slot 201 answers, always false in this build,
// in a static only EF_Query uses. The others join (the first logged): EFQ_RecurseLevel (19), for one, is the flush's
// own while it runs.
constexpr int kQueryDrawNearFov = 36;
constexpr int kLoggedQueries = 8;
bool QuerySetting(int query) {
    return query == 20 || query == 23 || query == 32 || query == 35 || query == 37 || query == 38;
}
using FlagFn = uint8_t (*)(void*);  // (a bool answer, its byte as it comes)
using PolyCount2Fn = void (*)(void*, int*, int*);
// Draw2dLabel is variadic: its text is formatted here with the C runtime's _vsnprintf the renderer calls (msvcr80's),
// and the method gets it through "%s", whether now or at the join (its own buffer is smaller than ours: the same
// text). Without that _vsnprintf it joins.
using Draw2dLabelFn = void (*)(void*, float, float, float, const float*, bool, const char*, ...);
using VsnprintfFn = int(__cdecl*)(char*, size_t, const char*, va_list);
using Ints3Fn = void (*)(void*, uint64_t, uint64_t, uint64_t);
constexpr int kLoggedEvents = 8;
// The aux geometry's render flags: SetRenderFlags stores the dword its argument points at in the object, which only
// its draws read (to mark their primitives) and GetRenderFlags returns; its Flush doesn't. Beside the flush they are
// taken, in their order with the frame end's taken calls that may draw, and GetRenderFlags answers the last taken
// (else the object's own). Their code is checked at the install: the same field.
using AuxSetFlagsFn = void (*)(void*, const uint32_t*);
using AuxGetFlagsFn = uint32_t* (*)(void*, uint32_t*);  // (the flags' struct, returned through a pointer)
constexpr const char kAuxSetFlagsCode[] = "8B 02 89 81 ?? ?? ?? ?? C3";              // mov eax,[rdx]; mov [rcx+d],eax
constexpr const char kAuxGetFlagsCode[] = "8B 81 ?? ?? ?? ?? 89 02 48 8B C2 C3";     // mov eax,[rcx+d]; mov [rdx],eax
// A render mesh's AddRef only adds 1 to its count, not atomically, and the flush's thread may count the same mesh:
// taken. A shader's and a texture's AddRef (the same code) answer the count before it (`return m_nRefCount++`):
// taken, the answer predicted from the count now and that object's AddRef and Release calls taken before in the
// window, and compared when made. A shader's Release is taken too, its destruction with it (when the count comes to
// 0, made at the join, after the flush): it answers -1 for a shader whose flag (+0x3C bit 29) says it isn't counted,
// else the count after it. (A texture's and a render mesh's Release join.)
using CountedAddRefFn = int (*)(void*);
constexpr const char kMeshAddRefCode[] = "83 41 ?? 01 C3";                           // add dword [rcx+d], 1
constexpr const char kCountedAddRefCode[] = "8B 41 ?? 8D 50 01 89 51 ?? C3";         // mov eax,[rcx+d]; lea edx,[rax+1]
// (bt [rcx+3Ch], 1Dh: -1; then the count at 8 + 8, its destructor through the base at +8 when it comes to 0)
constexpr const char kShaderReleaseCode[] =
    "48 83 EC 28 0F BA 61 3C 1D 73 08 83 C8 FF 48 83 C4 28 C3 48 83 C1 08 83 41 08 FF 8B 41 08 75 0D 48 8B 01 BA 01 "
    "00 00 00 FF 50 18 33 C0 48 83 C4 28 C3";
constexpr size_t kShaderReleaseBytes = 50, kShaderFlags = 0x3C, kShaderCount = 0x10;
constexpr uint32_t kShaderUncounted = 1u << 29;
// IShader's slot 16 reads its first technique's first pass (a byte of it; 2 without one) and nothing else: answered
// with the field getters, by its code
constexpr const char kShaderPassGetterCode[] =
    "83 79 60 00 74 16 48 8B 41 58 48 8B 00 83 78 10 00 74 09 48 8B 40 08 0F BE 40 04 C3 B8 02 00 00 00 C3";
constexpr size_t kShaderPassGetterBytes = 34;
// GetCamera answers a reference to the renderer's camera, which the flush sets for its passes. The camera it leaves
// is the hand-off's but for its last 4 bytes, the struct's padding after its last member (+0x210), in 1-38 % of the
// flushes (vanilla's too): the renderer restores the camera of an object drawn in the nearest depth range, or with a
// camera of its own, from a copy built on the stack (+0xA4080), whose padding nothing writes. So beside the flush
// GetCamera answers a copy made at the hand-off, and at the join the copy's members (all but the padding) are compared
// with the camera's. (The caller gets a reference to the copy, valid until the next hand-off, rather than to the
// renderer's camera.) A diagnostic measures what the flush leaves changed, and its SetCamera calls: their number, the
// last one's time and caller.
using SetCameraFn = void (*)(void*, const void*);
using CameraFn = void* (*)(void*);
constexpr const char kGetCameraCode[] = "48 8D 81 ?? ?? ?? ?? C3";  // lea rax, [rcx+d]
constexpr const char kSetCameraSize[] = "41 B8 18 02 00 00";        // mov r8d, 218h (its memcpy of the camera)
constexpr size_t kCameraBytes = 0x218, kCameraMembers = 0x214, kSetCameraScan = 0x1000;
constexpr size_t kCameraParts = (kCameraBytes + 15) / 16;
constexpr unsigned kSlotSetCamera = 43, kSlotGetCamera = 44;
constexpr unsigned kCameraVersions = 16;  // the SetCamera calls taken per window (then it joins)
// GetModelViewMatrix and GetProjectionMatrix copy the top of the view or projection matrix stack (the field's
// displacement in their code: +0x2CD50, +0x2CD58), the matrices the camera copied at the hand-off has: answered from
// it, but within a taken 2D mode (other matrices), where they join.
using MatrixFn = void (*)(void*, float*);
constexpr const char kMatrixGetCode[] =
    "40 53 48 83 EC 20 48 8B 89 ?? CD 02 00 48 8B DA E8 ?? ?? ?? ?? 48 8B CB 41 B8 40 00 00 00 48 8B D0 48 83 C4 20 "
    "5B E9 ?? ?? ?? ??";
constexpr size_t kMatrixGetBytes = 43, kMatrixGetStackAt = 9;  // (mov rcx, [rcx+d]: d's low byte)
constexpr int kLoggedCameras = 8;
// After a taken SetCamera too (the double-buffered frame's step 3): the renderer's own SetCamera runs on the camera
// copy's block, whose stack tops are the copies. Besides the camera it copies in and the stacks, it reads the
// renderer's flags (+0xAF4: bit 17 picks the projection's form) and four words at +0x2A038, copied into the block at
// the hand-off, and it writes only the block. At its end it calls the camera-info method (+0x9DDF0 in this build),
// which derives the renderer's other camera matrices from the stacks and marks the shader constants through the
// global renderer pointer: detoured, it returns at once for the block. It is found among SetCamera's direct calls by
// its code. At the join each replayed SetCamera's matrices are compared with the block's.
using CameraInfoFn = void (*)(void*);
constexpr const char kCameraInfoCode[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 60 48 8B 01 48 8D 51 10 48 8B F9 FF 90 20 02 00 00 48 "
    "8D 4F 50 48 8D 57 10 41 B8 40 00";
constexpr size_t kCameraInfoBytes = 48, kCamFlags = 0xAF4, kCamScreen = 0x2A038, kCamScreenBytes = 16;
constexpr int kLoggedStandIns = 8;
// UnProjectFromScreen reads the matrices and the viewport through the renderer's own vtable (slots 68, 69, 20), and
// computes from them: answered on the camera copy's block too, given a vtable of the renderer's functions (not their
// gate's thunks). Its code is checked up to those three calls.
using UnProjectFn = int (*)(void*, float, float, float, float*, float*, float*);
constexpr const char kUnProjectCode[] =
    "48 8B C4 53 48 81 EC 80 01 00 00 0F 29 70 E8 0F 29 78 D8 44 0F 29 40 C8 48 8D 94 24 80 00 00 00 48 8B D9 44 0F "
    "29 48 B8 44 0F 28 C3 0F 28 F2 48 8B 01 0F 28 F9 FF 90 20 02 00 00 4C 8B 1B 48 8D 94 24 C0 00 00 00 48 8B CB 41 "
    "FF 93 28 02 00 00 4C 8B 1B 48 8D 44 24 3C 4C 8D 4C 24 38 4C 8D 44 24 34 48 8D 54 24 30 48 8B CB 48 89 44 24 20 "
    "41 FF 93 A0 00 00 00 4C 8D 84 24 80 00 00 00 48 8D";
constexpr size_t kUnProjectBytes = 128;
// The ocean (CREWaterOcean): slot 24 answers the water simulation's displacement grid (the start of a vector in the
// simulation's object: [[this+0x50]]+0x68, its end +0x70; 64 x 64 points of 16 bytes), which the 3D engine reads
// after the call, and slot 23 one point of it. The flush writes the grid: the ocean's draw (slot 15) updates the
// simulation, once a frame, and the water volume's post effect may too. Beside the flush they wait for the flush's
// first ocean draw (or its end), and are answered then; and the grid as it was at the window's first answer is
// compared with the grid at the join. Their code is checked at the install.
using OceanPointFn = float* (*)(void*, float*, float, float);  // (a Vec3, returned through a pointer)
using DrawFn = uint64_t (*)(void*, uint64_t, uint64_t);       // CRendElement::mfDraw(shader, pass)
constexpr const char kOceanGridCode[] = "48 8B 49 50 48 85 C9 74 05 E9 ?? ?? ?? ??";
constexpr const char kOceanVectorCode[] = "48 8B 11 48 8B 42 70 48 39 42 68 74 05 48 8B 42 68 C3";
constexpr size_t kOceanSim = 0x50, kOceanBegin = 0x68, kOceanEnd = 0x70, kOceanMaxBytes = size_t(1) << 20;
constexpr unsigned kSlotOceanDraw = 15;
// The grid final at the hand-off (for the double-buffered frame, render-thread.md §12: the next update reads it before
// the traversal). The ocean's draw updates the grid first (FrameUpdate, every draw), when the element has its geometry
// (its counts at +0x30, +0x34, its buffers at +0x40, +0x48). The traversal adds the element to the lists through
// EF_AddEf, which an observer notes (it still joins beside the flush). Then the flush's thread runs FrameUpdate at the
// flush's start: its inputs, the 3D engine's ocean parameters and the frame's time, are the hand-off's in the flush,
// and the simulation steps once a frame (by the frame id), so the draws after it find it done. The flush samples the
// grid before its first ocean draw as well (the water level at the camera, which the reflections' camera takes): the
// grid it had is put back for that, the update kept in a copy that answers beside the flush, and put in the grid at the
// flush's first ocean draw, where vanilla's update is. A flush the ocean isn't added to leaves the grid as it is: final
// at the hand-off. (The grid as at the window's first answer is still compared with the grid at the join.)
using OceanUpdateFn = void (*)(void*);
using AddEfFn = void (*)(void*, void*, void*, void*, int, int);
constexpr const char kOceanDrawCode[] =
    "48 8B C4 53 41 54 48 81 EC A8 00 00 00 83 79 30 00 4C 8B E2 48 8B D9 0F 84 ?? ?? ?? ?? 83 79 34 00 0F 84 ?? ?? "
    "?? ?? 48 83 79 40 00 0F 84 ?? ?? ?? ?? 48 83 79 48 00 0F 84 ?? ?? ?? ?? 48 89 70 E8 48 89 78 E0 48 8B 3D ?? ?? "
    "?? ?? 4C 89 68 D8 E8 ?? ?? ?? ??";
constexpr const char kOceanUpdateCode[] =
    "48 89 5C 24 10 57 48 81 EC 90 00 00 00 8B 05 ?? ?? ?? ?? 0F 29 B4 24 80 00 00 00 0F 57 F6 A8 01 0F 29 7C 24 70 "
    "48 8B F9";
constexpr size_t kOceanDrawBytes = 85, kOceanUpdateCallAt = 80, kOceanUpdateBytes = 40;
constexpr size_t kOceanVerts = 0x30, kOceanIndices = 0x34, kOceanVB = 0x40, kOceanIB = 0x48;
constexpr unsigned kSlotAddEf = 114;
// The post effects' float parameters read (CREPostProcess slot 25, from the 3D engine's GetPostEffectParam): the
// renderer looks the name up in its manager (a CRC of the name upper-cased, the key of a map) through a cache of one
// entry, which the flush's own lookups write too. Beside the flush the queue looks the name up itself, in the same
// map. A float parameter that doesn't blend (CParamFloat, its blending off or the renderer's switch for it off: its
// getter then only reads the value, and its setter only writes it) answers its value, or the last one taken in the
// window for that name; a missing one, nothing (as the renderer). A blending one, whose getter moves the value toward
// its target each time the flush's post effects call it, and any other kind, join. The code is checked at the
// install (the getter, the lookup, CParamFloat's getter and setter); the answers at the join.
using PostGetFn = void (*)(void*, const char*, float*);
constexpr const char kPostGetCode[] =
    "40 53 48 83 EC 20 48 8D 0D ?? ?? ?? ?? 49 8B D8 E8 ?? ?? ?? ?? 48 85 C0 74 0D 48 8B 10 48 8B C8 FF 52 38 F3 0F "
    "11 03 48 83 C4 20 5B C3";
constexpr const char kPostLookupCode[] =
    "48 85 D2 4C 8B C2 4C 8B D1 75 05 45 33 C9 EB 46 41 83 C9 FF 80 3A 00 74 3A 66 66 90 66 66 66 90 41 0F B6 10 49 "
    "83 C0 01 8D 42 9F 3C 19 77 03 80 C2 E0 41 0F B6 C9 0F B6 C2 48 33 C8 41 8B C1 45 8B 8C 8A 20 01 00 00 C1 E8 08 "
    "44 33 C8 41 80 38 00 75 CD 41 F7 D1 44 3B 0D ?? ?? ?? ?? 75 0C 48 8B 05 ?? ?? ?? ?? 48 85 C0 75 54 49 8D 82 C0 "
    "00 00 00 48 8B D0 48 8B 48 08 48 85 C9 74 27 66 90 44 39 49 20 72 09 48 8B D1 48 8B 49 10 EB 04 48 8B 49 18 48 "
    "85 C9 75 E8 48 3B D0 74 20 44 3B 4A 20 48 0F 42 D0 48 3B D0 74 13 48 8B 42 28 44 89 0D ?? ?? ?? ?? 48 89 05 ?? "
    "?? ?? ?? C3 33 C0 C2 00 00";
constexpr const char kParamFloatGetCode[] =
    "40 53 48 83 EC 20 80 79 0C 00 48 8B D9 0F 84 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 74 7F 0F B6 41 18 84 C0 74 19 F3 "
    "0F 10 49 14 0F B6 C0 C6 41 18 00 F3 0F 2A C0 F3 0F 5E C8 F3 0F 11 49 14 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 C8 "
    "01 00 00 33 D2 4C 8B 00 48 8B C8 41 FF 50 30 0F 57 D2 F3 0F 59 05 ?? ?? ?? ?? 0F 2F C2 72 10 F3 0F 10 15 ?? ?? "
    "?? ?? 0F 2F C2 73 03 0F 28 D0 F3 0F 10 4B 14 F3 0F 5C 4B 08 F3 0F 59 CA F3 0F 58 4B 08 F3 0F 11 4B 08 F3 0F 10 "
    "C1 48 83 C4 20 5B C3 F3 0F 10 41 08 48 83 C4 20 5B C3";
constexpr const char kParamFloatSetCode[] =
    "80 79 0C 00 74 48 83 3D ?? ?? ?? ?? 00 74 3F 45 84 C0 75 31 0F B6 51 18 84 D2 75 06 8B 41 10 89 41 14 84 D2 F3 "
    "0F 58 49 14 F3 0F 11 49 14 75 0E 0F 57 C0 0F 2E 41 10 74 05 C6 41 18 02 C3 80 C2 01 88 51 18 C3 F3 0F 11 49 14 "
    "C6 41 18 00 F3 0F 11 49 08 C3";
constexpr size_t kPostGetBytes = 45, kPostLookupBytes = 194, kParamGetBytes = 166, kParamSetBytes = 84;
constexpr size_t kPostMgrAt = 9, kPostLookupAt = 17;  // (in the getter: lea rcx, [rip+d] at 6; call at 16)
constexpr size_t kParamSwitchAt = 21;                  // (in CParamFloat's getter: cmp dword [rip+d], 0 at 19)
constexpr size_t kPostCrcTable = 0x120, kPostMap = 0xC0, kPostNodeLeft = 0x10, kPostNodeRight = 0x18,
                 kPostNodeKey = 0x20, kPostNodeValue = 0x28, kParamBlends = 0x0C, kParamValue = 0x08;
constexpr unsigned kSlotParamGet = 7, kSlotParamSet = 2;
constexpr uint32_t kPostUnwritten = 0x7FC0DEAD;  // (a NaN the check's call starts with: still there, nothing found)
// CParamBool's and CParamInt's getters only read their value (+0x08: a byte, an int) as a float, and their setters
// only write it: the bool's to whether the float isn't a constant of the renderer's (ucomiss: an unordered pair counts
// as equal, false), the int's to the float truncated (cvttss2si)
constexpr const char kParamBoolGetCode[] = "0F B6 41 08 F3 0F 2A C0 C3";
constexpr const char kParamBoolSetCode[] = "0F 2E 0D ?? ?? ?? ?? 74 05 C6 41 08 01 C3 C6 41 08 00 C3";
constexpr const char kParamIntGetCode[] = "F3 0F 2A 41 08 C3";
constexpr const char kParamIntSetCode[] = "F3 0F 2C C1 89 41 08 C3";
constexpr size_t kParamBoolConstAt = 3;  // (the setter's ucomiss xmm1, [rip+d]: d at 3, the next instruction at 7)
// The Vec4 ones (slot 26) the same way: CParamVec4, its blending flag at +0x18, the same switch (its getter checked
// to read it), its four floats at the value's place; answered from them, or from the window's last Vec4 setter taken.
constexpr const char kPostGetVec4Code[] =
    "40 53 48 83 EC 30 48 8D 0D ?? ?? ?? ?? 49 8B D8 E8 ?? ?? ?? ?? 48 85 C0 74 25 4C 8B 08 48 8D 54 24 20 48 8B C8 "
    "41 FF 51 40 8B 08 89 0B 8B 48 04 89 4B 04 8B 48 08 89 4B 08 8B 40 0C 89 43 0C 48 83 C4 30 5B C3";
constexpr const char kParamVec4GetCode[] =
    "48 89 5C 24 08 57 48 83 EC 20 80 79 18 00 48 8B FA 48 8B D9 0F 84 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? "
    "?? ?? 0F B6 41 3C 84 C0 74 48 F3 0F 10 15 ?? ?? ?? ?? 0F B6 C0 F3 0F 2A C0 F3 0F 5E D0 0F 28 CA 0F 28 C2 F3 0F "
    "59 49 2C F3 0F 59 41 30 F3 0F 11 49 2C 0F 28 CA F3 0F 11 41 30 F3 0F 59 49 34 F3 0F 59 51 38 F3 0F 11 49 34 F3 "
    "0F 11 51 38 C6 41 3C 00 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 C8 01 00 00 33 D2 4C 8B 00 48 8B C8 41 FF 50 30 0F "
    "57 E4 F3 0F 59 05 ?? ?? ?? ?? 0F 2F C4 72 16 0F 2F 05 ?? ?? ?? ?? 73 05 0F 28 E0 EB 08 F3 0F 10 25 ?? ?? ?? ?? "
    "F3 0F 10 43 2C F3 0F 10 4B 30 F3 0F 10 53 34 F3 0F 5C 43 08 F3 0F 5C 4B 0C F3 0F 5C 53 10 F3 0F 59 C4 F3 0F 59 "
    "CC F3 0F 59 D4 F3 0F 10 5B 38 F3 0F 58 43 08 F3 0F 58 4B 0C F3 0F 5C 5B 14 F3 0F 58 53 10 F3 0F 11 43 08 F3 0F "
    "59 DC F3 0F 58 5B 14 F3 0F 11 4B 0C F3 0F 11 53 10 F3 0F 11 5B 14 8B 43 08 89 07 8B 43 0C 89 47 04 8B 43 10 89 "
    "47 08 8B 43 14 48 8B 5C 24 30 89 47 0C 48 8B C7 48 83 C4 20 5F C3";
constexpr const char kParamVec4SetCode[] =
    "80 79 18 00 0F 84 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 45 84 C0 0F 85 ?? ?? ?? ?? 44 38 41 3C 75 "
    "18 8B 41 1C 89 41 2C 8B 41 20 89 41 30 8B 41 24 89 41 34 8B 41 28 89 41 38 F3 0F 10 41 2C F3 0F 58 02 F3 0F 11 "
    "41 2C F3 0F 10 4A 04 F3 0F 58 49 30 F3 0F 11 49 30 F3 0F 10 42 08 F3 0F 58 41 34 F3 0F 11 41 34 F3 0F 10 4A 0C "
    "F3 0F 58 49 38 F3 0F 11 49 38 0F B6 41 3C 84 C0 75 22 F3 0F 10 41 28 F3 0F 58 41 24 F3 0F 58 41 20 F3 0F 58 41 "
    "1C 0F 2E 05 ?? ?? ?? ?? 74 05 C6 41 3C 02 C3 04 01 88 41 3C C3 8B 02 89 41 2C 8B 42 04 89 41 30 8B 42 08 89 41 "
    "34 8B 42 0C 89 41 38 8B 02 89 41 08 8B 42 04 89 41 0C 8B 42 08 89 41 10 8B 42 0C C6 41 3C 00 89 41 14 C3 8B 02 "
    "89 41 08 8B 42 04 89 41 0C 8B 42 08 89 41 10 8B 42 0C 89 41 14 C3";
using PostGetVec4Fn = void (*)(void*, const char*, float*);
constexpr size_t kPostGetVec4Bytes = 69, kParamVec4GetBytes = 318, kParamVec4SetBytes = 244;
constexpr size_t kParamVec4SwitchAt = 28, kParamVec4Blends = 0x18;  // (cmp dword [rip+d], 0 at 26)
constexpr unsigned kSlotParamGetVec4 = 8, kSlotParamSetVec4 = 3;
// DownLoadToVideoMemory with the Id of a texture that exists writes its data into the texture's first level (its
// UpdateTextureRegion: the width and height of the data, in its format) and answers the Id; with none, it makes a
// texture and answers its new Id. Beside the flush (the terrain's sectors streamed into its texture pool, mostly) an
// update is taken, its data copied into a room of the window's, and answers its Id (compared with the made call's
// answer at the join); a new texture, a format whose size isn't known here, or a full room joins.
using DownLoadFn = unsigned (*)(void*, const uint8_t*, int, int, int, int, int, bool, int, int, char*, int);
constexpr size_t kUploadBytes = size_t(16) << 20, kUploadGuard = size_t(64) << 10;
constexpr int kLoggedUploads = 4;
// SetViewport stores the viewport (x, y, width, height at the field ProjectToScreen and GetViewport read), marks it
// changed for the flush, which uses it, and sets a LOD bias from its size: taken. The camera copied at the hand-off
// takes the new viewport at once, so that GetViewport and ProjectToScreen answer it as they would after it; the
// hand-off's own is kept, for the camera the flush leaves.
using SetViewportFn = void (*)(void*, int, int, int, int);
// The shader resources' references (IRenderShaderResources, the interface at +0x38: GetDiffuseColor,
// GetSpecularColor, GetEmissiveColor, ColorF&, and GetSpecularShininess, GetGlow, GetAlphaRef, GetOpacity, float&,
// the colors' alphas; their code only works out the address, in the resources' light material or a default). The
// caller reads or writes through the reference (the 3D engine's material parameters: v = GetGlow(), or GetGlow() =
// v), and the flush reads them. Beside the flush they answer bytes of the queue's own, as the real ones were, or as
// the window's earlier answers for the same bytes left them; at the join, at its place, bytes the caller changed are
// written to the real ones, and those it didn't are checked to be as they were. (A reference kept past the next
// hand-off would point at the queue's bytes.)
using ResRefFn = void* (*)(void*);
constexpr const char* const kResCodes[] = {
    "48 8B 81 ?? ?? ?? ?? 48 83 C0 ?? C3",                                             // mov rax,[rcx+d]; add rax,i
    "48 8D 41 ?? C3",                                                                  // lea rax,[rcx+d]
    "48 8B 81 ?? ?? ?? ?? 83 78 FC 00 75 07 48 8D 05 ?? ?? ?? ?? C2 00 00",            // the material, or a default
    "48 8B 81 ?? ?? ?? ?? 83 78 FC 00 75 08 48 8D 05 ?? ?? ?? ?? C3 48 83 C0 ?? C3"};  // (and add rax,i)
struct ResSlot {
    unsigned slot;
    uint32_t bytes;
};
constexpr ResSlot kResSlots[] = {{6, 16}, {7, 16}, {8, 16}, {9, 4}, {16, 4}, {17, 4}, {18, 4}};
constexpr unsigned kResAt = 0x38;
constexpr size_t kMaxResRefs = 1024;
struct ResRef {
    alignas(16) uint8_t value[16];  // the queue's own bytes: what the caller left in them
    uint8_t before[16];             // as answered
    uint8_t* real;
    uint32_t bytes;
};
// GetIndices (a render mesh's indices and their count, through a pointer) only reads two fields: answered.
using IndicesFn = uint16_t* (*)(void*, int*);
constexpr const char kIndicesCode[] =
    "48 85 D2 74 08 8B 81 F0 00 00 00 89 02 83 B9 F0 00 00 00 00 74 08 48 8B 81 E8 00 00 00 C3 33 C0 C3";
// GetStridedPosPtr (a render mesh's positions): reading them through its system stream ([this or its vertex
// container (+0x80)]+0xC0, its first word non-null) only works out a pointer and a stride: answered. A write (which
// marks the video buffer to update), or the path through the video buffer, which the flush may make, joins.
using PosPtrFn = uint8_t* (*)(void*, int*, int, bool);
constexpr const char kPosPtrCode[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B 81 80 00 00 00 48 8B D9 41 8B F0 48 85 C0 48 8B FA 4C 8B D1 "
    "48 0F 45 D8 48 8B 83 C0 00 00 00 48 85 C0 74 50 48 83 38 00 74 4A 45 84 C9 48 8B";
constexpr size_t kPosPtrBytes = 64, kMeshContainer = 0x80, kMeshSysStream = 0xC0;
// The getters whose code only returns a field of their object (mov rax/eax/al, [rcx+d] or movzx eax, byte [rcx+d],
// then ret), found at the install among the slots of CRenderMesh, CShader and the shader resources' interface at
// +0x38, and CTexture's GetFlags (slot 9): answered, and checked at the join (the bytes the getter sets: a byte's
// leaves the rest of rax as it was). Left out, what the flush may set as it draws: a mesh's video buffer
// (GetVideoVertBuffer, slot 29) and the frame of its update (GetUpdateFrame, slot 41).
using GetterFn = uint64_t (*)(void*);
constexpr unsigned kGetterSlots = 80;  // (CRenderMesh's vtable has 72 in this build)
enum GetterClass : uint8_t { kGetterMesh, kGetterShader, kGetterTexture, kGetterResources, kGetterClasses };
const char* const kGetterNames[kGetterClasses] = {"IRenderMesh", "IShader", "ITexture", "IRenderShaderResources"};
constexpr unsigned kMeshSlotVideoBuffer = 29, kMeshSlotUpdateFrame = 41, kTextureSlotFlags = 9;
// Methods that only read what the main thread alone writes, or do nothing: they pass the gate beside the flush, made
// at once, with no record (each checked by its code). The 3D engine's lights' EF_PrecacheResource (slot 83) only
// calls the light's texture's slot 14 when the textures stream, an empty method in this build (checked too); slot 201
// answers false; a render mesh's GetVertexContainer (+0x80, or itself) and GetBBox (+0x130..+0x144: SetBBox and
// UpdateBBoxFromMesh write them, on the main thread); a shader's GetREs (a technique's elements, by its index); the
// shader resources' GetTexture (by its index).
struct PureSlot {
    uint8_t cls;  // a Class, or kResClass: the shader resources' interface at +0x38
    unsigned slot;
    const char* code;
    const char* name;
};
constexpr uint8_t kResClass = 0xFF;
constexpr PureSlot kPureSlots[] = {
    {kRenderer, 83,
     "48 83 EC 28 83 3D ?? ?? ?? ?? 00 74 1F 48 83 BA 98 01 00 00 00 74 15 48 8B 8A 98 01 00 00 44 8B 44 24 50 0F 28 "
     "CA 48 8B 01 FF 50 70 B0 01 48 83 C4 28 C3",
     "EF_PrecacheResource (a light's)"},
    {kRenderer, 201, "32 C0 C3", "CD3D9Renderer slot 201 (false)"},
    {kRenderMesh, 17, "48 8B 81 80 00 00 00 48 85 C0 48 0F 45 C8 48 8B C1 C3", "IRenderMesh::GetVertexContainer"},
    {kRenderMesh, 37,
     "8B 81 30 01 00 00 89 02 8B 81 34 01 00 00 89 42 04 8B 81 38 01 00 00 89 42 08 8B 81 3C 01 00 00 41 89 00 8B 81 "
     "40 01 00 00 41 89 40 04 8B 81 44 01 00 00 41 89 40 08 C3",
     "IRenderMesh::GetBBox"},
    {kShader, 10, "33 C0 85 D2 0F 48 D0 3B 51 60 7D 0C 48 8B 41 58 48 8B 04 D0 48 83 C0 28 C2 00 00", "IShader::GetREs"},
    {kResClass, 19, "48 63 C2 48 8B 44 C1 10 C3", "IRenderShaderResources::GetTexture"},
};
// (not GetParameters, slot 20, which only answers the address of the resources' parameters: its callers may set a
// parameter there, or add one, moving the array the flush reads)
constexpr unsigned kTextureSlotPrecache = 14;  // (what EF_PrecacheResource calls: an empty method, ret 0)
constexpr const char kEmptyMethodCode[] = "C2 00 00";
// SetSkyLightRenderParams: mov [rcx + the field], rdx; ret
constexpr const char kSetSkyLightCode[] = "48 89 91 ?? ?? ?? ?? C3";
// SetTerrainAONodes: mov dword [rcx + the count], 0; add rcx, the array; jmp its AppendList, whose new size is the
// count times 72 (lea rsi, [rax+rax*8]; shl rsi, 3: the nodes' size, which the taken call copies by)
constexpr const char kSetTerrainAOCode[] = "C7 81 ?? ?? ?? ?? 00 00 00 00 48 81 C1 ?? ?? ?? ?? E9 ?? ?? ?? ??";
constexpr const char kAONodesAppendSize[] = "48 8D 34 C0 48 C1 E6 03";
constexpr size_t kAONodeBytes = 72, kAONodesJmpAt = 17, kAONodesAppendScan = 128;
// CrySystem's CFlashPlayer: Render (slot 9) and Release (slot 0, which logs if asked and deletes the player through its
// destructor's slot), their code checked
constexpr unsigned kSlotFlashRender = 9, kSlotFlashRelease = 0;
constexpr const char kFlashRenderCode[] = "40 53 56 57 48 81 EC 80 00 00 00 83 3D ?? ?? ?? ?? 00 48 8B F1 74 14 48";
constexpr const char kFlashReleaseCode[] =
    "40 53 48 83 EC 20 80 79 08 00 48 8B D9 74 18 4C 8B 89 B8 00 00 00 33 D2 4C 8D 05 ?? ?? ?? ?? 8D 4A 07 E8 ?? ?? ?? "
    "?? 48 8B 03 BA 01 00 00 00 48 8B CB 48 83 C4 20 5B 48 FF A0 F0 00 00 00";
// SetDefaultMaterials(the default material, the terrain's) adds a reference to the new ones, releases the old and
// keeps the new, which the flush draws with: taken. The queue adds a reference of its own meanwhile (the renderer's
// would hold them from the call on), released at the join once the call is made.
using DefaultMaterialsFn = void (*)(void*, void*, void*);
constexpr unsigned kSlotMaterialAddRef = 1, kSlotMaterialRelease = 2;  // (IMaterial's, as the renderer calls them)

struct Call {
    Kind kind;
    void* renderer;
    uint64_t a[15];   // the arguments (floats as their bits)
    uint32_t out[4];  // answered: the answer's bits
};

// The parts whose verify mode calls vanilla's code from inside the flush (to compare): with any of them in verify, the
// flush may take the other modules' locks, and nothing passes (the main thread joins at its first renderer call)
const wchar_t* const kVerifyKeys[][2] = {
    {L"Mode", L"OPTIPERF_SHADOW_PREPASS"},
    {L"Reflections", L"OPTIPERF_SHADOW_PREPASS_REFLECTIONS"},
    {L"FarTrees", L"OPTIPERF_SHADOW_PREPASS_FARTREES"},
    {L"DynTextures", L"OPTIPERF_SHADOW_PREPASS_DYNTEXTURES"},
    {L"Environment", L"OPTIPERF_SHADOW_PREPASS_ENVIRONMENT"},
    {L"Particles", L"OPTIPERF_SHADOW_PREPASS_PARTICLES"},
    {L"Skinning", L"OPTIPERF_SHADOW_PREPASS_SKINNING"},
    {L"Materials", L"OPTIPERF_SHADOW_PREPASS_MATERIALS"},
    {L"Cvars", L"OPTIPERF_SHADOW_PREPASS_CVARS"},
    {L"System", L"OPTIPERF_SHADOW_PREPASS_SYSTEM"},
};

// The text's dynamic vertex buffer: CryFont writes its vertices where GetDynVBPtr points, then calls DrawDynVB with the
// offset it was given. The renderer's answer is the same scratch each time, at offset 0, locked for the most vertices
// the string can take (its DrawDynVB copies the drawn ones out). Beside the flush the queue points CryFont into a
// room of its own and gives it a token for the offset; at the join the real GetDynVBPtr is made at its place, and the
// vertices are copied into the scratch right before their DrawDynVB. Until a pool's vertex size is known, the room is
// filled with a pattern first and the bytes CryFont wrote over it are measured at the draw (and copied); the size is
// known when kStrideSamples draws have written the same bytes per drawn vertex.
constexpr int kPools = 8, kStrideSamples = 16;
constexpr int kTokenBase = 0x40000000;           // offsets handed out beside the flush: kTokenBase + the lock's index
constexpr size_t kTextBytes = size_t(4) << 20;   // the queue's own vertex buffer, per window
constexpr uint32_t kRoomPerVertex = 64;          // while the pool's vertex size isn't known
constexpr uint8_t kUnwritten = 0xCD;             // the room's fill then
struct Pool {
    uint32_t stride = 0, samples = 0;
    bool contradicted = false;  // (a draw wrote other bytes per vertex: its rooms stay measured)
};
struct Lock {          // a lock taken beside the flush
    int pool, verts;
    size_t at, room;   // its room in the queue's buffer
    char* real;        // made at the join: the renderer's pointer and offset (null: the lock failed)
    int realOffs;
    bool measure;      // its pool's vertex size wasn't known: the room was filled
    bool made, copied;
};
constexpr int kLoggedSamples = 8;  // the first measured draws, logged
constexpr size_t kMaxString = 1024;  // the post effects' names and strings copied (longer: the call joins)
constexpr uint64_t kNoString = ~uint64_t(0);
// the room per window for the strings and the terrain's AO nodes: reserved once, never moved (more: the call joins)
constexpr size_t kStringBytes = size_t(256) << 10, kAONodesBytes = size_t(256) << 10;

// ProjectToScreen reads the viewport and the tops of the view and projection matrix stacks (their first word), and
// nothing else of the renderer's (this build: CryRenderD3D10.dll+0xE1E90). They are copied at the hand-off; beside
// the flush it projects through a block laid out as the renderer with only these set, whose stacks are the copies.
// Set2DMode and the font's rendering state push their own matrices (made at the join, when taken): until they are
// undone, it joins.
constexpr size_t kViewport = 0x2C2AC, kViewportBytes = 16, kViewStack = 0x2CD50, kProjStack = 0x2CD58;
constexpr size_t kMatrixBytes = 64;
alignas(16) float g_matrices[2][16];  // the copies: view, projection (aligned as the renderer's)
// The hand-off's camera, GetCamera's answer (the hand-off's camera, or a taken SetCamera's), and this window's taken
// SetCamera calls' cameras (outside the state: their alignment would pad it)
alignas(16) uint8_t g_cameraHandOff[kCameraBytes];
alignas(16) uint8_t g_cameraAnswer[kCameraBytes];
alignas(16) uint8_t g_cameraVersion[kCameraVersions][kCameraBytes];
// the matrices the block's SetCamera made for each taken one (view, projection), and the hand-off's: what the matrix
// getters answered, for the join's checks
alignas(16) float g_matrixVersion[kCameraVersions][2][16];
alignas(16) float g_matrixHandOff[2][16];
// FlushThreadPresent: the camera, the view and the projection the flush left, copied by the worker right after it
alignas(16) uint8_t g_cameraLeft[kCameraBytes];
alignas(16) float g_matrixLeft[2][16];
struct Camera {
    std::vector<uint8_t> block;          // laid out as the renderer, up to the projection stack
    float* const view = g_matrices[0];
    float* const proj = g_matrices[1];
    float* viewStack[8];                 // the stacks: every word points at the copy
    float* projStack[8];
    char* renderer = nullptr;
    bool valid = false;
    bool matrices = false;  // its matrices the renderer's would be (else ProjectToScreen and the matrix getters join)
    unsigned version = 0;   // whose: 0 the hand-off's, v + 1 taken SetCamera v's (made on the block)
};
enum CameraPart { kCamViewport, kCamView, kCamProj, kCamParts };

struct State {
    bool installed = false;
    bool verifyCalls = false;  // a part in verify mode calls vanilla from the flush: nothing passes
    Pool pools[kPools];
    std::vector<uint8_t> text;  // the queue's vertex buffer
    size_t textUsed = 0;
    std::vector<Lock> locks;    // this window's
    uint64_t textJoins = 0, locksTaken = 0, locksMeasured = 0, locksFailed = 0, textMisses = 0;
    int loggedSamples = 0;
    bool cameraLayout = false;  // ProjectToScreen's code reads the fields above
    Camera camera;
    int depth2d = 0;            // the taken calls' pushed 2D matrices, not yet undone
    uint64_t projectJoins = 0, cameraCompared = 0, cameraSame = 0, cameraDiffer[kCamParts] = {};
    std::vector<char> strings;  // this window's: the post effects' names and string arguments, copied
    int postReturn[3] = {};     // the setters' last answer when made (what a taken one answers)
    uint64_t postJoins = 0;
    int loggedCallers = 0;
    void* nearFovReturn = nullptr;  // EFQ_DrawNearFov's last answer when made
    uint64_t queryJoins = 0;
    int loggedQueries = 0;
    bool windowLogged = false;
    bool barrierTried = false;
    int barrier = -1;         // the gate's thunk of the 3D engine's RenderWorld (the barrier), once installed
    uint64_t barrierPassed = 0;  // the traversals the recorder took beside the flush (FlushThreadRecord=beside)
    int beginFrameIndex = -1;  // BeginFrame's thunk: redirected once the barrier is in place
    int pendingFrames = 0;     // BeginFrame calls taken in this window (GetFrameID's answers add them)
    uint64_t frameIdJoins = 0;  // GetFrameID(true) beside the flush: joined
    volatile bool scenesDone = false;  // this window's flush is past its scene passes
    uint64_t scenesWaits = 0, scenesWaitCycles = 0;
    bool viewportLayout = false;  // GetViewport's code reads the viewport the camera copies
    uint64_t viewportJoins = 0;
    volatile unsigned replayAt = 0, replayCount = 0;  // at a join: the call being made (1-based; 0: none)
    volatile Kind replayKind = kDeleteRenderMesh;
    VsnprintfFn vsnprintf80 = nullptr;
    uint64_t labelJoins = 0;
    struct AuxFlags {  // this window's last taken SetRenderFlags
        void* aux = nullptr;
        uint32_t flags = 0;
        bool set = false;
    } auxFlags;
    unsigned countAt[2] = {};  // the shader's and the texture's count, in their objects (their AddRef's code)
    // this window's counted AddRef and Release calls taken, per object: those still in the main thread's list, and
    // (FlushThreadPresent) those sealed for the worker that it hasn't made yet. The lock guards both while the worker
    // makes the sealed ones and the main thread goes on predicting answers from the objects' counts.
    struct Ref {
        void* obj;
        int window, sealed;
    };
    std::vector<Ref> refs;
    SRWLOCK refsLock = SRWLOCK_INIT;
    uint64_t refsDiffer = 0, releasesToZero = 0;
    std::vector<void*> blockVtable;  // the renderer's functions, for UnProjectFromScreen on the camera copy's block
    uint64_t unprojectJoins = 0;
    // the ocean
    void* oceanDraw = nullptr;     // its mfDraw
    volatile bool oceanDrawn = false;  // this window's flush has drawn the ocean once
    uint64_t handOffAt = 0;        // this window's hand-off (rdtsc), for the first draw's delay
    uint64_t oceanDrawWindows = 0, oceanDrawCycles = 0, oceanWaits = 0, oceanWaitCycles = 0, oceanEnded = 0;
    void* oceanVt = nullptr;      // CREWaterOcean's vtable
    void* oceanUpdate = nullptr;  // its FrameUpdate (null: the grid waits for the flush's ocean draw, as before)
    void* addEf = nullptr;        // EF_AddEf, observed
    void* volatile oceanAdded = nullptr;  // the ocean's element added to the lists since the last hand-off
    void* oceanPre = nullptr;             // this window's: updated at the flush's start
    bool oceanStayed = false;             // this window's grid was to stay (none added)
    bool oceanDrawSeen = false;           // this window's flush has drawn the ocean
    uint64_t oceanPreUpdates = 0, oceanPreCycles = 0, oceanStays = 0, oceanLate = 0, oceanUndrawn = 0;
    std::vector<uint8_t> oceanOld, oceanNew;  // this window's grid before its update (put back for the flush), after it
    const uint8_t* oceanAnswer = nullptr;     // the grid answered beside the flush: oceanNew (null: the simulation's)
    char* oceanAnswerOf = nullptr;            // its ocean
    uint8_t* oceanPublishTo = nullptr;        // the simulation's grid, where the flush's first ocean draw puts the update
    volatile bool oceanPublished = false;     // the simulation's grid is this window's last (a point of it may be read)
    uint64_t oceanPublishes = 0, oceanUnrestored = 0, oceanPublishFailed = 0, oceanPointWaits = 0, oceanNoGrid = 0;
    // the flush's scene traversals (the 3D engine's RenderWorld, left to it by the reflection part or fallen back to):
    // the main thread's next update waits for them at BeginFrame
    volatile bool scenesFree = false;     // this window's flush runs none from here on
    volatile bool pastBeginFrame = false;  // the main thread is in its next update
    uint64_t sceneWaits = 0, sceneWaitCycles = 0, sceneRuns = 0, sceneRunsUnforeseen = 0, sceneRunsInUpdate = 0;
    std::vector<uint8_t> oceanCopy;  // the grid at the window's first answer
    char* oceanOf = nullptr;         // its ocean (null: not copied this window)
    const uint8_t* oceanGrid = nullptr;
    uint64_t oceanCompared = 0, oceanChanged = 0;
    // the post effects' parameters read
    char* postMgr = nullptr;              // the renderer's manager (null: GetPostEffectParam joins)
    const void* paramFloatGet = nullptr;  // CParamFloat's getter
    const void* paramVec4Get = nullptr;   // CParamVec4's
    const void* paramBoolGet = nullptr;   // CParamBool's and CParamInt's (null: they join)
    const void* paramIntGet = nullptr;
    const float* paramBoolFalse = nullptr;  // the constant CParamBool's setter compares with (equal: false)
    const char* postLookup = nullptr;     // the manager's lookup (the getters call it)
    const int* paramBlending = nullptr;   // the renderer's switch for the parameters' blending
    uint64_t postGetJoins = 0, postGetMissing = 0, postGetTaken = 0, postGetOther = 0;  // (other: a bool's, an int's)
    uint64_t replayCycles[kKinds] = {};  // each kind's calls' time at the joins (taken: made; answered: made again)
    std::vector<uint8_t> aoNodes;  // this window's terrain AO nodes, copied for the taken SetTerrainAONodes
    // the Flash players' draws: this window's parameters, vertices, indices and transforms, copied into a room of a
    // fixed size (its copies stay where they are until the next hand-off)
    std::vector<uint8_t> sf;
    size_t sfUsed = 0;
    uint64_t sfJoins = 0, sfOddFormat = 0, sfCopied = 0;
    bool sfTaken = false;  // the draws are taken (their code as expected): the players' renders go on beside the flush
    // the Flash player's Render and Release (CrySystem's CFlashPlayer vtable, patched; null: its renders join)
    void* flashRender = nullptr;
    void* flashRelease = nullptr;
    uint64_t flashReleaseJoins = 0;  // releases of a player with a render taken: joined first
    int loggedPostJoins = 0;
    // the waits for a mark of the flush's (the scene passes, the ocean): set at each and at its end
    HANDLE marks = nullptr;
    volatile bool window = false;  // a flush handed off beside the main thread, not joined yet
    uint64_t windowAt = 0;         // its hand-off (rdtsc)
    // the camera the flush leaves (a diagnostic)
    size_t cameraAt = 0;  // the renderer's camera, as GetCamera reads it (0: not measured)
    unsigned cameraVersions = 0;
    uint64_t setCameraJoins = 0, matrixJoins = 0;
    char* cameraOf = nullptr;  // the renderer whose camera was copied at this window's hand-off
    void* setCamera = nullptr;
    unsigned windowSets = 0;   // the flush's SetCamera calls in this window, the last one's time and caller
    uint64_t lastSetAt = 0;
    void* lastSetBy = nullptr;
    uint64_t camerasCompared = 0, camerasDiffer = 0, cameraParts[kCameraParts] = {}, setsSame = 0, setsDiffer = 0,
             lastSetCyclesDiffer = 0, lastSetsDiffer = 0, camerasMembers = 0, cameraJoins = 0;
    int loggedCameras = 0;
    // a taken SetCamera's matrices, made on the block
    void* cameraInfo = nullptr;  // the camera-info method, its trampoline (null: the matrices after one join)
    uint64_t standInSets = 0, standInCycles = 0, standInSame = 0, standInDiffer = 0, standIn2d = 0;
    int loggedStandIns = 0;
    // the viewport the hand-off's camera copy had (the copy takes the taken SetViewport calls')
    uint8_t handOffViewport[kViewportBytes] = {};
    // the shader resources' references
    void* resFns[std::size(kResSlots)] = {};
    std::vector<int> resPasses;
    ResRef resRefs[kMaxResRefs] = {};  // this window's (a fixed room: the answers point into it)
    size_t resUsed = 0;
    uint64_t resWrites = 0, resJoins = 0;
    // the positions read
    uint64_t posJoins = 0;
    // the texture uploads
    std::vector<uint8_t> upload;  // this window's data copied
    size_t uploadUsed = 0;
    uint64_t uploadBytes = 0, uploadJoins = 0, uploadDiffer = 0;
    int loggedUploads = 0;
    void* getterFns[kGetterClasses][kGetterSlots] = {};  // the field getters answered (null: that slot isn't)
    uint64_t getterMask[kGetterClasses][kGetterSlots] = {};  // the bits of rax each sets
    std::vector<int> getterPasses;                      // their gate indices
    std::vector<int> purePasses;                        // the methods made beside the flush with no record
    uint64_t getterCounts[kGetterClasses][kGetterSlots] = {}, getterDiffer[kGetterClasses][kGetterSlots] = {};
    char* renderer = nullptr;  // the one whose outer flush was handed off last
    struct Stats {  // the frame's statistics when the last flush ended
        bool valid = false;
        int drawCalls = 0, polygons = 0, polygons2 = 0, shadowPolygons2 = 0;
    } stats;
    uint64_t eventJoins = 0;
    int loggedEvents = 0;
    // FlushThreadPresent: the frame's end, sealed at the taken EndFrame for the worker to make after the flush (the
    // calls moved out of `calls`, which the main thread goes on filling), and what the join compares the flush's end
    // with, copied by the worker before it
    std::vector<Call> sealed;
    uint64_t seals = 0, sealsRefused = 0, refusedBy[kKinds] = {};
    uint64_t workerCycles[kKinds] = {}, workerFrames = 0;
    struct Left {
        bool valid = false;  // (the worker copied these after the flush)
        bool view = false, proj = false, camera = false;
        uint8_t viewport[kViewportBytes] = {};
        Stats stats;
    } left;
    void* fns[kKinds] = {};    // the methods
    int passes[kKinds];        // the gate's indices of their slots
    std::vector<Call> calls;   // since the hand-off
    uint64_t counts[kKinds] = {}, differ[kKinds] = {}, joins = 0, most = 0;
    int logged = 0;
};
State g;

uint64_t B(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
float F(uint64_t b) {
    const uint32_t v = uint32_t(b);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
template <class T>
T Fn(Kind k) {
    return reinterpret_cast<T>(g.fns[k]);
}
Call& Record(Kind k, void* renderer) {
    ++g.counts[k];
    Call& c = g.calls.emplace_back();
    c.kind = k;
    c.renderer = renderer;
    return c;
}

// --- the taken calls ---------------------------------------------------------------------------------------------
void DeleteRenderMeshHook(void* r, void* mesh) {
    if (!FlushThread_Beside()) return Fn<DeleteRenderMeshFn>(kDeleteRenderMesh)(r, mesh);
    Record(kDeleteRenderMesh, r).a[0] = uint64_t(mesh);
}
bool EnableFogHook(void* r, bool enable) {
    if (!FlushThread_Beside()) return Fn<BoolFn>(kEnableFog)(r, enable);
    Record(kEnableFog, r).a[0] = enable;
    return enable;  // (the 3D engine's calls don't read it)
}
void EnableTMUHook(void* r, bool enable) {
    if (!FlushThread_Beside()) return Fn<VoidBoolFn>(kEnableTMU)(r, enable);
    Record(kEnableTMU, r).a[0] = enable;
}
void SelectTMUHook(void* r, int unit) {
    if (!FlushThread_Beside()) return Fn<VoidIntFn>(kSelectTMU)(r, unit);
    Record(kSelectTMU, r).a[0] = uint64_t(int64_t(unit));
}
void Set2DModeHook(void* r, bool enable, int ortoX, int ortoY, float zNear, float zFar) {
    if (!FlushThread_Beside()) return Fn<Set2DModeFn>(kSet2DMode)(r, enable, ortoX, ortoY, zNear, zFar);
    g.depth2d += enable ? 1 : -1;
    Call& c = Record(kSet2DMode, r);
    c.a[0] = enable;
    c.a[1] = uint64_t(int64_t(ortoX));
    c.a[2] = uint64_t(int64_t(ortoY));
    c.a[3] = B(zNear);
    c.a[4] = B(zFar);
}
void SetCullModeHook(void* r, int mode) {
    if (!FlushThread_Beside()) return Fn<VoidIntFn>(kSetCullMode)(r, mode);
    Record(kSetCullMode, r).a[0] = uint64_t(int64_t(mode));
}
void SetColorOpHook(void* r, uint8_t co, uint8_t ao, uint8_t ca, uint8_t aa) {
    if (!FlushThread_Beside()) return Fn<SetColorOpFn>(kSetColorOp)(r, co, ao, ca, aa);
    Call& c = Record(kSetColorOp, r);
    c.a[0] = co;
    c.a[1] = ao;
    c.a[2] = ca;
    c.a[3] = aa;
}
void SetStateHook(void* r, int state, int alphaRef) {
    if (!FlushThread_Beside()) return Fn<SetStateFn>(kSetState)(r, state, alphaRef);
    Call& c = Record(kSetState, r);
    c.a[0] = uint64_t(int64_t(state));
    c.a[1] = uint64_t(int64_t(alphaRef));
}
void Draw2dImageHook(void* r, float x, float y, float w, float h, int texture, float s0, float t0, float s1, float t1,
                     float angle, float cr, float cg, float cb, float ca, float z) {
    if (!FlushThread_Beside())
        return Fn<Draw2dImageFn>(kDraw2dImage)(r, x, y, w, h, texture, s0, t0, s1, t1, angle, cr, cg, cb, ca, z);
    Call& c = Record(kDraw2dImage, r);
    const float f[14] = {x, y, w, h, s0, t0, s1, t1, angle, cr, cg, cb, ca, z};
    for (int i = 0; i < 14; ++i) c.a[i] = B(f[i]);
    c.a[14] = uint64_t(int64_t(texture));
}

template <Kind K>
void Ints2Hook(void* r, uint64_t a, uint64_t b) {
    if (!FlushThread_Beside()) return Fn<Ints2Fn>(K)(r, a, b);
    if (K == kFontSetRenderingState) ++g.depth2d;
    Call& c = Record(K, r);
    c.a[0] = a;
    c.a[1] = b;
}
void WaitScenesFree();
// FlushThreadPresent: at the taken EndFrame, the calls taken so far are the frame's end, for the worker to make after
// the flush, if each of them is its to make: the renderer's own code, over what the main thread doesn't go on changing
// (not the default materials, whose references are the 3D engine's code; not a Flash player's whole render,
// CrySystem's; not a shader's release that destroys it; not a window's event, whose display mode changes belong on
// the window's thread). Else the join makes them, as before.
constexpr uint64_t kEventRandomSeed = 1;  // (ESYSTEM_EVENT_RANDOM_SEED; the renderer's listener: SysEventHook)
bool WorkerMakes(const Call& c) {
    switch (c.kind) {
        case kSetDefaultMaterials:
        case kFlashRender: return false;
        case kShaderRelease: return c.out[0] != 0;
        case kOnSystemEvent: return c.a[0] == kEventRandomSeed;
        default: return true;
    }
}
void Seal() {
    if (g.calls.empty() || !FlushThread_SealOpen()) return;
    for (const Call& c : g.calls)
        if (!WorkerMakes(c)) {
            ++g.sealsRefused;
            ++g.refusedBy[c.kind];
            return;
        }
    g.sealed.swap(g.calls);  // (empty since the last window's frame's end was made: the main thread goes on in `calls`)
    AcquireSRWLockExclusive(&g.refsLock);  // (the counted references taken so far go with them: pending on the worker)
    for (State::Ref& r : g.refs) {
        r.sealed += r.window;
        r.window = 0;
    }
    ReleaseSRWLockExclusive(&g.refsLock);
    if (FlushThread_Seal()) {
        ++g.seals;
        return;
    }
    g.sealed.swap(g.calls);
    AcquireSRWLockExclusive(&g.refsLock);  // (not sealed after all: the join makes them)
    for (State::Ref& r : g.refs) {
        r.window += r.sealed;
        r.sealed = 0;
    }
    ReleaseSRWLockExclusive(&g.refsLock);
}
template <Kind K>
void Ints0Hook(void* r) {
    if (!FlushThread_Beside()) return Fn<Ints0Fn>(K)(r);
    if (K == kFontRestoreRenderingState) --g.depth2d;
    if (K == kBeginFrame) {
        ++g.pendingFrames;
        WaitScenesFree();  // (the next update only once the flush can't run a scene traversal any more)
        g.pastBeginFrame = true;
    }
    Record(K, r);
    if (K == kEndFrame) Seal();
}
void Vf214Hook(void* r, uint64_t a, uint64_t b) {  // (two value arguments, neither dereferenced; no result)
    if (!FlushThread_Beside()) return Fn<Ints2Fn>(kVf214)(r, a, b);
    Call& c = Record(kVf214, r);
    c.a[0] = a;
    c.a[1] = b;
}
void AuxFlushHook(void* aux, uint64_t a) {
    if (!FlushThread_Beside()) return Fn<Ints1Fn>(kAuxFlush)(aux, a);
    Record(kAuxFlush, aux).a[0] = a;
}

// a measured room: the bytes up to the last 4 that aren't the fill
size_t Written(const uint8_t* room, size_t bytes) {
    for (size_t n = bytes / 4; n > 0; --n) {
        uint32_t w;
        std::memcpy(&w, room + (n - 1) * 4, 4);
        if (w != 0x01010101u * kUnwritten) return n * 4;
    }
    return 0;
}
// a measured draw: the pool's vertex size is the bytes written over the drawn vertices, if every draw agrees
void Sample(int pool, size_t written, int drawn, int locked) {
    Pool& q = g.pools[pool];
    if (g.loggedSamples < kLoggedSamples) {
        ++g.loggedSamples;
        Log("render queue: the text measured: pool %d, %zu bytes written for a draw of %d vertices (locked for %d)",
            pool, written, drawn, locked);
    }
    if (drawn <= 0 || !written) return;
    if (written % size_t(drawn) != 0) {
        q.contradicted = true;
        return;
    }
    const uint32_t s = uint32_t(written / size_t(drawn));
    if (!q.stride) q.stride = s;
    if (s == q.stride)
        ++q.samples;
    else
        q.contradicted = true;
}
bool StrideKnown(const Pool& q) { return q.samples >= kStrideSamples && !q.contradicted; }

// the lock's vertices into the renderer's scratch, once, right before their draw (of `drawn` vertices): the measured
// bytes, or the drawn vertices at the known size (both within what the renderer's lock gave room for)
Lock* TokenLock(int offs) {
    const int i = offs - kTokenBase;
    return i >= 0 && size_t(i) < g.locks.size() ? &g.locks[size_t(i)] : nullptr;
}
void CopyLock(Lock& k, int drawn) {
    if (k.copied || !k.real) return;
    k.copied = true;
    const uint8_t* const room = g.text.data() + k.at;
    size_t bytes;
    if (k.measure) {
        bytes = Written(room, k.room);
        ++g.locksMeasured;
        Sample(k.pool, bytes, drawn, k.verts);
    } else {
        bytes = size_t((std::max)(0, (std::min)(drawn, k.verts))) * g.pools[k.pool].stride;
    }
    std::memcpy(k.real, room, bytes);
}

void* GetDynVBPtrHook(void* r, int verts, int& offs, int pool) {
    if (!FlushThread_Beside()) return Fn<GetDynVBPtrFn>(kGetDynVBPtr)(r, verts, offs, pool);
    const Pool* q = pool >= 0 && pool < kPools ? &g.pools[pool] : nullptr;
    const bool known = q && StrideKnown(*q);
    const size_t room = q && verts > 0 ? size_t(verts) * (known ? q->stride : kRoomPerVertex) : 0;
    // (nor more locks than reserved: the frame's end on the worker makes its own while this thread adds more)
    if (!room || g.textUsed + room > g.text.size() || g.locks.size() == g.locks.capacity()) {
        ++g.textMisses;
        FlushThread_JoinNow();  // (not taken: the flush ends here, and the call is made as before)
        return Fn<GetDynVBPtrFn>(kGetDynVBPtr)(r, verts, offs, pool);
    }
    Lock k{pool, verts, g.textUsed, room, nullptr, 0, !known, false, false};
    if (k.measure) std::memset(g.text.data() + k.at, kUnwritten, room);
    g.textUsed += (room + 15) & ~size_t(15);
    g.locks.push_back(k);
    ++g.locksTaken;
    offs = kTokenBase + int(g.locks.size() - 1);
    Call& c = Record(kGetDynVBPtr, r);
    c.a[0] = uint64_t(g.locks.size() - 1);
    return g.text.data() + k.at;
}

void DrawDynVBHook(void* r, int offs, int pool, int verts) {
    if (!FlushThread_Beside()) {
        if (Lock* k = TokenLock(offs)) {  // a lock taken beside the flush, drawn after the join
            if (!k->real) return;          // (the renderer's lock failed: nothing to draw)
            CopyLock(*k, verts);
            offs = k->realOffs;
        }
        return Fn<DrawDynVBFn>(kDrawDynVB)(r, offs, pool, verts);
    }
    Call& c = Record(kDrawDynVB, r);
    c.a[0] = uint64_t(int64_t(offs));
    c.a[1] = uint64_t(int64_t(pool));
    c.a[2] = uint64_t(int64_t(verts));
}

// --- the answered calls (made at once; checked at the join) --------------------------------------------------------
template <Kind K>
int IntHook(void* r) {
    const int v = Fn<IntFn>(K)(r);
    if (FlushThread_Beside()) Record(K, r).out[0] = uint32_t(v);
    return v;
}
template <Kind K>
float ScaleHook(void* r, float value) {
    const float v = Fn<ScaleFn>(K)(r, value);
    if (FlushThread_Beside()) {
        Call& c = Record(K, r);
        c.a[0] = B(value);
        c.out[0] = uint32_t(B(v));
    }
    return v;
}
// (a BeginFrame taken in the window, not made yet, adds 1 to both frame ids it answers from: so does the answer.
// The recursive one also counts the scene passes the flush renders itself (a reflection's, from the first list it
// processes), which come before the main pass's shadow pass: it waits for that, or for the flush's end (no shadow
// pass in it), and then joins.)
// beside the flush: until a mark of the flush's is set, or the flush has ended, blocking on the marks' event, which
// the flush's thread sets at each mark and at its end (looked at again after the reset: a mark set meanwhile)
constexpr DWORD kMarkWaitMs = 100;           // (then it looks again)
constexpr uint64_t kMarkSpinCycles = 60000;  // spun first, about 20 us (the reports' 3 GHz): a mark that comes soon
void WaitMark(const volatile bool& mark) {
    // (the flush's end, not the worker's: the frame's end it may make after it waits for this thread's seal)
    const uint64_t c0 = __rdtsc();
    while (!mark && !FlushThread_FlushEnded() && __rdtsc() - c0 < kMarkSpinCycles) YieldProcessor();
    while (!mark && !FlushThread_FlushEnded()) {
        if (!g.marks) {
            SwitchToThread();
            continue;
        }
        ResetEvent(g.marks);
        if (mark || FlushThread_FlushEnded()) break;
        WaitForSingleObject(g.marks, kMarkWaitMs);
    }
}
// beside the flush, at the next frame's start: until the flush can't run a scene traversal any more (the 3D engine's
// code, over the data the main thread's update changes, with its locks), or has ended
void WaitScenesFree() {
    if (g.scenesFree) return;
    const uint64_t c0 = __rdtsc();
    WaitMark(g.scenesFree);
    ++g.sceneWaits;
    g.sceneWaitCycles += __rdtsc() - c0;
}
bool WaitScenesDone() {
    if (g.scenesDone) return true;
    const uint64_t c0 = __rdtsc();
    WaitMark(g.scenesDone);
    ++g.scenesWaits;
    g.scenesWaitCycles += __rdtsc() - c0;
    return g.scenesDone;
}
int GetFrameIDHook(void* r, bool recursive) {
    if (recursive && FlushThread_Beside() && !WaitScenesDone()) {
        ++g.frameIdJoins;
        FlushThread_JoinNow();
    }
    int v = Fn<FrameIdFn>(kGetFrameID)(r, recursive);
    if (FlushThread_Beside()) {
        v += g.pendingFrames;
        Call& c = Record(kGetFrameID, r);
        c.a[0] = recursive;
        c.out[0] = uint32_t(v);
    }
    return v;
}
template <Kind K>
void* PtrHook(void* r) {
    void* const v = Fn<PtrFn>(K)(r);
    if (FlushThread_Beside()) {
        Call& c = Record(K, r);
        c.out[0] = uint32_t(uintptr_t(v));
        c.out[1] = uint32_t(uintptr_t(v) >> 32);
    }
    return v;
}
uint8_t MultiGPUHook(void* r) {
    const uint8_t v = Fn<FlagFn>(kIsMultiGPUModeActive)(r);
    if (FlushThread_Beside()) Record(kIsMultiGPUModeActive, r).out[0] = v;
    return v;
}
// The frame's statistics are those the flush is still counting (read beside it, they are 0 or partial: every answer
// made at once differed at the join). Beside the flush they answer the last flush's, one frame late, as a render
// thread's statistics are; without one yet, they join.
template <Kind K>
int StatHook(void* r) {
    if (!FlushThread_Beside()) return Fn<IntFn>(K)(r);
    if (!g.stats.valid) {
        FlushThread_JoinNow();
        return Fn<IntFn>(K)(r);
    }
    const int v = K == kGetPolyCount0 ? g.stats.polygons : g.stats.drawCalls;
    Record(K, r).out[0] = uint32_t(v);
    return v;
}
void PolyCount2Hook(void* r, int* polygons, int* shadowPolygons) {
    if (!FlushThread_Beside()) return Fn<PolyCount2Fn>(kGetPolyCount2)(r, polygons, shadowPolygons);
    if (!g.stats.valid) {
        FlushThread_JoinNow();
        return Fn<PolyCount2Fn>(kGetPolyCount2)(r, polygons, shadowPolygons);
    }
    *polygons = g.stats.polygons2;
    *shadowPolygons = g.stats.shadowPolygons2;
    Call& c = Record(kGetPolyCount2, r);
    c.out[0] = uint32_t(*polygons);
    c.out[1] = uint32_t(*shadowPolygons);
}
// at the join, the flush ended: its statistics, for the next window (FlushThreadPresent: the worker's copy of them,
// taken right after the flush)
void StashStats(void* r, State::Stats& s = g.stats) {
    if (!r) return;
    s.drawCalls = Fn<IntFn>(kGetCurrentNumberOfDrawCalls)(r);
    s.polygons = Fn<IntFn>(kGetPolyCount0)(r);
    Fn<PolyCount2Fn>(kGetPolyCount2)(r, &s.polygons2, &s.shadowPolygons2);
    s.valid = true;
}

bool CopyString(const char* s, uint64_t& at);
const char* Str(uint64_t at);

void Draw2dLabelHook(void* r, float x, float y, float size, const float* color, bool center, const char* format, ...) {
    char text[kMaxString];
    va_list args;
    va_start(args, format);
    g.vsnprintf80(text, sizeof text - 1, format, args);
    va_end(args);
    text[sizeof text - 1] = 0;
    uint64_t at = 0;
    if (FlushThread_Beside() && !CopyString(text, at)) {
        ++g.labelJoins;
        FlushThread_JoinNow();
    }
    if (!FlushThread_Beside()) return Fn<Draw2dLabelFn>(kDraw2dLabel)(r, x, y, size, color, center, "%s", text);
    Call& c = Record(kDraw2dLabel, r);
    c.a[0] = B(x);
    c.a[1] = B(y);
    c.a[2] = B(size);
    c.a[3] = color != nullptr;
    for (int i = 0; i < 4 && color; ++i) c.a[4 + i] = B(color[i]);
    c.a[8] = center;
    c.a[9] = at;
}

// The renderer's system event listener (CSystemEventListner_Render). ESYSTEM_EVENT_RANDOM_SEED (1), which CrySystem
// sends once a frame in the timedemo runs, only sets and clears the handler's own re-entry flag: taken. (Taking it
// let the window reach into the timedemo's restart, which resets the level; the join there, deep inside it, once had
// the particle part's counter updates after the flush wait for a container's lock for good: render-thread.md §11.3,
// step 3k. The parts' after-flush steps now keep what they write from being freed meanwhile, allocator.h's pins, and
// don't wait for the game's locks.) The window's events, which the game's window procedure sends when the window gains
// or loses the focus, moves, is resized or leaves or enters full screen (ESYSTEM_EVENT_CHANGE_FOCUS, MOVE, RESIZE,
// TOGGLE_FULLSCREEN: 10, 11, 12, 17), are taken too: the window procedure may run inside DXGI's own handling of the
// window (a full-screen switch), which a join there would wait in, with the present on the flush's thread waiting for
// DXGI. The listener's work for them (the display mode among it) is then made at the join, on the main thread. The
// other events do the renderer's work where they come (a level's load): they join, the first logged.
constexpr bool kTakeRandomSeed = true;
bool TakenEvent(uint64_t event) {
    return (kTakeRandomSeed && event == kEventRandomSeed) || event == 10 || event == 11 || event == 12 || event == 17;
}
void SysEventHook(void* listener, uint64_t event, uint64_t wparam, uint64_t lparam) {
    if (FlushThread_Beside() && TakenEvent(event)) {
        Call& c = Record(kOnSystemEvent, listener);
        c.a[0] = event;
        c.a[1] = wparam;
        c.a[2] = lparam;
        return;
    }
    if (FlushThread_Beside()) {
        if (g.loggedEvents < kLoggedEvents) {
            ++g.loggedEvents;
            char where[128];
            Log("render queue: the renderer's OnSystemEvent(%llu, 0x%llX, 0x%llX) joins the flush, sent from %s", event,
                wparam, lparam, DescribeAddress(_ReturnAddress(), where, sizeof where));
        }
        ++g.eventJoins;
        FlushThread_JoinNow();
    }
    Fn<Ints3Fn>(kOnSystemEvent)(listener, event, wparam, lparam);
}
template <Kind K>
int IntIntHook(void* r, int arg) {
    const int v = Fn<IntIntFn>(K)(r, arg);
    if (FlushThread_Beside()) {
        Call& c = Record(K, r);
        c.a[0] = uint64_t(int64_t(arg));
        c.out[0] = uint32_t(v);
    }
    return v;
}
// (its answer byte is what its projection's last call leaves, the low byte of a stack address, which follows the
// caller's stack depth and is zero now and then: not compared)
uint8_t ProjectToScreenHook(void* r, float x, float y, float z, float* sx, float* sy, float* sz) {
    if (!FlushThread_Beside()) return Fn<ProjectFn>(kProjectToScreen)(r, x, y, z, sx, sy, sz);
    if (!g.camera.valid || !g.camera.matrices || g.camera.renderer != r || g.depth2d != 0) {
        ++g.projectJoins;
        FlushThread_JoinNow();  // (the camera isn't the hand-off's: the flush ends here, and the call is made)
        return Fn<ProjectFn>(kProjectToScreen)(r, x, y, z, sx, sy, sz);
    }
    const uint8_t v = Fn<ProjectFn>(kProjectToScreen)(g.camera.block.data(), x, y, z, sx, sy, sz);
    Call& c = Record(kProjectToScreen, r);
    c.a[0] = B(x);
    c.a[1] = B(y);
    c.a[2] = B(z);
    c.out[0] = uint32_t(B(*sx));
    c.out[1] = uint32_t(B(*sy));
    c.out[2] = uint32_t(B(*sz));
    c.out[3] = 0;
    return v;
}
int UnProjectHook(void* r, float sx, float sy, float sz, float* px, float* py, float* pz) {
    if (!FlushThread_Beside()) return Fn<UnProjectFn>(kUnProject)(r, sx, sy, sz, px, py, pz);
    if (g.blockVtable.empty() || !g.camera.valid || !g.camera.matrices || g.camera.renderer != r || g.depth2d != 0) {
        ++g.unprojectJoins;
        FlushThread_JoinNow();  // (other matrices than the copy's: the flush ends here, and the call is made)
        return Fn<UnProjectFn>(kUnProject)(r, sx, sy, sz, px, py, pz);
    }
    const int v = Fn<UnProjectFn>(kUnProject)(g.camera.block.data(), sx, sy, sz, px, py, pz);
    Call& c = Record(kUnProject, r);
    c.a[0] = B(sx);
    c.a[1] = B(sy);
    c.a[2] = B(sz);
    c.out[0] = uint32_t(B(*px));
    c.out[1] = uint32_t(B(*py));
    c.out[2] = uint32_t(B(*pz));
    c.out[3] = uint32_t(v);
    return v;
}
// GetViewport reads the viewport ProjectToScreen does, and nothing else (its code checked at the install): from the
// same block. (Set2DMode and the font's state push matrices, not the viewport.)
void GetViewportHook(void* r, int* x, int* y, int* width, int* height) {
    if (!FlushThread_Beside()) return Fn<ViewportFn>(kGetViewport)(r, x, y, width, height);
    if (!g.viewportLayout || !g.camera.valid || g.camera.renderer != r) {
        ++g.viewportJoins;
        FlushThread_JoinNow();
        return Fn<ViewportFn>(kGetViewport)(r, x, y, width, height);
    }
    Fn<ViewportFn>(kGetViewport)(g.camera.block.data(), x, y, width, height);
    Call& c = Record(kGetViewport, r);
    c.out[0] = uint32_t(*x);
    c.out[1] = uint32_t(*y);
    c.out[2] = uint32_t(*width);
    c.out[3] = uint32_t(*height);
}
void SetClearColorHook(void* r, const float* color) {  // (const Vec3&: copied)
    if (!FlushThread_Beside()) return Fn<ClearColorFn>(kSetClearColor)(r, color);
    Call& c = Record(kSetClearColor, r);
    for (int i = 0; i < 3; ++i) c.a[i] = B(color[i]);
}

void AuxSetFlagsHook(void* aux, const uint32_t* flags) {
    if (!FlushThread_Beside()) return Fn<AuxSetFlagsFn>(kAuxSetRenderFlags)(aux, flags);
    Record(kAuxSetRenderFlags, aux).a[0] = *flags;
    g.auxFlags = {aux, *flags, true};
}
uint32_t* AuxGetFlagsHook(void* aux, uint32_t* out) {
    if (!FlushThread_Beside()) return Fn<AuxGetFlagsFn>(kAuxGetRenderFlags)(aux, out);
    if (g.auxFlags.set && g.auxFlags.aux == aux)
        *out = g.auxFlags.flags;
    else
        Fn<AuxGetFlagsFn>(kAuxGetRenderFlags)(aux, out);
    Record(kAuxGetRenderFlags, aux).out[0] = *out;
    return out;
}
// The object's entry among this window's counted references (added at nothing pending); under refsLock.
State::Ref& RefOf(void* obj) {
    auto it = std::find_if(g.refs.begin(), g.refs.end(), [obj](const State::Ref& r) { return r.obj == obj; });
    if (it == g.refs.end()) it = g.refs.insert(g.refs.end(), {obj, 0, 0});
    return *it;
}
// A taken AddRef's or Release's answer: the object's count once the calls taken before it are made. Some may be sealed
// for the worker, which makes them after the flush while the main thread goes on here (FlushThreadPresent): those it
// has made are in the count, the others are pending in the entry.
template <Kind K>
int CountedAddRefHook(void* obj) {
    if (!FlushThread_Beside()) return Fn<CountedAddRefFn>(K)(obj);
    const unsigned at = g.countAt[K == kTextureAddRef];
    AcquireSRWLockExclusive(&g.refsLock);
    State::Ref& ref = RefOf(obj);
    const int v = *reinterpret_cast<const int*>(static_cast<const char*>(obj) + at) + ref.sealed + ref.window++;
    ReleaseSRWLockExclusive(&g.refsLock);
    Record(K, obj).out[0] = uint32_t(v);
    return v;
}
int ShaderReleaseHook(void* obj) {
    if (!FlushThread_Beside()) return Fn<CountedAddRefFn>(kShaderRelease)(obj);
    const char* const s = static_cast<const char*>(obj);
    int v = -1;  // (not counted)
    if (!(*reinterpret_cast<const uint32_t*>(s + kShaderFlags) & kShaderUncounted)) {
        AcquireSRWLockExclusive(&g.refsLock);
        State::Ref& ref = RefOf(obj);
        v = *reinterpret_cast<const int*>(s + kShaderCount) + ref.sealed + --ref.window;
        ReleaseSRWLockExclusive(&g.refsLock);
        if (v == 0) ++g.releasesToZero;  // (destroyed at the join)
    }
    Record(kShaderRelease, obj).out[0] = uint32_t(v);
    return v;
}
// SetSkyLightRenderParams (the 3D engine's sky light parameters, once a frame in its traversal) only sets the
// renderer's field, which the flush reads (the engine data part has pointed it at its copy until the flush's end):
// taken, the pointer kept
void SetSkyLightHook(void* r, uint64_t params) {
    if (!FlushThread_Beside()) return Fn<Ints1Fn>(kSetSkyLight)(r, params);
    Record(kSetSkyLight, r).a[0] = params;
}
// SetTerrainAONodes (slot 142: the 3D engine's terrain ambient occlusion nodes, once a frame in its traversal) clears
// the renderer's array (+0x2D870) and appends a copy of the given one (a PodArray of 72-byte nodes), which the flush's
// terrain pass reads: taken, the nodes copied, made at the join over a PodArray of the copy
struct PodArrayView {
    const void* elems;
    int32_t count, capacity;
};
using SetAONodesFn = void (*)(void*, const PodArrayView*);
void SetTerrainAOHook(void* r, const PodArrayView* nodes) {
    if (!FlushThread_Beside() || !nodes || nodes->count < 0) return Fn<SetAONodesFn>(kSetTerrainAO)(r, nodes);
    // (within what is reserved, as the strings)
    if (g.aoNodes.size() + size_t(nodes->count) * kAONodeBytes > g.aoNodes.capacity()) {
        FlushThread_JoinNow();
        return Fn<SetAONodesFn>(kSetTerrainAO)(r, nodes);
    }
    const size_t at = g.aoNodes.size();
    const uint8_t* const src = static_cast<const uint8_t*>(nodes->elems);
    if (src) g.aoNodes.insert(g.aoNodes.end(), src, src + size_t(nodes->count) * kAONodeBytes);
    Call& c = Record(kSetTerrainAO, r);
    c.a[0] = at;
    c.a[1] = src ? uint64_t(nodes->count) : 0;
}
// The Flash players' draws (Scaleform's, through CrySystem's renderer for it): the triangle lists, the line strips and
// the glyphs take an SSF_GlobalDrawParams (the Mod SDK's layout, 0xD0 bytes), which points at Scaleform's vertices and
// indices, reused after the call, and at the shape's transform (a Matrix34). Each draw copies numVertices vertices into
// the renderer's dynamic buffer, at its format's size, and the triangle lists numIndices 16-bit indices; while it
// draws, the renderer's shader constants read the parameters through a renderer field (+0x28FC0): the transform through
// its pointer, the texture matrices and the color transforms in the struct. Beside the flush the struct, the vertices,
// the indices and the transform are copied into the queue's room, the copy's pointers at the copies, and the draw is
// taken; so are the stencil mask's set-up and the renderer's flush for Scaleform (integers only), and ClearBuffer (its
// color copied). A texture made or updated (SF_CreateTexture, which answers its id; SF_UpdateTexture, rarely) joins.
// The draws' code is checked at the install; with them taken, the players' renders go on beside the flush as the game
// calls them, the movies drawn as they are then.
constexpr size_t kSFParams = 0xD0, kSFVertexFmt = 0x04, kSFVertices = 0x08, kSFNumVertices = 0x10, kSFIndices = 0x18,
                 kSFNumIndices = 0x20, kSFTransform = 0x28, kSFMatrixBytes = 48;
constexpr size_t kSFBytes = size_t(8) << 20;  // the room, per window
using SFTriListFn = void (*)(void*, int, int, int, int, int, const void*);
using SFLineStripFn = void (*)(void*, int, int, const void*);
using SFGlyphFn = void (*)(void*, const void*);
using ClearBufferFn = void (*)(void*, uint32_t, const float*, float);
// a draw's vertex size by format (EVertexFmt: 1 XY16i, 2 XY16iC32, 3 XY16iCF32, 4 Glyph), as its code copies them;
// 0: a format that draw doesn't copy
uint32_t SFStride(Kind k, int32_t fmt) {
    if (fmt == 1) return 4;
    if (k == kSFDrawTriList) return fmt == 2 ? 8 : fmt == 3 ? 12 : 0;
    if (k == kSFDrawGlyphClear) return fmt == 4 ? 20 : 0;
    return 0;
}
uint8_t* SFRoom(size_t bytes) {  // (16-byte aligned; null: the room is full)
    const size_t at = (g.sfUsed + 15) & ~size_t(15);
    if (at + bytes > g.sf.size()) return nullptr;
    g.sfUsed = at + bytes;
    return g.sf.data() + at;
}
// a draw's parameters copied with what they point at; its place in the room, or ~0 (no room, or the vertices are in a
// format that draw doesn't copy)
uint64_t CopySFParams(Kind k, const uint8_t* p) {
    const size_t mark = g.sfUsed;
    uint8_t* const q = SFRoom(kSFParams);
    if (!q) return ~uint64_t(0);
    std::memcpy(q, p, kSFParams);
    int32_t fmt;
    uint32_t numVertices, numIndices;
    const uint8_t *vertices, *indices, *transform;
    std::memcpy(&fmt, p + kSFVertexFmt, 4);
    std::memcpy(&vertices, p + kSFVertices, 8);
    std::memcpy(&numVertices, p + kSFNumVertices, 4);
    std::memcpy(&indices, p + kSFIndices, 8);
    std::memcpy(&numIndices, p + kSFNumIndices, 4);
    std::memcpy(&transform, p + kSFTransform, 8);
    const uint32_t stride = SFStride(k, fmt);
    const struct {
        const uint8_t* from;
        size_t bytes, field;
    } parts[3] = {{vertices, size_t(numVertices) * stride, kSFVertices},
                  {indices, size_t(numIndices) * 2, kSFIndices},
                  {transform, kSFMatrixBytes, kSFTransform}};
    if (vertices && !stride) {
        ++g.sfOddFormat;
        g.sfUsed = mark;
        return ~uint64_t(0);
    }
    for (const auto& part : parts) {
        if (!part.from || !part.bytes) continue;
        uint8_t* const to = SFRoom(part.bytes);
        if (!to) {
            g.sfUsed = mark;
            return ~uint64_t(0);
        }
        std::memcpy(to, part.from, part.bytes);
        std::memcpy(q + part.field, &to, 8);
    }
    g.sfCopied += g.sfUsed - mark;
    return uint64_t(q - g.sf.data());
}
void SFTriListHook(void* r, int baseVertex, int minVertex, int numVertices, int startIndex, int triangles,
                   const uint8_t* params) {
    const uint64_t at = FlushThread_Beside() ? CopySFParams(kSFDrawTriList, params) : ~uint64_t(0);
    if (at == ~uint64_t(0)) {
        if (FlushThread_Beside()) {
            ++g.sfJoins;
            FlushThread_JoinNow();  // (not taken: the flush ends here, and the call is made as before)
        }
        return Fn<SFTriListFn>(kSFDrawTriList)(r, baseVertex, minVertex, numVertices, startIndex, triangles, params);
    }
    Call& c = Record(kSFDrawTriList, r);
    const int v[5] = {baseVertex, minVertex, numVertices, startIndex, triangles};
    for (int i = 0; i < 5; ++i) c.a[i] = uint64_t(int64_t(v[i]));
    c.a[5] = at;
}
void SFLineStripHook(void* r, int baseVertex, int lines, const uint8_t* params) {
    const uint64_t at = FlushThread_Beside() ? CopySFParams(kSFDrawLineStrip, params) : ~uint64_t(0);
    if (at == ~uint64_t(0)) {
        if (FlushThread_Beside()) {
            ++g.sfJoins;
            FlushThread_JoinNow();
        }
        return Fn<SFLineStripFn>(kSFDrawLineStrip)(r, baseVertex, lines, params);
    }
    Call& c = Record(kSFDrawLineStrip, r);
    c.a[0] = uint64_t(int64_t(baseVertex));
    c.a[1] = uint64_t(int64_t(lines));
    c.a[2] = at;
}
void SFGlyphHook(void* r, const uint8_t* params) {
    const uint64_t at = FlushThread_Beside() ? CopySFParams(kSFDrawGlyphClear, params) : ~uint64_t(0);
    if (at == ~uint64_t(0)) {
        if (FlushThread_Beside()) {
            ++g.sfJoins;
            FlushThread_JoinNow();
        }
        return Fn<SFGlyphFn>(kSFDrawGlyphClear)(r, params);
    }
    Record(kSFDrawGlyphClear, r).a[0] = at;
}
void ClearBufferHook(void* r, uint32_t flags, const float* color, float depth) {
    if (!FlushThread_Beside()) return Fn<ClearBufferFn>(kClearBuffer)(r, flags, color, depth);
    Call& c = Record(kClearBuffer, r);
    c.a[0] = flags;
    c.a[1] = color != nullptr;
    for (int i = 0; i < 4 && color; ++i) c.a[2 + i] = B(color[i]);
    c.a[6] = B(depth);
}
// what the draws' code is checked for (anywhere in its first kSFScan bytes): the vertex sizes and the copies' fields
// as above, and the parameters handed to the shader constants at +0x28FC0
constexpr size_t kSFScan = 0x600;
const char* const kSFTriListCode[] = {
    "41 BF 04 00 00 00",                                                      // mov r15d, 4
    "41 BC 08 00 00 00",                                                      // mov r12d, 8
    "44 8D 61 0B",                                                            // lea r12d, [rcx+0Bh]: 12
    "8B 5E 10 4C 8D 84 24 ?? ?? ?? ?? 48 8B CF C7 84 24 ?? ?? ?? ?? FF FF FF FF 41 0F AF DC",  // numVertices * size
    "48 8B 56 08 44 8B C3",                                                   // the vertices, that many bytes
    "44 8B 46 20 48 8B 56 18 4D 03 C0",                                       // the indices, numIndices * 2
    "48 89 B7 C0 8F 02 00"};                                                  // the parameters at +0x28FC0
const char* const kSFLineStripCode[] = {
    "83 7D 04 01 75 06 41 BC 04 00 00 00 8B 5D 10",  // XY16i: 4 bytes, numVertices
    "48 8B 55 08 44 8B C3",                          // the vertices
    "C0 8F 02 00"};                                  // (+0x28FC0)
const char* const kSFGlyphCode[] = {
    "8B 4D 04 83 E9 01 74 0B 83 F9 03 75 0C 44 8D 61 11 EB 06 41 BC 04 00 00 00 8B 5D 10",  // 4 or 20 bytes
    "48 8B 55 08 44 8B C3",
    "48 89 AF C0 8F 02 00"};
bool CodeHas(char* base, const char* fn, const char* code) {
    const Pattern p = ParsePattern(code);
    if (!IsInCode(base, fn, kSFScan + p.size())) return false;
    for (size_t i = 0; i < kSFScan; ++i)
        if (MatchesAt(fn + i, p)) return true;
    return false;
}

// The Flash player's Render (CrySystem's CFlashPlayer, slot 9: the HUD's and the menus' movies, each on its own) draws
// its movie through the renderer (EF_Query, the Scaleform calls, ...) right after the outer flush is handed off. Where
// its draws can't be taken (their code isn't as expected), the whole render is taken beside the flush instead, and made
// at the join after the calls taken before it, before the present. The movie is drawn as the game's update beside the
// flush has left it by then (one update on, at most). Its Release (slot 0) deletes it: a player with a render taken
// joins first (the render made, then the release).
using FlashFn = void (*)(void*);
void FlashRenderHook(void* player) {
    if (!FlushThread_Beside()) return reinterpret_cast<FlashFn>(g.flashRender)(player);
    Record(kFlashRender, player);
}
void FlashReleaseHook(void* player) {
    if (FlushThread_Beside()) {
        for (const Call& c : g.calls) {
            if (c.kind != kFlashRender || c.renderer != player) continue;
            ++g.flashReleaseJoins;
            FlushThread_JoinNow();
            break;
        }
    }
    reinterpret_cast<FlashFn>(g.flashRelease)(player);
}

const uint8_t* OceanVector(char* re, size_t* bytes);
// at the flush's first ocean draw, where vanilla's update comes: the update made at the flush's start goes into the
// simulation's grid (the draw's own update then finds this frame's done)
void PublishOcean() {
    size_t bytes = 0;
    if (OceanVector(g.oceanAnswerOf, &bytes) == g.oceanPublishTo && bytes == g.oceanNew.size()) {
        std::memcpy(g.oceanPublishTo, g.oceanNew.data(), bytes);
        ++g.oceanPublishes;
    } else {
        ++g.oceanPublishFailed;
    }
    g.oceanPublishTo = nullptr;
    g.oceanPublished = true;
    if (g.marks && g.window) SetEvent(g.marks);
}
uint64_t OceanDrawHook(void* re, uint64_t shader, uint64_t pass) {
    if (g.oceanPublishTo && re == g.oceanAnswerOf && OnFlushThread()) PublishOcean();
    const uint64_t v = reinterpret_cast<DrawFn>(g.oceanDraw)(re, shader, pass);
    if (OnFlushThread()) g.oceanDrawSeen = true;
    if (!g.oceanDrawn && OnFlushThread()) {
        if (g.handOffAt) {  // (beside the main thread: the first draw's delay after the hand-off)
            ++g.oceanDrawWindows;
            g.oceanDrawCycles += __rdtsc() - g.handOffAt;
            g.handOffAt = 0;
        }
        g.oceanDrawn = true;  // (after the simulation's update, which the draw makes first)
        if (g.marks && g.window) SetEvent(g.marks);
    }
    return v;
}
// EF_AddEf, observed: the ocean's element added to the lists by the traversal, so the flush will draw it (its own
// scene passes add theirs on its thread, and draw them in it)
void AddEfHook(void* r, void* re, void* shaderItem, void* obj, int list, int aw) {
    RenderQueue_NoteAddEf(re);
    reinterpret_cast<AddEfFn>(g.addEf)(r, re, shaderItem, obj, list, aw);
}
// beside the flush: until it has drawn the ocean once, or ended
void WaitOcean() {
    if (g.oceanDrawn) return;
    const uint64_t c0 = __rdtsc();
    WaitMark(g.oceanDrawn);
    ++g.oceanWaits;
    g.oceanWaitCycles += __rdtsc() - c0;
    if (!g.oceanDrawn) ++g.oceanEnded;
}
// the grid of an ocean, through the fields its code reads (null: none)
const uint8_t* OceanVector(char* re, size_t* bytes) {
    char* const holder = *reinterpret_cast<char**>(re + kOceanSim);
    char* const sim = holder ? *reinterpret_cast<char**>(holder) : nullptr;
    if (!sim) return nullptr;
    const uint8_t* const begin = *reinterpret_cast<const uint8_t* const*>(sim + kOceanBegin);
    const uint8_t* const end = *reinterpret_cast<const uint8_t* const*>(sim + kOceanEnd);
    if (!begin || end <= begin || size_t(end - begin) > kOceanMaxBytes) return nullptr;
    *bytes = size_t(end - begin);
    return begin;
}
// at the window's first answer: the grid answered (the simulation's, or the update's copy), to compare with the grid at
// the join
void CopyOcean(void* re, const uint8_t* answer) {
    if (g.oceanOf) return;
    size_t bytes = 0;
    const uint8_t* const grid = OceanVector(static_cast<char*>(re), &bytes);
    if (!grid) return;
    const uint8_t* const from = answer && (answer == grid || g.oceanNew.size() == bytes) ? answer : grid;
    g.oceanCopy.assign(from, from + bytes);
    g.oceanGrid = grid;
    g.oceanOf = static_cast<char*>(re);
}
void* OceanGridHook(void* re) {
    if (!FlushThread_Beside()) return Fn<PtrFn>(kOceanGrid)(re);
    WaitOcean();
    void* const v = Fn<PtrFn>(kOceanGrid)(re);
    // (the update made at the flush's start: its copy, until the flush's first ocean draw puts it in the grid)
    const uint8_t* const answer = g.oceanAnswer && re == g.oceanAnswerOf ? g.oceanAnswer : static_cast<uint8_t*>(v);
    CopyOcean(re, answer);
    Call& c = Record(kOceanGrid, re);
    c.out[0] = uint32_t(uintptr_t(v));  // (the grid's place, checked at the join; what it holds by CompareOcean)
    c.out[1] = uint32_t(uintptr_t(v) >> 32);
    return const_cast<uint8_t*>(answer);
}
float* OceanPointHook(void* re, float* out, float x, float y) {
    if (!FlushThread_Beside()) return Fn<OceanPointFn>(kOceanPoint)(re, out, x, y);
    WaitOcean();
    if (!g.oceanPublished && g.oceanAnswer) {  // (computed from the grid: once the update is in it)
        ++g.oceanPointWaits;
        WaitMark(g.oceanPublished);
    }
    Fn<OceanPointFn>(kOceanPoint)(re, out, x, y);
    CopyOcean(re, nullptr);
    Call& c = Record(kOceanPoint, re);
    c.a[0] = B(x);
    c.a[1] = B(y);
    for (int i = 0; i < 3; ++i) c.out[i] = uint32_t(B(out[i]));
    return out;
}
// at the join: the grid the window's answers saw, against the grid the flush left
void CompareOcean() {
    if (!g.oceanOf) return;
    size_t bytes = 0;
    const uint8_t* const grid = OceanVector(g.oceanOf, &bytes);
    ++g.oceanCompared;
    if (grid != g.oceanGrid || bytes != g.oceanCopy.size() || std::memcmp(grid, g.oceanCopy.data(), bytes) != 0)
        ++g.oceanChanged;
    g.oceanOf = nullptr;
}

// the flush's SetCamera calls, counted (the diagnostic of the camera it leaves); the gate's thunk keeps the caller's
// return address
// SetCamera beside the flush is taken: the camera copied, GetCamera answers it from then on (as the renderer's camera
// would be), and ProjectToScreen joins (SetCamera sets the view and projection matrices too; not the viewport)
// the camera-info method (detoured): for the block SetCamera runs on beside the flush, nothing (its matrices are the
// renderer's other state)
void CameraInfoHook(void* r) {
    if (r == g.camera.block.data()) return;
    reinterpret_cast<CameraInfoFn>(g.cameraInfo)(r);
}
void SetCameraHook(void* r, const void* camera) {
    if (FlushThread_Beside()) {
        if (g.cameraOf == r && g.cameraVersions < kCameraVersions) {
            const unsigned v = g.cameraVersions++;
            std::memcpy(g_cameraVersion[v], camera, kCameraBytes);
            std::memcpy(g_cameraAnswer, camera, kCameraBytes);
            Call& c = Record(kSetCamera, r);
            c.a[0] = v;
            // its matrices: SetCamera itself on the block (not within a taken 2D mode, whose matrices are pushed)
            Camera& k = g.camera;
            if (g.cameraInfo && k.valid && k.renderer == r && g.depth2d == 0) {
                const uint64_t c0 = __rdtsc();
                reinterpret_cast<SetCameraFn>(g.setCamera)(k.block.data(), camera);
                g.standInCycles += __rdtsc() - c0;
                ++g.standInSets;
                std::memcpy(g_matrixVersion[v][0], k.view, kMatrixBytes);
                std::memcpy(g_matrixVersion[v][1], k.proj, kMatrixBytes);
                k.matrices = true;
                k.version = v + 1;
                c.a[1] = 1;  // (checked at the join, after it is made)
            } else {
                if (g.depth2d != 0) ++g.standIn2d;
                k.matrices = false;
            }
            return;
        }
        ++g.setCameraJoins;
        FlushThread_JoinNow();  // (no copy of this renderer's camera, or no room: the flush ends here)
    }
    reinterpret_cast<SetCameraFn>(g.setCamera)(r, camera);
    if (g.window && OnFlushThread()) {
        ++g.windowSets;
        g.lastSetAt = __rdtsc();
        g.lastSetBy = _ReturnAddress();
    }
}
void* GetCameraHook(void* r) {
    if (!FlushThread_Beside()) return Fn<CameraFn>(kGetCamera)(r);
    if (g.cameraOf != r) {
        ++g.cameraJoins;
        FlushThread_JoinNow();  // (no copy of this renderer's camera: the flush ends here, and the call is made)
        return Fn<CameraFn>(kGetCamera)(r);
    }
    // (checked at the join: the members of the camera answered, the hand-off's or a taken SetCamera's, against the
    // camera's)
    Record(kGetCamera, r).a[0] = g.cameraVersions;
    return g_cameraAnswer;
}
template <Kind K>
void MatrixHook(void* r, float* out) {
    if (!FlushThread_Beside()) return Fn<MatrixFn>(K)(r, out);
    if (!g.camera.valid || !g.camera.matrices || g.camera.renderer != r || g.depth2d != 0) {
        ++g.matrixJoins;
        FlushThread_JoinNow();  // (other matrices than the hand-off's: the flush ends here, and the call is made)
        return Fn<MatrixFn>(K)(r, out);
    }
    std::memcpy(out, K == kGetModelView ? g.camera.view : g.camera.proj, kMatrixBytes);
    Record(K, r).a[0] = g.camera.version;  // (checked at the join: the renderer's matrix then against the answer)
}
// at the join: the camera the flush left (the worker's copy of it, if it made the frame's end), against the
// hand-off's, by 16-byte parts
void CompareFlushCamera() {
    if (!g.cameraOf) return;
    const uint8_t* const now = g.left.valid && g.left.camera ? g_cameraLeft
                                                              : reinterpret_cast<const uint8_t*>(g.cameraOf + g.cameraAt);
    g.cameraOf = nullptr;
    ++g.camerasCompared;
    char parts[160] = "";
    size_t used = 0;
    for (size_t p = 0; p < kCameraParts; ++p) {
        const size_t at = p * 16, n = (std::min)(size_t(16), kCameraBytes - at);
        if (std::memcmp(now + at, g_cameraHandOff + at, n) == 0) continue;
        ++g.cameraParts[p];
        const int w = snprintf(parts + used, sizeof parts - used, "%s+0x%zX", used ? " " : "", at);
        if (w > 0 && size_t(w) < sizeof parts - used) used += size_t(w);
    }
    if (!used) {
        g.setsSame += g.windowSets;
        return;
    }
    ++g.camerasDiffer;
    if (std::memcmp(now, g_cameraHandOff, kCameraMembers) != 0) ++g.camerasMembers;  // (more than the padding)
    g.setsDiffer += g.windowSets;
    if (g.windowSets) {
        ++g.lastSetsDiffer;
        g.lastSetCyclesDiffer += g.lastSetAt - g.windowAt;
    }
    if (g.loggedCameras < kLoggedCameras) {
        ++g.loggedCameras;
        char where[128] = "-";
        if (g.windowSets) DescribeAddress(g.lastSetBy, where, sizeof where);
        Log("render queue: the flush left another camera than the hand-off's (its parts differing: %s), after %u "
            "SetCamera calls in it, the last %.2f ms after the hand-off, called from %s",
            parts, g.windowSets, g.windowSets ? double(g.lastSetAt - g.windowAt) / 3.0e6 : 0.0, where);
    }
}

// the parameter of a name, as the manager's lookup finds it, without its cache (null: none)
char* PostParam(const char* name) {
    char* const mgr = g.postMgr;
    uint32_t crc = 0;
    if (name) {
        const uint32_t* const table = reinterpret_cast<const uint32_t*>(mgr + kPostCrcTable);
        crc = ~0u;
        for (const uint8_t* s = reinterpret_cast<const uint8_t*>(name); *s; ++s) {
            const uint8_t ch = uint8_t(*s - 'a') < 26 ? uint8_t(*s - 0x20) : *s;
            crc = table[(crc ^ ch) & 0xFF] ^ (crc >> 8);
        }
        crc = ~crc;
    }
    char* const head = mgr + kPostMap;
    char* best = head;
    for (char* n = *reinterpret_cast<char**>(head + 8); n;) {  // (the map's root)
        if (*reinterpret_cast<const uint32_t*>(n + kPostNodeKey) < crc) {
            n = *reinterpret_cast<char**>(n + kPostNodeRight);
        } else {
            best = n;
            n = *reinterpret_cast<char**>(n + kPostNodeLeft);
        }
    }
    if (best == head || crc < *reinterpret_cast<const uint32_t*>(best + kPostNodeKey)) return nullptr;
    return *reinterpret_cast<char**>(best + kPostNodeValue);
}
// a parameter whose getter only reads its value: a CParamFloat that doesn't blend, a CParamBool, a CParamInt
enum ParamKind : uint8_t { kNotPlain, kPlainFloat, kPlainBool, kPlainInt };
ParamKind PlainParam(char* param) {
    void* const* const vt = *reinterpret_cast<void* const* const*>(param);
    const void* const get = vt ? CallTrace_Original(vt[kSlotParamGet]) : nullptr;
    if (!get) return kNotPlain;
    if (get == g.paramFloatGet)
        return *reinterpret_cast<const uint8_t*>(param + kParamBlends) == 0 || *g.paramBlending == 0 ? kPlainFloat
                                                                                                    : kNotPlain;
    if (get == g.paramBoolGet) return kPlainBool;
    if (get == g.paramIntGet) return kPlainInt;
    return kNotPlain;
}
// what its getter answers: its value now, as a float
float PlainNow(ParamKind kind, const char* param) {
    if (kind == kPlainBool) return float(*reinterpret_cast<const uint8_t*>(param + kParamValue));
    if (kind == kPlainInt) return float(*reinterpret_cast<const int32_t*>(param + kParamValue));
    return *reinterpret_cast<const float*>(param + kParamValue);
}
// ... and once its setter has been given f
float PlainSet(ParamKind kind, float f) {
    if (kind == kPlainBool) return (f == *g.paramBoolFalse || f != f) ? 0.0f : 1.0f;
    if (kind == kPlainInt) return float(_mm_cvttss_si32(_mm_set_ss(f)));
    return f;
}
// a vtable's slot pointed at fn (its page writable for the one aligned store)
bool PatchSlot(char** slot, void* fn) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return false;
    *reinterpret_cast<void* volatile*>(slot) = fn;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return true;
}
// a post effect getter's join beside the flush, the first logged (the parameter: its getter, whether it blends; the
// caller)
void LogPostGetJoin(const char* what, const char* name, char* param, unsigned slot, size_t blendsAt, void* caller) {
    if (g.loggedPostJoins >= kLoggedPostJoins) return;
    ++g.loggedPostJoins;
    char where[128], getter[128] = "-";
    void* const* const vt = param ? *reinterpret_cast<void* const* const*>(param) : nullptr;
    if (vt) DescribeAddress(CallTrace_Original(vt[slot]), getter, sizeof getter);
    Log("render queue: %s(\"%.64s\") joins the flush (a blending or other parameter: %s; its getter %s, blends %u, "
        "blending %u), called from %s",
        what, name ? name : "", param ? "found" : "its name too long to copy", getter,
        param ? unsigned(*reinterpret_cast<const uint8_t*>(param + blendsAt)) : 0u,
        g.paramBlending ? unsigned(*g.paramBlending) : 0u, DescribeAddress(caller, where, sizeof where));
}
void PostGetHook(void* p, const char* name, float* out) {
    if (!FlushThread_Beside()) return Fn<PostGetFn>(kPostGet)(p, name, out);
    char* const param = PostParam(name);
    const ParamKind kind = param ? PlainParam(param) : kNotPlain;
    uint64_t at = 0;
    if ((param && kind == kNotPlain) || !CopyString(name, at)) {
        ++g.postGetJoins;
        LogPostGetJoin("GetPostEffectParam", name, param, kSlotParamGet, kParamBlends, _ReturnAddress());
        FlushThread_JoinNow();  // (a blending or other parameter: the flush ends here, and the call is made)
        return Fn<PostGetFn>(kPostGet)(p, name, out);
    }
    if (!param) {  // (none of that name: the renderer leaves the answer as it was)
        ++g.postGetMissing;
        Record(kPostGet, p).a[0] = at;
        return;
    }
    float v = PlainNow(kind, param);
    for (size_t i = g.calls.size(); i-- > 0;) {  // (the window's last setter of that name, taken)
        const Call& s = g.calls[i];
        const char* const set = s.kind == kPostSet ? Str(s.a[0]) : nullptr;
        if (set && name && _stricmp(set, name) == 0) {
            v = PlainSet(kind, F(s.a[1]));
            ++g.postGetTaken;
            break;
        }
    }
    if (kind != kPlainFloat) ++g.postGetOther;
    *out = v;
    Call& c = Record(kPostGet, p);
    c.a[0] = at;
    c.out[0] = uint32_t(B(v));
    c.out[1] = 1;
}
// a CParamVec4 that doesn't blend: its getter only copies its four floats
bool PlainVec4(char* param) {
    void* const* const vt = *reinterpret_cast<void* const* const*>(param);
    return vt && CallTrace_Original(vt[kSlotParamGetVec4]) == g.paramVec4Get &&
           (*reinterpret_cast<const uint8_t*>(param + kParamVec4Blends) == 0 || *g.paramBlending == 0);
}
void PostGetVec4Hook(void* p, const char* name, float* out) {
    if (!FlushThread_Beside()) return Fn<PostGetVec4Fn>(kPostGetVec4)(p, name, out);
    char* const param = PostParam(name);
    uint64_t at = 0;
    if ((param && !PlainVec4(param)) || !CopyString(name, at)) {
        ++g.postGetJoins;
        LogPostGetJoin("GetPostEffectParamVec4", name, param, kSlotParamGetVec4, kParamVec4Blends, _ReturnAddress());
        FlushThread_JoinNow();  // (a blending or other parameter: the flush ends here, and the call is made)
        return Fn<PostGetVec4Fn>(kPostGetVec4)(p, name, out);
    }
    if (!param) {  // (none of that name: the renderer leaves the answer as it was)
        ++g.postGetMissing;
        Record(kPostGetVec4, p).a[0] = at;
        return;
    }
    float v[4];
    std::memcpy(v, param + kParamValue, sizeof v);
    for (size_t i = g.calls.size(); i-- > 0;) {  // (the window's last Vec4 setter of that name, taken)
        const Call& s = g.calls[i];
        const char* const set = s.kind == kPostSetVec4 ? Str(s.a[0]) : nullptr;
        if (set && name && _stricmp(set, name) == 0) {
            for (int k = 0; k < 4; ++k) v[k] = F(s.a[1 + k]);
            ++g.postGetTaken;
            break;
        }
    }
    std::memcpy(out, v, sizeof v);
    Call& c = Record(kPostGetVec4, p);
    c.a[0] = at;
    c.a[1] = 1;  // (found)
    for (int k = 0; k < 4; ++k) c.out[k] = uint32_t(B(v[k]));
}

void SetViewportHook(void* r, int x, int y, int w, int h) {
    if (!FlushThread_Beside()) return Fn<SetViewportFn>(kSetViewport)(r, x, y, w, h);
    Call& c = Record(kSetViewport, r);
    c.a[0] = uint64_t(int64_t(x));
    c.a[1] = uint64_t(int64_t(y));
    c.a[2] = uint64_t(int64_t(w));
    c.a[3] = uint64_t(int64_t(h));
    if (g.camera.valid && g.camera.renderer == r) {  // (what GetViewport and ProjectToScreen answer from now on)
        const int v[4] = {x, y, w, h};
        std::memcpy(g.camera.block.data() + kViewport, v, kViewportBytes);
    }
}

template <unsigned S>
void* ResRefHook(void* res) {
    uint8_t* const real = static_cast<uint8_t*>(reinterpret_cast<ResRefFn>(g.resFns[S])(res));
    if (!FlushThread_Beside()) return real;
    if (g.resUsed == kMaxResRefs) {
        ++g.resJoins;
        FlushThread_JoinNow();  // (the room full: the flush ends here, and the reference is the real one)
        return real;
    }
    const uint32_t n = kResSlots[S].bytes;
    ResRef& f = g.resRefs[g.resUsed];
    f.real = real;
    f.bytes = n;
    std::memcpy(f.value, real, n);
    for (size_t i = 0; i < g.resUsed; ++i) {  // (as the window's earlier answers for the same bytes left them)
        const ResRef& e = g.resRefs[i];
        const uint8_t* const lo = (std::max)(static_cast<const uint8_t*>(e.real), static_cast<const uint8_t*>(real));
        const uint8_t* const hi = (std::min)(static_cast<const uint8_t*>(e.real + e.bytes),
                                             static_cast<const uint8_t*>(real + n));
        if (lo < hi) std::memcpy(f.value + (lo - real), e.value + (lo - e.real), size_t(hi - lo));
    }
    std::memcpy(f.before, f.value, n);
    Call& c = Record(kResRef, res);
    c.a[0] = g.resUsed++;
    std::memcpy(c.out, f.before, n);
    return f.value;
}
template <size_t... S>
std::array<ResRefFn, sizeof...(S)> MakeResHooks(std::index_sequence<S...>) {
    return {{&ResRefHook<unsigned(S)>...}};
}
const std::array<ResRefFn, std::size(kResSlots)> kResHooks =
    MakeResHooks(std::make_index_sequence<std::size(kResSlots)>{});

uint16_t* IndicesHook(void* mesh, int* count) {
    uint16_t* const p = Fn<IndicesFn>(kMeshIndices)(mesh, count);
    if (FlushThread_Beside()) {
        Call& c = Record(kMeshIndices, mesh);
        c.a[0] = count != nullptr;
        c.out[0] = uint32_t(uintptr_t(p));
        c.out[1] = uint32_t(uintptr_t(p) >> 32);
        c.out[2] = count ? uint32_t(*count) : 0;
    }
    return p;
}

uint8_t* PosPtrHook(void* mesh, int* stride, int id, bool write) {
    if (!FlushThread_Beside()) return Fn<PosPtrFn>(kMeshPosPtr)(mesh, stride, id, write);
    char* const m = static_cast<char*>(mesh);
    char* const container = *reinterpret_cast<char**>(m + kMeshContainer);
    char* const owner = container ? container : m;
    void* const* const stream = *reinterpret_cast<void* const* const*>(owner + kMeshSysStream);
    if (write || !stream || !*stream) {
        ++g.posJoins;
        FlushThread_JoinNow();  // (a write, or no system stream: the flush ends here, and the call is made)
        return Fn<PosPtrFn>(kMeshPosPtr)(mesh, stride, id, write);
    }
    uint8_t* const p = Fn<PosPtrFn>(kMeshPosPtr)(mesh, stride, id, false);
    Call& c = Record(kMeshPosPtr, mesh);
    c.a[0] = uint64_t(int64_t(id));
    c.out[0] = uint32_t(uintptr_t(p));
    c.out[1] = uint32_t(uintptr_t(p) >> 32);
    c.out[2] = uint32_t(*stride);
    return p;
}

// the bytes of a texture's first level in a format, as UpdateTextureRegion reads them (0: a format not known here)
size_t TextureBytes(int w, int h, int format) {
    if (w <= 0 || h <= 0) return 0;
    const size_t blocks = size_t((w + 3) / 4) * size_t((h + 3) / 4), pixels = size_t(w) * size_t(h);
    switch (format) {  // (ETEX_Format)
        case 22: return blocks * 8;                                  // DXT1
        case 23: case 24: case 25: return blocks * 16;               // DXT3, DXT5, 3DC
        case 2: case 3: return pixels * 4;                           // A8R8G8B8, X8R8G8B8
        case 1: return pixels * 3;                                   // R8G8B8
        case 4: case 6: return pixels;                               // A8, L8
        case 5: case 7: case 8: case 9: case 10: return pixels * 2;  // A8L8, A4R4G4B4, R5G6B5, R5G5B5, V8U8
        default: return 0;
    }
}
unsigned DownLoadHook(void* r, const uint8_t* data, int w, int h, int src, int dst, int mips, bool repeat,
                      int filter, int id, char* name, int flags) {
    if (!FlushThread_Beside())
        return Fn<DownLoadFn>(kDownLoad)(r, data, w, h, src, dst, mips, repeat, filter, id, name, flags);
    const size_t bytes = id > 0 && data ? TextureBytes(w, h, src) : 0;
    uint64_t nameAt = 0;
    if (!bytes || g.uploadUsed + bytes > kUploadBytes || !CopyString(name, nameAt)) {
        ++g.uploadJoins;
        FlushThread_JoinNow();  // (a new texture, or its data not copied: the flush ends here, and the call is made)
        return Fn<DownLoadFn>(kDownLoad)(r, data, w, h, src, dst, mips, repeat, filter, id, name, flags);
    }
    if (g.loggedUploads < kLoggedUploads) {
        ++g.loggedUploads;
        char where[128];
        Log("render queue: DownLoadToVideoMemory(%dx%d, format %d to %d, %d mips, Id %d, flags 0x%X) taken beside the "
            "flush, %zu bytes copied, called from %s",
            w, h, src, dst, mips, id, unsigned(flags), bytes, DescribeAddress(_ReturnAddress(), where, sizeof where));
    }
    std::memcpy(g.upload.data() + g.uploadUsed, data, bytes);
    Call& c = Record(kDownLoad, r);
    c.a[0] = g.uploadUsed;
    c.a[1] = uint64_t(int64_t(w));
    c.a[2] = uint64_t(int64_t(h));
    c.a[3] = uint64_t(int64_t(src));
    c.a[4] = uint64_t(int64_t(dst));
    c.a[5] = uint64_t(int64_t(mips));
    c.a[6] = repeat;
    c.a[7] = uint64_t(int64_t(filter));
    c.a[8] = uint64_t(int64_t(id));
    c.a[9] = nameAt;
    c.a[10] = uint64_t(int64_t(flags));
    c.out[0] = uint32_t(id);
    g.uploadUsed += (bytes + 15) & ~size_t(15);
    g.uploadBytes += bytes;
    return unsigned(id);
}

template <unsigned C, unsigned S>
uint64_t GetterHook(void* obj) {
    const uint64_t v = reinterpret_cast<GetterFn>(g.getterFns[C][S])(obj);
    if (FlushThread_Beside()) {
        ++g.getterCounts[C][S];
        Call& c = Record(kFieldGetter, obj);
        c.a[0] = C;
        c.a[1] = S;
        const uint64_t set = v & g.getterMask[C][S];
        c.out[0] = uint32_t(set);
        c.out[1] = uint32_t(set >> 32);
    }
    return v;
}
template <unsigned C, size_t... S>
std::array<GetterFn, sizeof...(S)> MakeGetterHooks(std::index_sequence<S...>) {
    return {{&GetterHook<C, unsigned(S)>...}};
}
const std::array<GetterFn, kGetterSlots> kGetterHooks[kGetterClasses] = {
    MakeGetterHooks<kGetterMesh>(std::make_index_sequence<kGetterSlots>{}),
    MakeGetterHooks<kGetterShader>(std::make_index_sequence<kGetterSlots>{}),
    MakeGetterHooks<kGetterTexture>(std::make_index_sequence<kGetterSlots>{}),
    MakeGetterHooks<kGetterResources>(std::make_index_sequence<kGetterSlots>{})};

// an IMaterial's AddRef or Release (slot 1 or 2), if there is one
void MaterialRef(void* m, unsigned slot) {
    if (m) (*reinterpret_cast<void (**)(void*)>(*static_cast<char**>(m) + 8 * slot))(m);
}
void DefaultMaterialsHook(void* r, void* def, void* terrain) {
    if (!FlushThread_Beside()) return Fn<DefaultMaterialsFn>(kSetDefaultMaterials)(r, def, terrain);
    MaterialRef(def, kSlotMaterialAddRef);  // (kept alive until the call is made)
    MaterialRef(terrain, kSlotMaterialAddRef);
    Call& c = Record(kSetDefaultMaterials, r);
    c.a[0] = uint64_t(def);
    c.a[1] = uint64_t(terrain);
}
// a method that only returns a field of its object (rcx): mov rax/eax/al, [rcx+d] or movzx eax, byte [rcx+d]; ret.
// The bits of rax it sets (mov eax and movzx clear the upper ones), or 0 if it isn't one.
uint64_t FieldGetter(char* base, const char* fn) {
    if (!IsInCode(base, fn, 8)) return 0;
    const auto* b = reinterpret_cast<const uint8_t*>(fn);
    if (b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x81) return b[7] == 0xC3 ? ~0ull : 0;
    if (b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x41) return b[4] == 0xC3 ? ~0ull : 0;
    if (b[0] == 0x8B && b[1] == 0x81) return b[6] == 0xC3 ? ~0ull : 0;
    if (b[0] == 0x8B && b[1] == 0x41) return b[3] == 0xC3 ? ~0ull : 0;
    if (b[0] == 0x0F && b[1] == 0xB6 && b[2] == 0x81) return b[7] == 0xC3 ? ~0ull : 0;
    if (b[0] == 0x0F && b[1] == 0xB6 && b[2] == 0x41) return b[4] == 0xC3 ? ~0ull : 0;
    if (b[0] == 0x8A && b[1] == 0x81) return b[6] == 0xC3 ? 0xFFull : 0;
    if (b[0] == 0x8A && b[1] == 0x41) return b[3] == 0xC3 ? 0xFFull : 0;
    return 0;
}

// --- the post effects' parameters set (CREPostProcess, through the 3D engine) -------------------------------------
// a name or string argument into the window's copies (false: too long to copy)
bool CopyString(const char* s, uint64_t& at) {
    if (!s) {
        at = kNoString;
        return true;
    }
    const size_t n = strnlen(s, kMaxString);
    // (within what is reserved: the frame's end on the worker reads its strings while this thread adds more)
    if (n == kMaxString || g.strings.size() + n + 1 > g.strings.capacity()) return false;
    at = g.strings.size();
    g.strings.insert(g.strings.end(), s, s + n + 1);
    return true;
}
const char* Str(uint64_t at) { return at == kNoString ? nullptr : g.strings.data() + at; }

// the first taken setters' callers, logged
void LogCaller(Kind k, void* returnAddress) {
    if (g.loggedCallers >= 4) return;
    ++g.loggedCallers;
    char where[128];
    Log("render queue: %s taken beside the flush, called from %s (it answers %d, its last made call's answer)",
        kDefs[k].name, DescribeAddress(returnAddress, where, sizeof where), g.postReturn[k - kPostSet]);
}

int PostSetHook(void* p, const char* name, float value, uint64_t extra) {
    uint64_t at = 0;
    if (FlushThread_Beside() && !CopyString(name, at)) {
        ++g.postJoins;
        FlushThread_JoinNow();  // (its name too long to copy: the flush ends here, and the call is made)
    }
    if (!FlushThread_Beside()) return g.postReturn[0] = Fn<PostSetFn>(kPostSet)(p, name, value, extra);
    LogCaller(kPostSet, _ReturnAddress());
    Call& c = Record(kPostSet, p);
    c.a[0] = at;
    c.a[1] = B(value);
    c.a[2] = extra;
    return g.postReturn[0];
}
int PostSetVec4Hook(void* p, const char* name, const float* v, uint64_t extra) {
    uint64_t at = 0;
    if (FlushThread_Beside() && (!v || !CopyString(name, at))) {
        ++g.postJoins;
        FlushThread_JoinNow();
    }
    if (!FlushThread_Beside()) return g.postReturn[1] = Fn<PostSetPtrFn>(kPostSetVec4)(p, name, v, extra);
    LogCaller(kPostSetVec4, _ReturnAddress());
    Call& c = Record(kPostSetVec4, p);
    c.a[0] = at;
    for (int i = 0; i < 4; ++i) c.a[1 + i] = B(v[i]);
    c.a[5] = extra;
    return g.postReturn[1];
}
int PostSetStringHook(void* p, const char* name, const char* arg, uint64_t extra) {
    uint64_t at = 0, argAt = 0;
    if (FlushThread_Beside() && (!CopyString(name, at) || !CopyString(arg, argAt))) {
        ++g.postJoins;
        FlushThread_JoinNow();
    }
    if (!FlushThread_Beside()) return g.postReturn[2] = Fn<PostSetPtrFn>(kPostSetString)(p, name, arg, extra);
    LogCaller(kPostSetString, _ReturnAddress());
    Call& c = Record(kPostSetString, p);
    c.a[0] = at;
    c.a[1] = argAt;
    c.a[2] = extra;
    return g.postReturn[2];
}

void* EF_QueryHook(void* r, int query, intptr_t param) {
    const bool setting = QuerySetting(query);
    if (FlushThread_Beside() && !setting && !(query == kQueryDrawNearFov && param)) {
        if (g.loggedQueries < kLoggedQueries) {
            ++g.loggedQueries;
            char where[128];
            Log("render queue: EF_Query(%d) joins the flush, called from %s", query,
                DescribeAddress(_ReturnAddress(), where, sizeof where));
        }
        ++g.queryJoins;
        FlushThread_JoinNow();
    }
    if (!FlushThread_Beside() || setting) {
        void* const v = Fn<QueryFn>(kEF_Query)(r, query, param);
        if (query == kQueryDrawNearFov) g.nearFovReturn = v;
        if (setting && FlushThread_Beside()) {  // (answered: checked at the join)
            Call& c = Record(kEF_QuerySetting, r);
            c.a[0] = uint64_t(query);
            c.a[1] = uint64_t(param);
            c.out[0] = uint32_t(uintptr_t(v));
            c.out[1] = uint32_t(uintptr_t(v) >> 32);
        }
        return v;
    }
    Call& c = Record(kEF_Query, r);
    c.a[0] = uint64_t(query);
    c.a[1] = B(*reinterpret_cast<const float*>(param));
    return g.nearFovReturn;
}

// ProjectToScreen's code reads the fields the camera copies (their displacements in it), and its stacks' top through
// their first word (calls to a `mov rax, [rcx]; ret`)
bool CheckCameraLayout(char* base, const char* fn) {
    if (!IsInCode(base, fn, 0x180)) return false;
    constexpr size_t kScan = 0x180;
    const uint32_t want[] = {uint32_t(kViewport), uint32_t(kViewport + 8), uint32_t(kViewStack),
                             uint32_t(kProjStack)};
    for (uint32_t d : want) {
        bool found = false;
        for (size_t i = 0; i + 4 <= kScan && !found; ++i) found = std::memcmp(fn + i, &d, 4) == 0;
        if (!found) return false;
    }
    int tops = 0;
    for (size_t i = 0; i + 5 <= kScan; ++i) {
        if (uint8_t(fn[i]) != 0xE8) continue;
        int32_t rel;
        std::memcpy(&rel, fn + i + 1, 4);
        const uint8_t* t = reinterpret_cast<const uint8_t*>(fn + i + 5 + rel);
        if (IsInCode(base, reinterpret_cast<const char*>(t), 4) && t[0] == 0x48 && t[1] == 0x8B && t[2] == 0x01 &&
            t[3] == 0xC3)
            ++tops;
    }
    return tops >= 2;
}

void* const kHooks[] = {
    reinterpret_cast<void*>(&DeleteRenderMeshHook), reinterpret_cast<void*>(&EnableFogHook),
    reinterpret_cast<void*>(&EnableTMUHook),        reinterpret_cast<void*>(&SelectTMUHook),
    reinterpret_cast<void*>(&Set2DModeHook),        reinterpret_cast<void*>(&SetCullModeHook),
    reinterpret_cast<void*>(&SetColorOpHook),       reinterpret_cast<void*>(&SetStateHook),
    reinterpret_cast<void*>(&Draw2dImageHook),
    reinterpret_cast<void*>(&Ints2Hook<kFontSetTexture78>),
    reinterpret_cast<void*>(&Ints2Hook<kFontSetTexture79>),
    reinterpret_cast<void*>(&Ints2Hook<kFontSetRenderingState>),
    reinterpret_cast<void*>(&Ints2Hook<kFontSetBlending>),
    reinterpret_cast<void*>(&Ints0Hook<kFontRestoreRenderingState>),
    reinterpret_cast<void*>(&Vf214Hook),
    reinterpret_cast<void*>(&AuxFlushHook),
    reinterpret_cast<void*>(&GetDynVBPtrHook),
    reinterpret_cast<void*>(&DrawDynVBHook),
    reinterpret_cast<void*>(&Ints0Hook<kRenderDebug>),
    reinterpret_cast<void*>(&PostSetHook),
    reinterpret_cast<void*>(&PostSetVec4Hook),
    reinterpret_cast<void*>(&PostSetStringHook),
    reinterpret_cast<void*>(&EF_QueryHook),
    reinterpret_cast<void*>(&Ints0Hook<kFlushTextMessages>),
    reinterpret_cast<void*>(&Ints0Hook<kEndFrame>),
    reinterpret_cast<void*>(&Draw2dLabelHook),
    reinterpret_cast<void*>(&SysEventHook),
    reinterpret_cast<void*>(&Ints0Hook<kBeginFrame>),
    reinterpret_cast<void*>(&SetClearColorHook),
    reinterpret_cast<void*>(&AuxSetFlagsHook),
    reinterpret_cast<void*>(&Ints0Hook<kMeshAddRef>),
    reinterpret_cast<void*>(&CountedAddRefHook<kShaderAddRef>),
    reinterpret_cast<void*>(&CountedAddRefHook<kTextureAddRef>),
    reinterpret_cast<void*>(&DownLoadHook),
    reinterpret_cast<void*>(&SetViewportHook),
    reinterpret_cast<void*>(&DefaultMaterialsHook),
    reinterpret_cast<void*>(&SetCameraHook),
    reinterpret_cast<void*>(&ShaderReleaseHook),
    reinterpret_cast<void*>(&SetSkyLightHook),
    nullptr,  // (the Flash player's Render: FlashRenderHook, in CrySystem's vtable)
    reinterpret_cast<void*>(&SetTerrainAOHook),
    reinterpret_cast<void*>(&Ints2Hook<kSFConfigMask>),
    reinterpret_cast<void*>(&SFTriListHook),
    reinterpret_cast<void*>(&SFLineStripHook),
    reinterpret_cast<void*>(&SFGlyphHook),
    reinterpret_cast<void*>(&Ints0Hook<kSFFlush>),
    reinterpret_cast<void*>(&ClearBufferHook),
    reinterpret_cast<void*>(&IntHook<kGetWidth>),
    reinterpret_cast<void*>(&IntHook<kGetHeight>),
    reinterpret_cast<void*>(&ScaleHook<kScaleCoordX>),
    reinterpret_cast<void*>(&ScaleHook<kScaleCoordY>),
    reinterpret_cast<void*>(&GetFrameIDHook),
    reinterpret_cast<void*>(&IntHook<kGetFeatures>),
    reinterpret_cast<void*>(&PtrHook<kGetIRenderAuxGeom>),
    reinterpret_cast<void*>(&IntHook<kGetTextureID>),
    reinterpret_cast<void*>(&ProjectToScreenHook),
    reinterpret_cast<void*>(&GetViewportHook),
    reinterpret_cast<void*>(&PtrHook<kGetHWND>),
    reinterpret_cast<void*>(&IntHook<kGetRenderType>),
    reinterpret_cast<void*>(&IntIntHook<kGetShaderQuality>),
    reinterpret_cast<void*>(&EF_QueryHook),
    reinterpret_cast<void*>(&MultiGPUHook),
    reinterpret_cast<void*>(&AuxGetFlagsHook),
    nullptr,  // (the field getters: kGetterHooks, per class and slot)
    reinterpret_cast<void*>(&OceanGridHook),
    reinterpret_cast<void*>(&OceanPointHook),
    reinterpret_cast<void*>(&PostGetHook),
    reinterpret_cast<void*>(&GetCameraHook),
    reinterpret_cast<void*>(&PostGetVec4Hook),
    nullptr,  // (the shader resources' references: kResHooks, per slot)
    reinterpret_cast<void*>(&PosPtrHook),
    reinterpret_cast<void*>(&IndicesHook),
    reinterpret_cast<void*>(&MatrixHook<kGetModelView>),
    reinterpret_cast<void*>(&MatrixHook<kGetProjection>),
    reinterpret_cast<void*>(&UnProjectHook),
    reinterpret_cast<void*>(&StatHook<kGetCurrentNumberOfDrawCalls>),
    reinterpret_cast<void*>(&StatHook<kGetPolyCount0>),
    reinterpret_cast<void*>(&PolyCount2Hook)};
static_assert(std::size(kHooks) == kKinds, "a kind's hook is missing");

// at the join, a taken SetCamera just made: the renderer's stack tops against the matrices the block's made
void CompareStandIn(char* r, unsigned v) {
    const float* const* const view = *reinterpret_cast<const float* const* const*>(r + kViewStack);
    const float* const* const proj = *reinterpret_cast<const float* const* const*>(r + kProjStack);
    if (!view || !*view || !proj || !*proj) return;
    const bool viewSame = std::memcmp(*view, g_matrixVersion[v][0], kMatrixBytes) == 0;
    const bool projSame = std::memcmp(*proj, g_matrixVersion[v][1], kMatrixBytes) == 0;
    if (viewSame && projSame) {
        ++g.standInSame;
        return;
    }
    ++g.standInDiffer;
    if (g.loggedStandIns++ >= kLoggedStandIns) return;
    int at = -1;  // (the first float that differs)
    const float* const real = viewSame ? *proj : *view;
    const float* const made = g_matrixVersion[v][viewSame ? 1 : 0];
    for (int i = 0; i < 16 && at < 0; ++i)
        if (std::memcmp(real + i, made + i, 4) != 0) at = i;
    Log("render queue: a taken SetCamera's matrices made on the block differ from the renderer's at the join: %s%s "
        "(from element %d: %.6g there, %.6g made)",
        viewSame ? "" : "view ", projSame ? "" : "projection", at, at >= 0 ? double(real[at]) : 0.0,
        at >= 0 ? double(made[at]) : 0.0);
}

// a taken call made at the join
void Make(const Call& c) {
    void* const r = c.renderer;
    const uint64_t* a = c.a;
    switch (c.kind) {
        case kDeleteRenderMesh: return Fn<DeleteRenderMeshFn>(kDeleteRenderMesh)(r, reinterpret_cast<void*>(a[0]));
        case kEnableFog: Fn<BoolFn>(kEnableFog)(r, a[0] != 0); return;
        case kEnableTMU: return Fn<VoidBoolFn>(kEnableTMU)(r, a[0] != 0);
        case kSelectTMU: return Fn<VoidIntFn>(kSelectTMU)(r, int(a[0]));
        case kSet2DMode: return Fn<Set2DModeFn>(kSet2DMode)(r, a[0] != 0, int(a[1]), int(a[2]), F(a[3]), F(a[4]));
        case kSetCullMode: return Fn<VoidIntFn>(kSetCullMode)(r, int(a[0]));
        case kSetColorOp:
            return Fn<SetColorOpFn>(kSetColorOp)(r, uint8_t(a[0]), uint8_t(a[1]), uint8_t(a[2]), uint8_t(a[3]));
        case kSetState: return Fn<SetStateFn>(kSetState)(r, int(a[0]), int(a[1]));
        case kDraw2dImage:
            return Fn<Draw2dImageFn>(kDraw2dImage)(r, F(a[0]), F(a[1]), F(a[2]), F(a[3]), int(a[14]), F(a[4]), F(a[5]),
                                                   F(a[6]), F(a[7]), F(a[8]), F(a[9]), F(a[10]), F(a[11]), F(a[12]),
                                                   F(a[13]));
        case kFontSetTexture78:
        case kFontSetTexture79:
        case kFontSetRenderingState:
        case kFontSetBlending: return Fn<Ints2Fn>(c.kind)(r, a[0], a[1]);
        case kFontRestoreRenderingState:
        case kRenderDebug:
        case kFlushTextMessages:
        case kEndFrame:
        case kBeginFrame:
        case kMeshAddRef: return Fn<Ints0Fn>(c.kind)(r);
        case kAuxSetRenderFlags: {
            const uint32_t flags = uint32_t(a[0]);
            return Fn<AuxSetFlagsFn>(kAuxSetRenderFlags)(r, &flags);
        }
        case kShaderAddRef:
        case kTextureAddRef:
        case kShaderRelease: {  // (its answer beside the flush was predicted: the same as now?)
            // a sealed one, on the worker while the main thread may be predicting answers from the same count: made
            // under the lock, and no longer pending once made (an uncounted shader's Release had no entry)
            const bool sealed = OnFlushThread();
            if (sealed) AcquireSRWLockExclusive(&g.refsLock);
            if (Fn<CountedAddRefFn>(c.kind)(r) != int(c.out[0])) ++g.refsDiffer;
            if (sealed) {
                if (c.kind != kShaderRelease || c.out[0] != ~0u) RefOf(r).sealed += c.kind == kShaderRelease ? 1 : -1;
                ReleaseSRWLockExclusive(&g.refsLock);
            }
            return;
        }
        case kSetViewport: return Fn<SetViewportFn>(kSetViewport)(r, int(a[0]), int(a[1]), int(a[2]), int(a[3]));
        case kSetCamera:
            reinterpret_cast<SetCameraFn>(g.setCamera)(r, g_cameraVersion[a[0]]);
            if (a[1]) CompareStandIn(static_cast<char*>(r), unsigned(a[0]));
            return;
        case kSetDefaultMaterials: {
            void* const def = reinterpret_cast<void*>(a[0]);
            void* const terrain = reinterpret_cast<void*>(a[1]);
            Fn<DefaultMaterialsFn>(kSetDefaultMaterials)(r, def, terrain);
            MaterialRef(def, kSlotMaterialRelease);  // (the queue's references, after the renderer's)
            MaterialRef(terrain, kSlotMaterialRelease);
            return;
        }
        case kDownLoad: {  // (its answer beside the flush was its Id: the same as now?)
            const unsigned v = Fn<DownLoadFn>(kDownLoad)(r, g.upload.data() + a[0], int(a[1]), int(a[2]), int(a[3]),
                                                         int(a[4]), int(a[5]), a[6] != 0, int(a[7]), int(a[8]),
                                                         const_cast<char*>(Str(a[9])), int(a[10]));
            if (v != c.out[0]) ++g.uploadDiffer;
            return;
        }
        case kSetClearColor: {
            const float color[3] = {F(a[0]), F(a[1]), F(a[2])};
            return Fn<ClearColorFn>(kSetClearColor)(r, color);
        }
        case kDraw2dLabel: {
            alignas(16) const float color[4] = {F(a[4]), F(a[5]), F(a[6]), F(a[7])};
            return Fn<Draw2dLabelFn>(kDraw2dLabel)(r, F(a[0]), F(a[1]), F(a[2]), a[3] ? color : nullptr, a[8] != 0,
                                                   "%s", Str(a[9]));
        }
        case kOnSystemEvent: return Fn<Ints3Fn>(kOnSystemEvent)(r, a[0], a[1], a[2]);
        case kVf214: return Fn<Ints2Fn>(kVf214)(r, a[0], a[1]);
        case kAuxFlush:
        case kSetSkyLight: return Fn<Ints1Fn>(c.kind)(r, a[0]);
        case kFlashRender: return reinterpret_cast<FlashFn>(g.flashRender)(r);  // (r: the player)
        case kSFConfigMask: return Fn<Ints2Fn>(kSFConfigMask)(r, a[0], a[1]);
        case kSFDrawTriList:
            return Fn<SFTriListFn>(kSFDrawTriList)(r, int(a[0]), int(a[1]), int(a[2]), int(a[3]), int(a[4]),
                                                   g.sf.data() + a[5]);
        case kSFDrawLineStrip: return Fn<SFLineStripFn>(kSFDrawLineStrip)(r, int(a[0]), int(a[1]), g.sf.data() + a[2]);
        case kSFDrawGlyphClear: return Fn<SFGlyphFn>(kSFDrawGlyphClear)(r, g.sf.data() + a[0]);
        case kSFFlush: return Fn<Ints0Fn>(kSFFlush)(r);
        case kClearBuffer: {
            alignas(16) const float color[4] = {F(a[2]), F(a[3]), F(a[4]), F(a[5])};
            return Fn<ClearBufferFn>(kClearBuffer)(r, uint32_t(a[0]), a[1] ? color : nullptr, F(a[6]));
        }
        case kSetTerrainAO: {
            const PodArrayView v{g.aoNodes.data() + a[0], int32_t(a[1]), int32_t(a[1])};
            return Fn<SetAONodesFn>(kSetTerrainAO)(r, &v);
        }
        case kGetDynVBPtr: {  // the renderer's lock, at its place; the vertices follow at their DrawDynVB
            Lock& k = g.locks[size_t(a[0])];
            int realOffs = 0;
            k.real = static_cast<char*>(Fn<GetDynVBPtrFn>(kGetDynVBPtr)(r, k.verts, realOffs, k.pool));
            k.realOffs = realOffs;
            k.made = true;
            if (!k.real) ++g.locksFailed;
            return;
        }
        case kDrawDynVB: {
            int offs = int(a[0]);
            if (Lock* k = TokenLock(offs)) {
                if (!k->real) return;
                CopyLock(*k, int(a[2]));
                offs = k->realOffs;
            }
            return Fn<DrawDynVBFn>(kDrawDynVB)(r, offs, int(a[1]), int(a[2]));
        }
        case kPostSet: g.postReturn[0] = Fn<PostSetFn>(kPostSet)(r, Str(a[0]), F(a[1]), a[2]); return;
        case kPostSetVec4: {
            alignas(16) const float v[4] = {F(a[1]), F(a[2]), F(a[3]), F(a[4])};
            g.postReturn[1] = Fn<PostSetPtrFn>(kPostSetVec4)(r, Str(a[0]), v, a[5]);
            return;
        }
        case kPostSetString:
            g.postReturn[2] = Fn<PostSetPtrFn>(kPostSetString)(r, Str(a[0]), Str(a[1]), a[2]);
            return;
        case kEF_Query: {
            float fov = F(a[1]);
            g.nearFovReturn = Fn<QueryFn>(kEF_Query)(r, int(a[0]), reinterpret_cast<intptr_t>(&fov));
            return;
        }
        default: return;
    }
}

// an answered call made again at its place: the same bits as beside the flush?
void Check(const Call& c) {
    uint32_t now[4] = {};
    void* const r = c.renderer;
    switch (c.kind) {
        case kGetWidth:
        case kGetHeight:
        case kGetFeatures:
        case kGetTextureID:
        case kGetRenderType:
        case kGetCurrentNumberOfDrawCalls:
        case kGetPolyCount0: now[0] = uint32_t(Fn<IntFn>(c.kind)(r)); break;
        case kIsMultiGPUModeActive: now[0] = Fn<FlagFn>(c.kind)(r); break;
        case kGetPolyCount2: {
            int polygons = 0, shadowPolygons = 0;
            Fn<PolyCount2Fn>(c.kind)(r, &polygons, &shadowPolygons);
            now[0] = uint32_t(polygons);
            now[1] = uint32_t(shadowPolygons);
            break;
        }
        case kGetShaderQuality: now[0] = uint32_t(Fn<IntIntFn>(c.kind)(r, int(c.a[0]))); break;
        case kGetIRenderAuxGeom:
        case kGetHWND: {
            const uintptr_t v = uintptr_t(Fn<PtrFn>(c.kind)(r));
            now[0] = uint32_t(v);
            now[1] = uint32_t(v >> 32);
            break;
        }
        case kEF_QuerySetting: {
            const uintptr_t v = uintptr_t(Fn<QueryFn>(kEF_QuerySetting)(r, int(c.a[0]), intptr_t(c.a[1])));
            now[0] = uint32_t(v);
            now[1] = uint32_t(v >> 32);
            break;
        }
        case kScaleCoordX:
        case kScaleCoordY: now[0] = uint32_t(B(Fn<ScaleFn>(c.kind)(r, F(c.a[0])))); break;
        case kGetFrameID: now[0] = uint32_t(Fn<FrameIdFn>(kGetFrameID)(r, c.a[0] != 0)); break;
        case kGetViewport: {
            int v[4] = {};
            Fn<ViewportFn>(kGetViewport)(r, &v[0], &v[1], &v[2], &v[3]);
            for (int i = 0; i < 4; ++i) now[i] = uint32_t(v[i]);
            break;
        }
        case kProjectToScreen: {
            float s[3] = {};
            Fn<ProjectFn>(kProjectToScreen)(r, F(c.a[0]), F(c.a[1]), F(c.a[2]), &s[0], &s[1], &s[2]);
            for (int i = 0; i < 3; ++i) now[i] = uint32_t(B(s[i]));
            break;
        }
        case kUnProject: {
            float p[3] = {};
            now[3] = uint32_t(Fn<UnProjectFn>(kUnProject)(r, F(c.a[0]), F(c.a[1]), F(c.a[2]), &p[0], &p[1], &p[2]));
            for (int i = 0; i < 3; ++i) now[i] = uint32_t(B(p[i]));
            break;
        }
        case kAuxGetRenderFlags: Fn<AuxGetFlagsFn>(kAuxGetRenderFlags)(r, &now[0]); break;
        case kResRef: {
            ResRef& f = g.resRefs[c.a[0]];
            if (std::memcmp(f.value, f.before, f.bytes) != 0) {  // (the caller wrote them: the write, made here)
                std::memcpy(f.real, f.value, f.bytes);
                ++g.resWrites;
                return;
            }
            std::memcpy(now, f.real, f.bytes);  // (read only: the same as answered?)
            break;
        }
        case kMeshPosPtr: {
            int stride = 0;
            uint8_t* const p = Fn<PosPtrFn>(kMeshPosPtr)(r, &stride, int(c.a[0]), false);
            now[0] = uint32_t(uintptr_t(p));
            now[1] = uint32_t(uintptr_t(p) >> 32);
            now[2] = uint32_t(stride);
            break;
        }
        case kGetModelView:
        case kGetProjection: {
            float m[16];
            Fn<MatrixFn>(c.kind)(r, m);
            const int which = c.kind == kGetProjection;
            const float* const answered = c.a[0] ? g_matrixVersion[c.a[0] - 1][which] : g_matrixHandOff[which];
            now[0] = std::memcmp(m, answered, kMatrixBytes) != 0;
            break;
        }
        case kMeshIndices: {
            int n = 0;
            uint16_t* const p = Fn<IndicesFn>(kMeshIndices)(r, c.a[0] ? &n : nullptr);
            now[0] = uint32_t(uintptr_t(p));
            now[1] = uint32_t(uintptr_t(p) >> 32);
            now[2] = uint32_t(n);
            break;
        }
        case kGetCamera: {  // (answered from a copy: its members the same as the camera's now?)
            const uint8_t* const answered = c.a[0] ? g_cameraVersion[c.a[0] - 1] : g_cameraHandOff;
            now[0] = std::memcmp(static_cast<char*>(r) + g.cameraAt, answered, kCameraMembers) != 0;
            break;
        }
        case kOceanGrid: {
            const uintptr_t v = uintptr_t(Fn<PtrFn>(kOceanGrid)(r));
            now[0] = uint32_t(v);
            now[1] = uint32_t(v >> 32);
            break;
        }
        case kOceanPoint: {
            float p[3] = {};
            Fn<OceanPointFn>(kOceanPoint)(r, p, F(c.a[0]), F(c.a[1]));
            for (int i = 0; i < 3; ++i) now[i] = uint32_t(B(p[i]));
            break;
        }
        case kPostGet: {
            uint32_t v = kPostUnwritten;
            Fn<PostGetFn>(kPostGet)(r, Str(c.a[0]), reinterpret_cast<float*>(&v));
            const bool found = v != kPostUnwritten;
            now[0] = found ? v : 0;
            now[1] = found;
            break;
        }
        case kPostGetVec4: {
            uint32_t v[4] = {kPostUnwritten, kPostUnwritten, kPostUnwritten, kPostUnwritten};
            Fn<PostGetVec4Fn>(kPostGetVec4)(r, Str(c.a[0]), reinterpret_cast<float*>(v));
            bool found = false;
            for (int k = 0; k < 4; ++k) found |= v[k] != kPostUnwritten;
            for (int k = 0; k < 4; ++k) now[k] = found ? v[k] : 0;
            if (found != (c.a[1] != 0)) now[0] = ~c.out[0];  // (found now and not then, or the other way: differing)
            break;
        }
        case kFieldGetter: {
            const uint64_t v = reinterpret_cast<GetterFn>(g.getterFns[c.a[0]][c.a[1]])(r) & g.getterMask[c.a[0]][c.a[1]];
            now[0] = uint32_t(v);
            now[1] = uint32_t(v >> 32);
            break;
        }
        default: return;
    }
    if (std::memcmp(now, c.out, sizeof now) == 0) return;
    ++g.differ[c.kind];
    if (c.kind == kFieldGetter) ++g.getterDiffer[c.a[0]][c.a[1]];
    if (c.kind >= kFirstLagged) return;  // (one frame late by design: counted, not logged)
    if (++g.logged > kLoggedDiffer) return;
    if (c.kind == kFieldGetter)
        Log("render queue: %s slot %llu (object %p) answered 0x%X 0x%X beside the flush, 0x%X 0x%X after it",
            kGetterNames[c.a[0]], c.a[1], r, c.out[0], c.out[1], now[0], now[1]);
    else
        Log("render queue: %s(%g, %g, %g) answered 0x%X 0x%X 0x%X 0x%X beside the flush, 0x%X 0x%X 0x%X 0x%X after it",
            kDefs[c.kind].name, double(F(c.a[0])), double(F(c.a[1])), double(F(c.a[2])), c.out[0], c.out[1], c.out[2],
            c.out[3], now[0], now[1], now[2], now[3]);
}

// at the join: the camera the flush left (the worker's copy, if it made the frame's end), against the hand-off's copy
// (ProjectToScreen's answers beside the flush)
void CompareCamera() {
    Camera& k = g.camera;
    if (!k.valid) return;
    k.valid = false;
    const char* const r = k.renderer;
    const float* const* const views = *reinterpret_cast<const float* const* const*>(r + kViewStack);
    const float* const* const projs = *reinterpret_cast<const float* const* const*>(r + kProjStack);
    const bool left = g.left.valid;
    const float* const view = left ? (g.left.view ? g_matrixLeft[0] : nullptr) : views ? *views : nullptr;
    const float* const proj = left ? (g.left.proj ? g_matrixLeft[1] : nullptr) : projs ? *projs : nullptr;
    const void* const viewport = left ? static_cast<const void*>(g.left.viewport) : r + kViewport;
    const bool differ[kCamParts] = {
        std::memcmp(viewport, g.handOffViewport, kViewportBytes) != 0,  // (the copy's may be a taken one's)
        !view || std::memcmp(view, k.view, kMatrixBytes) != 0,
        !proj || std::memcmp(proj, k.proj, kMatrixBytes) != 0};
    bool any = false;
    for (int p = 0; p < kCamParts; ++p)
        if (differ[p]) {
            ++g.cameraDiffer[p];
            any = true;
        }
    ++g.cameraCompared;
    if (!any) ++g.cameraSame;
}

// The barrier (§11): the next frame's scene traversal, I3DEngine::RenderWorld (slot 2 of C3DEngine's vtable), joins
// the flush on the main thread, through a thunk the gate adds (it keeps the caller's return address, by which the
// reflection part knows its RenderWorld calls). With it in place BeginFrame is taken, and the main thread runs its next
// Update beside the flush. The particle manager's queue swap, after which the particle thread updates the last frame's
// containers, is only reached from RenderWorld: the particles' counter updates after the flush keep their order.
constexpr unsigned kSlotRenderWorld = 2;
void InstallBarrier() {
    char* const engine = reinterpret_cast<char*>(GetModuleHandleW(L"Cry3DEngine.dll"));
    char** const vt = engine ? reinterpret_cast<char**>(FindRttiVtable(engine, ".?AVC3DEngine@@")) : nullptr;
    const int index = vt ? RenderGate_AddSlot(vt, kSlotRenderWorld, ".?AVC3DEngine@@") : -1;
    if (index < 0 || g.beginFrameIndex < 0) {
        Log("render queue: no barrier (%s): BeginFrame joins the flush",
            !vt ? "C3DEngine's vtable not found" : index < 0 ? "the gate has no spare thunk" : "BeginFrame not found");
        return;
    }
    g.barrier = index;
    RenderGate_Redirect(unsigned(g.beginFrameIndex), kHooks[kBeginFrame]);
    g.passes[kBeginFrame] = g.beginFrameIndex;
    const char* const world = static_cast<const char*>(RenderGate_Original(vt[kSlotRenderWorld]));
    Log("render queue: the barrier: the next RenderWorld (Cry3DEngine.dll+0x%X) joins the flush; BeginFrame is taken, "
        "and the main thread goes on into its next Update",
        unsigned(world - engine));
}
#endif

}  // namespace

bool RenderQueue_Install(const wchar_t* moduleName, char* base, char* endEf3D) {
#ifdef _WIN64
    // (the renderer's class is needed; without another one, its calls join)
    char** vts[kClasses];
    for (int c = 0; c < kClasses; ++c) vts[c] = reinterpret_cast<char**>(FindRttiVtable(base, kClassNames[c]));
    char** const vt = vts[kRenderer];
    if (!vt) {
        Log("%ls: render queue: not installed (%s's vtable not found): every renderer call joins the flush",
            moduleName, kClassNames[kRenderer]);
        return false;
    }
    for (int c = 0; c < kClasses; ++c)
        if (!vts[c]) Log("%ls: render queue: %s's vtable not found: its calls join the flush", moduleName, kClassNames[c]);
    if (CallTrace_Original(vt[kSlotEndEf3D]) != endEf3D) {
        Log("%ls: render queue: not installed (CD3D9Renderer's vtable doesn't have EF_EndEf3D at slot 115): every "
            "renderer call joins the flush",
            moduleName);
        return false;
    }
    int index[kKinds];
    for (int k = 0; k < kKinds; ++k) {
        char** const v = vts[kDefs[k].cls];
        if (!v || kDefs[k].slot == kDynamicSlot) {
            index[k] = -1;
            continue;
        }
        g.fns[k] = CallTrace_Original(v[kDefs[k].slot]);
        index[k] = RenderGate_IndexOf(v, kDefs[k].slot);
        if (!IsInCode(base, static_cast<const char*>(g.fns[k]), 1) || index[k] < 0) {
            Log("%ls: render queue: not installed (slot %u, %s, doesn't lead into the renderer's code through the gate)",
                moduleName, kDefs[k].slot, kDefs[k].name);
            return false;
        }
    }
    if (HMODULE crt = GetModuleHandleW(L"msvcr80.dll"))
        g.vsnprintf80 = reinterpret_cast<VsnprintfFn>(GetProcAddress(crt, "_vsnprintf"));
    // the code the aux geometry's flags and the AddRef calls are taken by, checked (else they join)
    auto codeIs = [base](int k, const char* pattern, size_t bytes) {
        const char* const fn = static_cast<const char*>(g.fns[k]);
        return fn && IsInCode(base, fn, bytes) && MatchesAt(fn, ParsePattern(pattern));
    };
    if (index[kAuxSetRenderFlags] >= 0 || index[kAuxGetRenderFlags] >= 0) {
        const char* const set = static_cast<const char*>(g.fns[kAuxSetRenderFlags]);
        const char* const get = static_cast<const char*>(g.fns[kAuxGetRenderFlags]);
        if (!codeIs(kAuxSetRenderFlags, kAuxSetFlagsCode, 9) || !codeIs(kAuxGetRenderFlags, kAuxGetFlagsCode, 12) ||
            std::memcmp(set + 4, get + 2, 4) != 0) {
            index[kAuxSetRenderFlags] = index[kAuxGetRenderFlags] = -1;
            Log("%ls: render queue: the aux geometry's render flags join the flush (their code isn't as expected)",
                moduleName);
        }
    }
    if (index[kMeshAddRef] >= 0 && !codeIs(kMeshAddRef, kMeshAddRefCode, 5)) {
        index[kMeshAddRef] = -1;
        Log("%ls: render queue: a render mesh's AddRef joins the flush (its code isn't as expected)", moduleName);
    }
    for (const Kind k : {kShaderAddRef, kTextureAddRef}) {
        if (index[k] < 0) continue;
        const char* const fn = static_cast<const char*>(g.fns[k]);
        if (codeIs(k, kCountedAddRefCode, 10) && fn[2] == fn[8]) {
            g.countAt[k == kTextureAddRef] = uint8_t(fn[2]);
        } else {
            index[k] = -1;
            Log("%ls: render queue: %s joins the flush (its code isn't as expected)", moduleName, kDefs[k].name);
        }
    }
    // a shader's Release: its code, and the count AddRef's code counts (its answers are predicted together)
    if (index[kShaderRelease] >= 0 && (!codeIs(kShaderRelease, kShaderReleaseCode, kShaderReleaseBytes) ||
                                       index[kShaderAddRef] < 0 || g.countAt[0] != kShaderCount)) {
        index[kShaderRelease] = -1;
        Log("%ls: render queue: IShader::Release joins the flush (its code, or AddRef's, isn't as expected)",
            moduleName);
    }
    if (index[kUnProject] >= 0 && !codeIs(kUnProject, kUnProjectCode, kUnProjectBytes)) {
        index[kUnProject] = -1;
        Log("%ls: render queue: UnProjectFromScreen joins the flush (its code isn't as expected)", moduleName);
    }
    if (index[kSetSkyLight] >= 0 && !codeIs(kSetSkyLight, kSetSkyLightCode, 8)) {
        index[kSetSkyLight] = -1;
        Log("%ls: render queue: SetSkyLightRenderParams joins the flush (its code isn't as expected)", moduleName);
    }
    if (index[kSetTerrainAO] >= 0) {  // (its code, and its append's: the nodes' size)
        bool ok = codeIs(kSetTerrainAO, kSetTerrainAOCode, 22);
        if (ok) {
            const char* const fn = static_cast<const char*>(g.fns[kSetTerrainAO]);
            int32_t rel;
            std::memcpy(&rel, fn + kAONodesJmpAt + 1, 4);
            char* const append = const_cast<char*>(fn + kAONodesJmpAt + 5 + rel);
            ok = IsInCode(base, append, kAONodesAppendScan) &&
                 !FindInRange(append, kAONodesAppendScan, ParsePattern(kAONodesAppendSize), 1).empty();
        }
        if (!ok) {
            index[kSetTerrainAO] = -1;
            Log("%ls: render queue: SetTerrainAONodes joins the flush (its code, or its append's, isn't as expected)",
                moduleName);
        }
    }
    // the ocean: its grid's code, and its draw (the mark the answers wait for)
    if (index[kOceanGrid] >= 0 || index[kOceanPoint] >= 0) {
        const char* const grid = static_cast<const char*>(g.fns[kOceanGrid]);
        bool ok = codeIs(kOceanGrid, kOceanGridCode, 14);
        if (ok) {
            int32_t rel;
            std::memcpy(&rel, grid + 10, 4);
            const char* const vector = grid + 14 + rel;
            ok = IsInCode(base, vector, 18) && MatchesAt(vector, ParsePattern(kOceanVectorCode));
        }
        char** const ov = vts[kOcean];
        const int draw = ok ? RenderGate_IndexOf(ov, kSlotOceanDraw) : -1;
        g.oceanDraw = draw >= 0 ? CallTrace_Original(ov[kSlotOceanDraw]) : nullptr;
        if (draw < 0 || !IsInCode(base, static_cast<const char*>(g.oceanDraw), 1)) {
            index[kOceanGrid] = index[kOceanPoint] = -1;
            Log("%ls: render queue: the ocean's grid joins the flush (its code isn't as expected)", moduleName);
        } else {
            RenderGate_Redirect(unsigned(draw), reinterpret_cast<void*>(&OceanDrawHook));  // (the mark; it still joins)
            // the grid's update the draw makes first, after its geometry's checks (FrameUpdate), and EF_AddEf
            // observed: the update made at the flush's start when the traversal has added the ocean
            const char* const od = static_cast<const char*>(g.oceanDraw);
            const char* update = nullptr;
            if (IsInCode(base, od, kOceanDrawBytes) && MatchesAt(od, ParsePattern(kOceanDrawCode))) {
                int32_t rel;
                std::memcpy(&rel, od + kOceanUpdateCallAt + 1, 4);
                update = od + kOceanUpdateCallAt + 5 + rel;
            }
            const int addAt = RenderGate_IndexOf(vt, kSlotAddEf);
            if (update && addAt >= 0 && IsInCode(base, update, kOceanUpdateBytes) &&
                MatchesAt(update, ParsePattern(kOceanUpdateCode))) {
                g.oceanVt = ov;
                g.addEf = CallTrace_Original(vt[kSlotAddEf]);
                g.oceanUpdate = const_cast<char*>(update);
                RenderGate_Redirect(unsigned(addAt), reinterpret_cast<void*>(&AddEfHook));  // (observed; it still joins)
            } else {
                Log("%ls: render queue: the ocean's grid waits for the flush's ocean draw beside it (its update's code "
                    "isn't as expected)",
                    moduleName);
            }
        }
    }
    // the post effects' float getter: the manager and its lookup it calls, CParamFloat's getter and setter
    if (index[kPostGet] >= 0) {
        const char* const get = static_cast<const char*>(g.fns[kPostGet]);
        bool ok = codeIs(kPostGet, kPostGetCode, kPostGetBytes);
        const char *lookup = nullptr, *paramGet = nullptr, *paramSet = nullptr;
        if (ok) {
            int32_t d, rel;
            std::memcpy(&d, get + kPostMgrAt, 4);
            std::memcpy(&rel, get + kPostLookupAt, 4);
            g.postMgr = const_cast<char*>(get + kPostMgrAt + 4 + d);
            lookup = get + kPostLookupAt + 4 + rel;
            char** const pv = reinterpret_cast<char**>(FindRttiVtable(base, ".?AVCParamFloat@NPostEffects@@"));
            paramGet = pv ? static_cast<const char*>(CallTrace_Original(pv[kSlotParamGet])) : nullptr;
            paramSet = pv ? static_cast<const char*>(CallTrace_Original(pv[kSlotParamSet])) : nullptr;
            ok = IsInCode(base, lookup, kPostLookupBytes) && MatchesAt(lookup, ParsePattern(kPostLookupCode)) &&
                 IsInCode(base, paramGet, kParamGetBytes) && MatchesAt(paramGet, ParsePattern(kParamFloatGetCode)) &&
                 IsInCode(base, paramSet, kParamSetBytes) && MatchesAt(paramSet, ParsePattern(kParamFloatSetCode));
        }
        if (ok) {
            int32_t d;
            std::memcpy(&d, paramGet + kParamSwitchAt, 4);
            g.paramBlending = reinterpret_cast<const int*>(paramGet + kParamSwitchAt + 5 + d);  // (cmp's imm8 after it)
            g.paramFloatGet = paramGet;
            g.postLookup = lookup;
            // (CParamBool's and CParamInt's getters and setters, by their code; else their parameters join)
            char** const bv = reinterpret_cast<char**>(FindRttiVtable(base, ".?AVCParamBool@NPostEffects@@"));
            char** const iv = reinterpret_cast<char**>(FindRttiVtable(base, ".?AVCParamInt@NPostEffects@@"));
            const auto fnOf = [](char** vt, unsigned slot) {
                return vt ? static_cast<const char*>(CallTrace_Original(vt[slot])) : nullptr;
            };
            const auto is = [base](const char* fn, const char* code) {
                const Pattern p = ParsePattern(code);
                return fn && IsInCode(base, fn, p.size()) && MatchesAt(fn, p);
            };
            const char* const boolGet = fnOf(bv, kSlotParamGet);
            const char* const boolSet = fnOf(bv, kSlotParamSet);
            if (is(boolGet, kParamBoolGetCode) && is(boolSet, kParamBoolSetCode)) {
                int32_t c;
                std::memcpy(&c, boolSet + kParamBoolConstAt, 4);
                const char* const constant = boolSet + kParamBoolConstAt + 4 + c;
                if (IsInImage(base, constant, sizeof(float))) {
                    g.paramBoolFalse = reinterpret_cast<const float*>(constant);
                    g.paramBoolGet = boolGet;
                }
            }
            if (is(fnOf(iv, kSlotParamGet), kParamIntGetCode) && is(fnOf(iv, kSlotParamSet), kParamIntSetCode))
                g.paramIntGet = fnOf(iv, kSlotParamGet);
            if (!g.paramBoolGet || !g.paramIntGet)
                Log("%ls: render queue: the post effects' %s parameters join the flush (their code isn't as expected)",
                    moduleName, !g.paramBoolGet && !g.paramIntGet ? "bool and int" : g.paramBoolGet ? "int" : "bool");
        } else {
            index[kPostGet] = -1;
            g.postMgr = nullptr;
            Log("%ls: render queue: GetPostEffectParam joins the flush (its code, or its lookup's or CParamFloat's, "
                "isn't as expected)",
                moduleName);
        }
    }
    // the Vec4 getter: the same manager and lookup, CParamVec4's getter and setter, the same blending switch
    if (index[kPostGetVec4] >= 0) {
        const char* const get = static_cast<const char*>(g.fns[kPostGetVec4]);
        bool ok = g.postMgr && codeIs(kPostGetVec4, kPostGetVec4Code, kPostGetVec4Bytes);
        if (ok) {
            int32_t d, rel;
            std::memcpy(&d, get + kPostMgrAt, 4);
            std::memcpy(&rel, get + kPostLookupAt, 4);
            char** const pv = reinterpret_cast<char**>(FindRttiVtable(base, ".?AVCParamVec4@NPostEffects@@"));
            const char* const paramGet =
                pv ? static_cast<const char*>(CallTrace_Original(pv[kSlotParamGetVec4])) : nullptr;
            const char* const paramSet =
                pv ? static_cast<const char*>(CallTrace_Original(pv[kSlotParamSetVec4])) : nullptr;
            ok = get + kPostMgrAt + 4 + d == g.postMgr && get + kPostLookupAt + 4 + rel == g.postLookup &&
                 IsInCode(base, paramGet, kParamVec4GetBytes) && MatchesAt(paramGet, ParsePattern(kParamVec4GetCode)) &&
                 IsInCode(base, paramSet, kParamVec4SetBytes) && MatchesAt(paramSet, ParsePattern(kParamVec4SetCode));
            if (ok) {
                std::memcpy(&d, paramGet + kParamVec4SwitchAt, 4);
                ok = reinterpret_cast<const int*>(paramGet + kParamVec4SwitchAt + 5 + d) == g.paramBlending;
            }
            if (ok) g.paramVec4Get = paramGet;
        }
        if (!ok) {
            index[kPostGetVec4] = -1;
            Log("%ls: render queue: GetPostEffectParamVec4 joins the flush (its code, or CParamVec4's, isn't as "
                "expected)",
                moduleName);
        }
    }
    if (index[kDownLoad] >= 0) g.upload.assign(kUploadBytes + kUploadGuard, 0);
    // the camera the flush leaves (a diagnostic): GetCamera's field, SetCamera copying the camera's size into it (the
    // field's displacement loaded right before), and SetCamera's calls observed
    {
        const char* const get = static_cast<const char*>(CallTrace_Original(vt[kSlotGetCamera]));
        const char* const set = static_cast<const char*>(CallTrace_Original(vt[kSlotSetCamera]));
        const int setAt = RenderGate_IndexOf(vt, kSlotSetCamera);
        uint32_t at = 0;
        bool ok = setAt >= 0 && IsInCode(base, get, 8) && MatchesAt(get, ParsePattern(kGetCameraCode)) &&
                  IsInCode(base, set, kSetCameraScan);
        if (ok) {
            std::memcpy(&at, get + 3, 4);
            const std::vector<char*> sizes =
                FindInRange(const_cast<char*>(set), kSetCameraScan, ParsePattern(kSetCameraSize), 2);
            bool loaded = false;
            for (size_t i = 0; sizes.size() == 1 && sizes[0] - 0x40 >= set && i < 0x40 && !loaded; ++i)
                loaded = std::memcmp(sizes[0] - 0x40 + i, &at, 4) == 0;
            ok = loaded;
        }
        if (ok) {
            g.cameraAt = at;
            g.setCamera = const_cast<char*>(set);  // (its slot goes to SetCameraHook with the kinds')
        } else {
            index[kGetCamera] = index[kSetCamera] = -1;
            Log("%ls: render queue: GetCamera and SetCamera join the flush, and the camera it leaves isn't measured "
                "(GetCamera's or SetCamera's code isn't as expected)",
                moduleName);
        }
    }
    // SetViewport: it writes the four words of the viewport the camera copy has (their displacements in its code;
    // without the copy, GetViewport and ProjectToScreen join, after the taken SetViewport is made)
    if (index[kSetViewport] >= 0) {
        const char* const fn = static_cast<const char*>(g.fns[kSetViewport]);
        constexpr size_t kSetViewportScan = 0x60;
        bool ok = IsInCode(base, fn, kSetViewportScan);
        for (uint32_t w = 0; w < 4 && ok; ++w) {
            const uint32_t d = uint32_t(kViewport + 4 * w);
            bool found = false;
            for (size_t i = 0; i + 4 <= kSetViewportScan && !found; ++i) found = std::memcmp(fn + i, &d, 4) == 0;
            ok = found;
        }
        if (!ok) {
            index[kSetViewport] = -1;
            Log("%ls: render queue: SetViewport joins the flush (its code isn't as expected)",
                moduleName);
        }
    }
    if (index[kMeshPosPtr] >= 0 && !codeIs(kMeshPosPtr, kPosPtrCode, kPosPtrBytes)) {
        index[kMeshPosPtr] = -1;
        Log("%ls: render queue: a render mesh's GetStridedPosPtr joins the flush (its code isn't as expected)",
            moduleName);
    }
    if (index[kMeshIndices] >= 0 && !codeIs(kMeshIndices, kIndicesCode, 33)) {
        index[kMeshIndices] = -1;
        Log("%ls: render queue: a render mesh's GetIndices joins the flush (its code isn't as expected)", moduleName);
    }
    for (const Kind k : {kGetModelView, kGetProjection}) {
        const uint8_t stack = uint8_t(k == kGetModelView ? kViewStack : kProjStack);
        if (index[k] >= 0 &&
            !(codeIs(k, kMatrixGetCode, kMatrixGetBytes) &&
              static_cast<const uint8_t*>(g.fns[k])[kMatrixGetStackAt] == stack)) {
            index[k] = -1;
            Log("%ls: render queue: %s joins the flush (its code isn't as expected)", moduleName, kDefs[k].name);
        }
    }
    // the shader resources' references, in the vtable of their interface at +0x38
    char** rv = nullptr;
    {
        const LONG header = reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew;
        auto* const nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + header);
        for (const RttiVtable& v : FindRttiVtables(base, nt->OptionalHeader.SizeOfImage, &CallTrace_Original))
            if (v.offset == kResAt && std::strcmp(v.mangled, ".?AUSRenderShaderResources@@") == 0) rv = v.vtable;
        for (size_t i = 0; rv && i < std::size(kResSlots); ++i) {
            const char* const fn = static_cast<const char*>(CallTrace_Original(rv[kResSlots[i].slot]));
            bool shape = false;
            for (const char* code : kResCodes) shape |= IsInCode(base, fn, 26) && MatchesAt(fn, ParsePattern(code));
            const int at = shape ? RenderGate_IndexOf(rv, kResSlots[i].slot) : -1;
            if (at < 0) continue;
            g.resFns[i] = const_cast<char*>(fn);
            RenderGate_Redirect(unsigned(at), reinterpret_cast<void*>(kResHooks[i]));
            g.resPasses.push_back(at);
        }
        Log("%ls: render queue: the shader resources' references answered beside the flush: %zu of %zu",
            moduleName, g.resPasses.size(), std::size(kResSlots));
    }
    // the Flash players' draws: taken if each draw's code copies what the queue copies (else these six join, and the
    // players' whole renders are taken, below)
    {
        const struct {
            Kind kind;
            const char* const* code;
            size_t n;
        } checks[] = {{kSFDrawTriList, kSFTriListCode, std::size(kSFTriListCode)},
                      {kSFDrawLineStrip, kSFLineStripCode, std::size(kSFLineStripCode)},
                      {kSFDrawGlyphClear, kSFGlyphCode, std::size(kSFGlyphCode)}};
        bool ok = true;
        for (const auto& c : checks)
            for (size_t i = 0; i < c.n && ok; ++i)
                ok = index[c.kind] >= 0 && CodeHas(base, static_cast<const char*>(g.fns[c.kind]), c.code[i]);
        g.sfTaken = ok;
        if (ok) {
            g.sf.assign(kSFBytes, 0);
        } else {
            for (const Kind k : {kSFConfigMask, kSFDrawTriList, kSFDrawLineStrip, kSFDrawGlyphClear, kSFFlush,
                                 kClearBuffer})
                index[k] = -1;
        }
        Log("%ls: render queue: the Flash players' draws %s", moduleName,
            ok ? "taken beside the flush (their parameters, vertices, indices and transform copied; the stencil masks, "
                 "the clears)"
               : "join the flush (the draws' code isn't as expected)");
    }
    g.marks = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (int k = 0; k < kKinds; ++k) {
        // (its class not found; Draw2dLabel's text can't be formatted as the renderer would: they join. BeginFrame
        // waits for the barrier, at the first hand-off)
        if (index[k] < 0 || (k == kDraw2dLabel && !g.vsnprintf80) || k == kBeginFrame) {
            if (k == kBeginFrame) g.beginFrameIndex = index[k];
            g.passes[k] = -1;
            continue;
        }
        RenderGate_Redirect(unsigned(index[k]), kHooks[k]);
        g.passes[k] = index[k];
    }
    if (!g.vsnprintf80)
        Log("%ls: render queue: msvcr80's _vsnprintf not found: Draw2dLabel joins the flush", moduleName);
    // the field getters, through each class's vtable slots up to its end (where its entries stop leading into code),
    // but those the queue handles otherwise (the texture's: GetFlags only)
    constexpr uint8_t kGetterOf[kGetterClasses] = {kRenderMesh, kShader, kTexture, kResClass};
    for (unsigned gc = 0; gc < kGetterClasses; ++gc) {
        char** const cv = kGetterOf[gc] == kResClass ? rv : vts[kGetterOf[gc]];
        if (!cv) continue;
        char list[400] = "";
        size_t used = 0, answered = 0;
        for (unsigned s = 0; s < kGetterSlots; ++s) {
            const char* const fn = static_cast<const char*>(CallTrace_Original(cv[s]));
            if (!IsInCode(base, fn, 1)) break;
            bool other = (gc == kGetterMesh && (s == kMeshSlotVideoBuffer || s == kMeshSlotUpdateFrame)) ||
                         (gc == kGetterTexture && s != kTextureSlotFlags);
            for (const Def& d : kDefs) other |= d.cls == kGetterOf[gc] && d.slot == s;
            for (const ResSlot& r : kResSlots) other |= gc == kGetterResources && r.slot == s;
            const bool passGetter = gc == kGetterShader && IsInCode(base, fn, kShaderPassGetterBytes) &&
                                    MatchesAt(fn, ParsePattern(kShaderPassGetterCode));
            const uint64_t mask = passGetter ? ~0ull : FieldGetter(base, fn);
            if (other || !mask) continue;
            const int at = RenderGate_IndexOf(cv, s);
            if (at < 0) continue;
            g.getterFns[gc][s] = const_cast<char*>(fn);
            g.getterMask[gc][s] = mask;
            RenderGate_Redirect(unsigned(at), reinterpret_cast<void*>(kGetterHooks[gc][s]));
            g.getterPasses.push_back(at);
            ++answered;
            const int w = snprintf(list + used, sizeof list - used, "%s%u", used ? " " : "", s);
            if (w > 0 && size_t(w) < sizeof list - used) used += size_t(w);
        }
        Log("%ls: render queue: %s's field getters answered beside the flush: %zu slots (%s)", moduleName,
            kGetterNames[gc], answered, used ? list : "none");
    }
    // the methods made beside the flush with no record, each if its code is as expected (EF_PrecacheResource's too
    // only if the texture method it calls is the empty one)
    {
        const char* const empty =
            vts[kTexture] ? static_cast<const char*>(CallTrace_Original(vts[kTexture][kTextureSlotPrecache])) : nullptr;
        const bool emptyPrecache = empty && IsInCode(base, empty, 3) && MatchesAt(empty, ParsePattern(kEmptyMethodCode));
        char list[600] = "";
        size_t used = 0;
        for (const PureSlot& p : kPureSlots) {
            char** const cv = p.cls == kResClass ? rv : vts[p.cls];
            const char* const fn = cv ? static_cast<const char*>(CallTrace_Original(cv[p.slot])) : nullptr;
            const Pattern pattern = ParsePattern(p.code);
            bool ok = fn && IsInCode(base, fn, pattern.size()) && MatchesAt(fn, pattern);
            if (p.cls == kRenderer && p.slot == 83) ok &= emptyPrecache;
            const int at = ok ? RenderGate_IndexOf(cv, p.slot) : -1;
            if (at >= 0) g.purePasses.push_back(at);
            const int w = snprintf(list + used, sizeof list - used, "%s%s%s", used ? ", " : "", p.name,
                                   at >= 0 ? "" : " (not: its code isn't as expected)");
            if (w > 0 && size_t(w) < sizeof list - used) used += size_t(w);
        }
        Log("%ls: render queue: made beside the flush with no record: %s", moduleName, list);
    }
    // the Flash player's Render and Release, where its draws aren't taken: CrySystem's CFlashPlayer vtable, its two
    // slots pointed at the hooks (the release guard first, the originals kept before either)
    if (HMODULE sys = g.sfTaken ? nullptr : GetModuleHandleW(L"CrySystem.dll")) {
        char* const sb = reinterpret_cast<char*>(sys);
        char** const fv = reinterpret_cast<char**>(FindRttiVtable(sb, ".?AVCFlashPlayer@@"));
        const auto codeAt = [sb](const char* fn, const char* code) {
            const Pattern p = ParsePattern(code);
            return fn && IsInCode(sb, fn, p.size()) && MatchesAt(fn, p);
        };
        char* const render = fv ? fv[kSlotFlashRender] : nullptr;
        char* const release = fv ? fv[kSlotFlashRelease] : nullptr;
        bool ok = codeAt(render, kFlashRenderCode) && codeAt(release, kFlashReleaseCode);
        if (ok) {
            g.flashRender = render;
            g.flashRelease = release;
            ok = PatchSlot(&fv[kSlotFlashRelease], reinterpret_cast<void*>(&FlashReleaseHook)) &&
                 PatchSlot(&fv[kSlotFlashRender], reinterpret_cast<void*>(&FlashRenderHook));
        }
        Log("%ls: render queue: the Flash players' renders (the HUD, the menus) %s", moduleName,
            ok ? "taken beside the flush (CrySystem's CFlashPlayer)"
               : "join the flush (CrySystem's CFlashPlayer, its vtable or code, isn't as expected)");
    }
    g.calls.reserve(4096);
    g.sealed.reserve(4096);
    g.text.assign(kTextBytes, 0);
    g.locks.reserve(1024);  // (each kept where it is: a window takes no more, see GetDynVBPtrHook; also the strings')
    g.strings.reserve(kStringBytes);
    g.aoNodes.reserve(kAONodesBytes);
    g.cameraLayout = CheckCameraLayout(base, static_cast<const char*>(g.fns[kProjectToScreen]));
    if (g.cameraLayout) {
        Camera& k = g.camera;
        k.block.assign(kProjStack + sizeof(void*), 0);
        for (int i = 0; i < 8; ++i) {
            k.viewStack[i] = k.view;
            k.projStack[i] = k.proj;
        }
        float** const stacks[2] = {k.viewStack, k.projStack};
        std::memcpy(k.block.data() + kViewStack, &stacks[0], sizeof(void*));
        std::memcpy(k.block.data() + kProjStack, &stacks[1], sizeof(void*));
        // its vtable, for UnProjectFromScreen's calls: the renderer's functions (behind the gate's thunks)
        for (unsigned s = 0; index[kUnProject] >= 0 && s < 256; ++s) {
            void* const fn = CallTrace_Original(vt[s]);
            if (!IsInCode(base, static_cast<const char*>(fn), 1)) break;
            g.blockVtable.push_back(fn);
        }
        if (g.blockVtable.size() > kDefs[kGetProjection].slot) {
            void* const table = g.blockVtable.data();
            std::memcpy(k.block.data(), &table, sizeof table);
        } else {
            g.blockVtable.clear();
        }
    }
    Log("%ls: render queue: ProjectToScreen %s", moduleName,
        g.cameraLayout ? "answered beside the flush from the camera copied at the hand-off (its viewport and matrices)"
                       : "joins the flush (its code doesn't read the camera's expected fields)");
    // a taken SetCamera's matrices: SetCamera on the block, its code reading the fields copied into it, and its call of
    // the camera-info method (by that method's code) detoured for the block
    if (g.cameraLayout && g.setCamera) {
        const char* const set = static_cast<const char*>(g.setCamera);
        bool ok = IsInCode(base, set, kSetCameraScan);
        for (const size_t d : {kCamFlags, kCamScreen, kCamScreen + 4, kCamScreen + 8, kCamScreen + 12, kViewStack,
                               kProjStack}) {
            const uint32_t d32 = uint32_t(d);
            bool found = false;
            for (size_t i = 0; ok && i + 4 <= kSetCameraScan && !found; ++i) found = std::memcmp(set + i, &d32, 4) == 0;
            ok = ok && found;
        }
        char* info = nullptr;
        const auto infoCode = ParsePattern(kCameraInfoCode);
        for (size_t i = 0; ok && !info && i + 5 <= kSetCameraScan; ++i) {
            if (uint8_t(set[i]) != 0xE8) continue;
            int32_t rel;
            std::memcpy(&rel, set + i + 1, 4);
            const char* const t = set + i + 5 + rel;
            if (IsInCode(base, t, kCameraInfoBytes) && MatchesAt(t, infoCode)) info = const_cast<char*>(t);
        }
        if (info && InstallHook(info, reinterpret_cast<void*>(&CameraInfoHook), &g.cameraInfo, "camera info")) {
            Log("%ls: render queue: a taken SetCamera's matrices made by SetCamera on the camera copy's block (its "
                "camera-info method +0x%llX returning for it)",
                moduleName, static_cast<unsigned long long>(info - base));
        } else {
            g.cameraInfo = nullptr;
            Log("%ls: render queue: the matrices after a taken SetCamera join the flush (SetCamera's code or its "
                "camera-info call isn't as expected)",
                moduleName);
        }
    }
    // GetViewport: a leaf reading the same four words (their displacements in its first bytes)
    if (g.cameraLayout) {
        const char* const fn = static_cast<const char*>(g.fns[kGetViewport]);
        constexpr size_t kViewportScan = 48;
        bool ok = IsInCode(base, fn, kViewportScan);
        for (uint32_t w = 0; w < 4 && ok; ++w) {
            const uint32_t d = uint32_t(kViewport + 4 * w);
            bool found = false;
            for (size_t i = 0; i + 4 <= kViewportScan && !found; ++i) found = std::memcmp(fn + i, &d, 4) == 0;
            ok = found;
        }
        g.viewportLayout = ok;
    }
    Log("%ls: render queue: GetViewport %s", moduleName,
        g.viewportLayout ? "answered beside the flush from the same copy" : "joins the flush");
    for (const auto& k : kVerifyKeys)
        if (_wcsicmp(ConfigString(L"ShadowPrepass", k[0], k[1], L"off").c_str(), L"verify") == 0) g.verifyCalls = true;
    g.installed = true;
    if (g.verifyCalls)
        Log("%ls: render queue: a part in verify mode calls vanilla from the flush: the main thread joins it at its "
            "first renderer call",
            moduleName);
    Log("%ls: render queue: %d calls taken beside the flush (DeleteRenderMesh +0x%X, ...), %d answered and checked at "
        "the join",
        moduleName, int(kFirstAnswered), unsigned(static_cast<char*>(g.fns[kDeleteRenderMesh]) - base),
        int(kKinds - kFirstAnswered));
    ConsoleGate_Install();  // (what CrySystem's console calls of the renderer's without a vtable)
    return true;
#else
    (void)moduleName;
    (void)base;
    (void)endEf3D;
    return false;
#endif
}

bool RenderQueue_Passes(unsigned gateIndex) {
#ifdef _WIN64
    // only while the flush runs no scene traversal (the 3D engine's code, with its locks and its streaming, beside the
    // main thread's: a deadlock once seen)
    if (!g.installed || g.verifyCalls || !ReflectionPrepass_Early()) return false;
    // (the traversal's recorder: its producer calls answered without the renderer, and the next traversal itself
    // beside the flush once it can answer them, FlushThreadRecord=beside)
    if (TraversalRecord_Passes(gateIndex)) return true;
    if (g.barrier >= 0 && unsigned(g.barrier) == gateIndex && TraversalRecord_TakesTraversal()) {
        ++g.barrierPassed;
        return true;
    }
    for (int p : g.passes)
        if (unsigned(p) == gateIndex) return true;
    for (int p : g.getterPasses)
        if (unsigned(p) == gateIndex) return true;
    for (int p : g.resPasses)
        if (unsigned(p) == gateIndex) return true;
    for (int p : g.purePasses)
        if (unsigned(p) == gateIndex) return true;
#else
    (void)gateIndex;
#endif
    return false;
}

void RenderQueue_BeforeFlush(char* renderer) {
#ifdef _WIN64
    Camera& k = g.camera;
    k.valid = false;
    if (g.installed && renderer) g.renderer = renderer;
    g.cameraOf = nullptr;
    if (g.installed && renderer && g.cameraAt) {  // (the camera the flush will leave, compared at the join)
        std::memcpy(g_cameraHandOff, renderer + g.cameraAt, kCameraBytes);
        std::memcpy(g_cameraAnswer, g_cameraHandOff, kCameraBytes);
        g.cameraVersions = 0;
        g.cameraOf = renderer;
    }
    if (g.installed && !g.barrierTried) {  // (the 3D engine is loaded by the first flush)
        g.barrierTried = true;
        // (off by default: with BeginFrame taken, the main thread goes on into code that may destroy what the parts'
        // after-flush steps write, and joins there with the game's locks held; see SysEventHook)
        if (_wcsicmp(ConfigString(L"ShadowPrepass", L"FlushThreadBarrier", L"OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD_BARRIER",
                                  L"off")
                         .c_str(),
                     L"on") != 0)
            Log("render queue: no barrier (FlushThreadBarrier=off): BeginFrame joins the flush");
        else
            InstallBarrier();
    }
    if (g.installed && renderer && !g.windowLogged) {  // (what the window's messages go through, once)
        g.windowLogged = true;
        HWND const w = static_cast<HWND>(Fn<PtrFn>(kGetHWND)(renderer));
        if (w && IsWindow(w)) {
            const bool wide = IsWindowUnicode(w) != FALSE;
            const LONG_PTR proc = wide ? GetWindowLongPtrW(w, GWLP_WNDPROC) : GetWindowLongPtrA(w, GWLP_WNDPROC);
            char where[128];
            Log("render queue: the game window's procedure is %s (%s): what it calls of the renderer's without a "
                "vtable doesn't go through the gate",
                DescribeAddress(reinterpret_cast<const void*>(proc), where, sizeof where), wide ? "unicode" : "ansi");
            FlushThread_WatchWindowProc(reinterpret_cast<void*>(proc));  // (FlushThreadPresent)
        }
    }
    if (!g.installed || !g.cameraLayout || !renderer) return;
    const float* const* view = *reinterpret_cast<const float* const* const*>(renderer + kViewStack);
    const float* const* proj = *reinterpret_cast<const float* const* const*>(renderer + kProjStack);
    if (!view || !*view || !proj || !*proj) return;
    std::memcpy(k.view, *view, kMatrixBytes);
    std::memcpy(k.proj, *proj, kMatrixBytes);
    std::memcpy(g_matrixHandOff[0], k.view, kMatrixBytes);
    std::memcpy(g_matrixHandOff[1], k.proj, kMatrixBytes);
    std::memcpy(k.block.data() + kViewport, renderer + kViewport, kViewportBytes);
    std::memcpy(g.handOffViewport, renderer + kViewport, kViewportBytes);
    if (g.cameraInfo) {  // (what SetCamera reads of the renderer's, besides the camera and the stacks)
        std::memcpy(k.block.data() + kCamFlags, renderer + kCamFlags, 4);
        std::memcpy(k.block.data() + kCamScreen, renderer + kCamScreen, kCamScreenBytes);
    }
    k.renderer = renderer;
    k.valid = true;
    k.matrices = true;
    k.version = 0;
#else
    (void)renderer;
#endif
}

void RenderQueue_Replay() {
#ifdef _WIN64
    CompareCamera();
    CompareOcean();
    CompareFlushCamera();
    g.window = false;
    // (before the calls: what the flush counted, as the first call after it reads; the worker's copy, if it made the
    // frame's end after it)
    if (g.left.valid)
        g.stats = g.left.stats;
    else
        StashStats(g.renderer);
    g.left.valid = false;
    if (g.calls.empty()) return;
    ++g.joins;
    g.most = (std::max)(g.most, uint64_t(g.calls.size()));
    g.replayCount = unsigned(g.calls.size());
    for (size_t i = 0; i < g.calls.size(); ++i) {  // in their order: what vanilla's would have been
        const Call& c = g.calls[i];
        g.replayAt = unsigned(i) + 1;
        g.replayKind = c.kind;
        const uint64_t t0 = __rdtsc();
        if (c.kind < kFirstAnswered)
            Make(c);
        else
            Check(c);
        g.replayCycles[c.kind] += __rdtsc() - t0;
    }
    g.replayAt = 0;
    g.calls.clear();
#endif
}

void RenderQueue_AfterFlush() {
#ifdef _WIN64
    State::Left& l = g.left;
    l.valid = false;
    if (!g.installed || !g.window) return;
    char* const r = g.camera.renderer;  // (set at the hand-off, as the camera's copy below: the main thread's now)
    l.view = l.proj = false;
    if (r && g.cameraLayout) {
        std::memcpy(l.viewport, r + kViewport, kViewportBytes);
        const float* const* const views = *reinterpret_cast<const float* const* const*>(r + kViewStack);
        const float* const* const projs = *reinterpret_cast<const float* const* const*>(r + kProjStack);
        l.view = views && *views;
        l.proj = projs && *projs;
        if (l.view) std::memcpy(g_matrixLeft[0], *views, kMatrixBytes);
        if (l.proj) std::memcpy(g_matrixLeft[1], *projs, kMatrixBytes);
    }
    l.camera = g.cameraOf && g.cameraAt;
    if (l.camera) std::memcpy(g_cameraLeft, g.cameraOf + g.cameraAt, kCameraBytes);
    l.stats.valid = false;
    StashStats(g.renderer, l.stats);
    l.valid = true;
#endif
}

void RenderQueue_MakeSealed() {
#ifdef _WIN64
    if (g.sealed.empty()) return;
    ++g.workerFrames;
    for (const Call& c : g.sealed) {  // (in their order, as at a join; the rest follow at the join)
        const uint64_t t0 = __rdtsc();
        if (c.kind < kFirstAnswered)
            Make(c);
        else
            Check(c);
        g.workerCycles[c.kind] += __rdtsc() - t0;
    }
    g.sealed.clear();
#endif
}

void RenderQueue_OnHandOff() {
#ifdef _WIN64
    if (!g.locks.empty()) ++g.textJoins;
    g.locks.clear();
    g.textUsed = 0;
    g.depth2d = 0;
    g.strings.clear();
    g.aoNodes.clear();
    g.sfUsed = 0;
    g.pendingFrames = 0;
    g.scenesDone = false;
    g.auxFlags.set = false;
    g.refs.clear();
    g.oceanDrawn = g.oceanStayed = g.oceanDrawSeen = g.oceanPublished = false;
    g.oceanAnswer = nullptr;
    g.oceanAnswerOf = nullptr;
    g.oceanPublishTo = nullptr;
    if (g.oceanUpdate) {  // (the grid updated at the flush's start, or not in this flush at all: final from the start)
        g.oceanPre = g.oceanAdded;
        g.oceanAdded = nullptr;
        if (!g.oceanPre) {
            g.oceanDrawn = g.oceanStayed = g.oceanPublished = true;
            ++g.oceanStays;
        }
    }
    g.scenesFree = !ReflectionPrepass_MayRunScenes();
    g.pastBeginFrame = false;
    g.handOffAt = __rdtsc();
    g.uploadUsed = 0;
    g.resUsed = 0;
    g.windowSets = 0;
    g.windowAt = g.handOffAt;
    if (g.marks) ResetEvent(g.marks);  // (the flush hasn't started: its marks come after)
    g.window = true;
#endif
}

void RenderQueue_OnScenePassesDone() {
#ifdef _WIN64
    g.scenesDone = true;
    g.scenesFree = true;  // (the reflections are rendered by now)
    if (g.marks && g.window) SetEvent(g.marks);
#endif
}

void RenderQueue_NoteAddEf(void* re) {
#ifdef _WIN64
    if (g.oceanVt && re && *static_cast<void**>(re) == g.oceanVt && !OnFlushThread()) g.oceanAdded = re;
#else
    (void)re;
#endif
}

void RenderQueue_OnScenesFree() {
#ifdef _WIN64
    if (!g.window || !OnFlushThread()) return;
    g.scenesFree = true;
    if (g.marks) SetEvent(g.marks);
#endif
}

void RenderQueue_OnSceneRun() {
#ifdef _WIN64
    if (!g.window || !OnFlushThread()) return;
    ++g.sceneRuns;
    if (g.scenesFree) ++g.sceneRunsUnforeseen;  // (the main thread may be in its next update by now)
    if (g.pastBeginFrame) ++g.sceneRunsInUpdate;
#endif
}

void RenderQueue_OnFlushEnd() {
#ifdef _WIN64
    if (g.window && g.oceanUpdate) {  // (the hand-off's guess checked: an ocean added is drawn, and none is drawn else)
        if (g.oceanStayed && g.oceanDrawSeen) ++g.oceanLate;
        if (!g.oceanStayed && !g.oceanDrawSeen) ++g.oceanUndrawn;
    }
    if (g.window && g.oceanPublishTo) {  // (added, not drawn: the grid stays last frame's, as in vanilla)
        g.oceanPublishTo = nullptr;
        g.oceanPublished = true;
    }
    if (g.marks && g.window) SetEvent(g.marks);
#endif
}

void RenderQueue_OnFlushStart() {
#ifdef _WIN64
    char* const re = static_cast<char*>(g.oceanPre);
    if (!re || !g.window) return;
    g.oceanPre = nullptr;
    // (as the ocean's draw: with its geometry, FrameUpdate; without, the draw won't update the grid either). Before its
    // first ocean draw the flush samples the grid too (the water level at the camera, which the reflections' camera
    // takes): there it has last frame's, as in vanilla, put back after the update, which that draw puts in
    if (*reinterpret_cast<int*>(re + kOceanVerts) && *reinterpret_cast<int*>(re + kOceanIndices) &&
        *reinterpret_cast<void**>(re + kOceanVB) && *reinterpret_cast<void**>(re + kOceanIB)) {
        size_t bytes = 0, after = 0;
        uint8_t* const grid = const_cast<uint8_t*>(OceanVector(re, &bytes));
        if (!grid) {  // (no grid yet to put back: this window's answers wait for the flush's first ocean draw)
            ++g.oceanNoGrid;
            return;
        }
        g.oceanOld.assign(grid, grid + bytes);
        const uint64_t c0 = __rdtsc();
        reinterpret_cast<OceanUpdateFn>(g.oceanUpdate)(re);
        ++g.oceanPreUpdates;
        g.oceanPreCycles += __rdtsc() - c0;
        if (OceanVector(re, &after) == grid && after == bytes) {
            g.oceanNew.assign(grid, grid + bytes);
            std::memcpy(grid, g.oceanOld.data(), bytes);
            g.oceanAnswer = g.oceanNew.data();
            g.oceanAnswerOf = re;
            g.oceanPublishTo = grid;
        } else {
            ++g.oceanUnrestored;  // (the flush has the update from its start)
            g.oceanPublished = true;
        }
    } else {
        g.oceanPublished = true;
    }
    g.oceanDrawn = true;
    if (g.marks) SetEvent(g.marks);
#endif
}

void RenderQueue_Where(char* buffer, unsigned size) {
    snprintf(buffer, size, "not installed");
#ifdef _WIN64
    if (!g.installed) return;
    const unsigned at = g.replayAt, count = g.replayCount;
    const Kind kind = g.replayKind;
    if (at)
        snprintf(buffer, size, "making the calls taken beside the flush, at %u of %u (%s)", at, count, kDefs[kind].name);
    else
        snprintf(buffer, size, "not making the taken calls (%zu taken in this window)", g.calls.size());
#endif
}

void RenderQueue_Report(const char* when) {
#ifdef _WIN64
    if (!g.installed) return;
    const double n = double(g.joins ? g.joins : 1);
    char line[2000] = "";
    size_t used = 0;
    for (int k = 0; k < kKinds; ++k) {
        if (!g.counts[k]) continue;
        const int w = k < kFirstAnswered
                          ? snprintf(line + used, sizeof line - used, "%s%s %.2f", used ? ", " : "", kDefs[k].name,
                                     double(g.counts[k]) / n)
                          : snprintf(line + used, sizeof line - used, "%s%s %.2f (%s %llu)", used ? ", " : "",
                                     kDefs[k].name, double(g.counts[k]) / n,
                                     k >= kFirstLagged ? "one frame late: differing" : "differing after it",
                                     g.differ[k]);
        if (w < 0 || size_t(w) >= sizeof line - used) break;
        used += size_t(w);
    }
    Log("render queue [%s]: calls beside the flush per join: %s | %llu joins with any, most at once %llu", when,
        used ? line : "none", g.joins, g.most);
    // (the calls' time, the costliest first: at the joins, per join; on the flush's thread, per frame's end it made)
    const auto costs = [when](const uint64_t* cycles, double per, const char* what) {
        int order[kKinds];
        int kinds = 0;
        uint64_t all = 0;
        for (int k = 0; k < kKinds; ++k) {
            all += cycles[k];
            if (cycles[k]) order[kinds++] = k;
        }
        std::sort(order, order + kinds, [cycles](int a, int b) { return cycles[a] > cycles[b]; });
        char list[1200] = "";
        size_t cu = 0;
        for (int i = 0; i < kinds && i < 12; ++i) {
            const int w = snprintf(list + cu, sizeof list - cu, "%s%s %.1f", cu ? ", " : "", kDefs[order[i]].name,
                                   double(cycles[order[i]]) / 3.0e3 / per);
            if (w < 0 || size_t(w) >= sizeof list - cu) break;
            cu += size_t(w);
        }
        if (all) Log("render queue [%s]: %s: %.1f in all | %s", when, what, double(all) / 3.0e3 / per, list);
    };
    costs(g.replayCycles, n, "the calls made at the joins, us per join");
    costs(g.workerCycles, double(g.workerFrames ? g.workerFrames : 1),
          "the frame's end made on the flush's thread, us per frame");
    if (g.seals || g.sealsRefused) {  // (FlushThreadPresent: the frames' ends sealed, and why not)
        char by[600] = "";
        size_t bu = 0;
        for (int k = 0; k < kKinds; ++k) {
            if (!g.refusedBy[k]) continue;
            const int w = snprintf(by + bu, sizeof by - bu, "%s%s %llu", bu ? ", " : "", kDefs[k].name, g.refusedBy[k]);
            if (w < 0 || size_t(w) >= sizeof by - bu) break;
            bu += size_t(w);
        }
        Log("render queue [%s]: the frame's end sealed at EndFrame for the flush's thread: %llu, made there %llu; not "
            "sealed, a call in it the join's to make: %llu (%s)",
            when, g.seals, g.workerFrames, g.sealsRefused, bu ? by : "-");
    }
    if (g.cameraLayout)
        Log("render queue [%s]: ProjectToScreen from the hand-off's camera: answered %llu, joined %llu (in 2D mode, or "
            "no camera); UnProjectFromScreen answered %llu (differing after it %llu), joined %llu | the camera the "
            "flush left: the hand-off's at %llu of %llu joins (differing: viewport %llu, view %llu, projection %llu)",
            when, g.counts[kProjectToScreen], g.projectJoins, g.counts[kUnProject], g.differ[kUnProject],
            g.unprojectJoins, g.cameraSame, g.cameraCompared, g.cameraDiffer[kCamViewport], g.cameraDiffer[kCamView],
            g.cameraDiffer[kCamProj]);
    char pools[300] = "";
    size_t pu = 0;
    for (int p = 0; p < kPools; ++p) {
        const Pool& q = g.pools[p];
        if (!q.samples && !q.contradicted) continue;
        const int w = snprintf(pools + pu, sizeof pools - pu, "%spool %d: %u bytes a vertex (%u draws%s)",
                               pu ? ", " : "", p, q.stride, q.samples,
                               q.contradicted ? ", contradicted: its rooms stay measured" : "");
        if (w < 0 || size_t(w) >= sizeof pools - pu) break;
        pu += size_t(w);
    }
    Log("render queue [%s]: the text's vertex buffer: %s | windows with text taken %llu, locks taken %llu (measured "
        "%llu), the renderer's locks that failed at the join %llu, locks not taken (the buffer full) %llu",
        when, pu ? pools : "no draw measured", g.textJoins, g.locksTaken, g.locksMeasured, g.locksFailed,
        g.textMisses);
    if (g.counts[kPostSet] || g.counts[kPostSetVec4] || g.counts[kPostSetString] || g.postJoins)
        Log("render queue [%s]: the post effects' parameters set beside the flush: %llu, %llu Vec4, %llu strings; "
            "joined for a name or string too long to copy %llu",
            when, g.counts[kPostSet], g.counts[kPostSetVec4], g.counts[kPostSetString], g.postJoins);
    if (g.eventJoins || g.labelJoins || g.frameIdJoins || g.viewportJoins || g.scenesWaits)
        Log("render queue [%s]: joined at the renderer's OnSystemEvent %llu, at a Draw2dLabel too long to copy %llu, at "
            "GetFrameID(true) %llu (it waited for the flush's scene passes %llu times, %.1f us each), at GetViewport "
            "without the camera %llu",
            when, g.eventJoins, g.labelJoins, g.frameIdJoins, g.scenesWaits,
            g.scenesWaits ? double(g.scenesWaitCycles) / 3.0e3 / double(g.scenesWaits) : 0.0, g.viewportJoins);
    if (g.counts[kFlashRender] || g.flashReleaseJoins)
        Log("render queue [%s]: the Flash players' renders taken beside the flush: %llu (releases of a player with one "
            "taken, joined first: %llu)",
            when, g.counts[kFlashRender], g.flashReleaseJoins);
    if (g.sfTaken)
        Log("render queue [%s]: the Flash players' draws taken beside the flush: triangle lists %llu, line strips %llu, "
            "glyphs %llu, stencil masks %llu, flushes %llu, clears %llu; their copies %.1f KB per join | joined: %llu "
            "(no room %llu, a vertex format the draw doesn't copy %llu)",
            when, g.counts[kSFDrawTriList], g.counts[kSFDrawLineStrip], g.counts[kSFDrawGlyphClear],
            g.counts[kSFConfigMask], g.counts[kSFFlush], g.counts[kClearBuffer], double(g.sfCopied) / 1024.0 / n,
            g.sfJoins, g.sfJoins - g.sfOddFormat, g.sfOddFormat);
    if (g.counts[kEF_Query] || g.counts[kEF_QuerySetting] || g.queryJoins)
        Log("render queue [%s]: EF_Query beside the flush: EFQ_DrawNearFov taken %llu, settings answered %llu "
            "(differing after it %llu), the other queries joined %llu",
            when, g.counts[kEF_Query], g.counts[kEF_QuerySetting], g.differ[kEF_QuerySetting], g.queryJoins);
    if (g.counts[kFieldGetter] || g.counts[kShaderAddRef] || g.counts[kTextureAddRef]) {
        char slots[1200] = "";
        size_t su = 0;
        for (unsigned gc = 0; gc < kGetterClasses; ++gc)
            for (unsigned s = 0; s < kGetterSlots; ++s) {
                if (!g.getterCounts[gc][s]) continue;
                const int w = snprintf(slots + su, sizeof slots - su, "%s%s slot %u %llu (differing %llu)",
                                       su ? ", " : "", kGetterNames[gc], s, g.getterCounts[gc][s],
                                       g.getterDiffer[gc][s]);
                if (w < 0 || size_t(w) >= sizeof slots - su) break;
                su += size_t(w);
            }
        Log("render queue [%s]: the field getters answered beside the flush: %s | AddRef taken: a shader's %llu, a "
            "texture's %llu; a shader's Release taken %llu (to 0 %llu); their answers differing when made %llu | "
            "SetDefaultMaterials taken %llu",
            when, su ? slots : "none", g.counts[kShaderAddRef], g.counts[kTextureAddRef], g.counts[kShaderRelease],
            g.releasesToZero, g.refsDiffer, g.counts[kSetDefaultMaterials]);
    }
    if (g.oceanDraw &&
        (g.counts[kOceanGrid] || g.counts[kOceanPoint] || g.oceanDrawWindows || g.oceanPreUpdates || g.oceanStays))
        Log("render queue [%s]: the ocean beside the flush: its grid answered %llu, a point %llu (differing after it "
            "%llu, %llu); they waited for the grid's update %llu times, %.1f us each (%llu until the flush's end) | "
            "the grid updated at the flush's start %llu times (%.1f us each; put in at its first ocean draw %llu, "
            "failed %llu; in from the start %llu; left to the draw, no grid yet %llu), final at the hand-off with no "
            "ocean added %llu; drawn though none was added %llu, added but not drawn %llu; a point waiting for the "
            "update to be put in %llu | the flush's first draw where it updates the grid %.2f ms after the hand-off "
            "(%llu windows) | the grid as at a window's first answer, changed by the join: %llu of %llu",
            when, g.counts[kOceanGrid], g.counts[kOceanPoint], g.differ[kOceanGrid], g.differ[kOceanPoint],
            g.oceanWaits, g.oceanWaits ? double(g.oceanWaitCycles) / 3.0e3 / double(g.oceanWaits) : 0.0, g.oceanEnded,
            g.oceanPreUpdates, g.oceanPreUpdates ? double(g.oceanPreCycles) / 3.0e3 / double(g.oceanPreUpdates) : 0.0,
            g.oceanPublishes, g.oceanPublishFailed, g.oceanUnrestored, g.oceanNoGrid, g.oceanStays, g.oceanLate,
            g.oceanUndrawn, g.oceanPointWaits,
            g.oceanDrawWindows ? double(g.oceanDrawCycles) / 3.0e6 / double(g.oceanDrawWindows) : 0.0,
            g.oceanDrawWindows, g.oceanChanged, g.oceanCompared);
    if (g.sceneRuns || g.sceneWaits)
        Log("render queue [%s]: the flush's scene traversals beside the main thread: %llu (after the flush had none "
            "left %llu, with the main thread in its next update %llu) | its next update waited for them at BeginFrame "
            "%llu times, %.1f us each",
            when, g.sceneRuns, g.sceneRunsUnforeseen, g.sceneRunsInUpdate, g.sceneWaits,
            g.sceneWaits ? double(g.sceneWaitCycles) / 3.0e3 / double(g.sceneWaits) : 0.0);
    if (g.postMgr && (g.counts[kPostGet] || g.counts[kPostGetVec4] || g.postGetJoins))
        Log("render queue [%s]: the post effects' getters beside the flush: answered %llu floats (%llu of them a bool's "
            "or an int's) and %llu Vec4s (%llu from a setter taken before them, %llu of a name with none; differing "
            "after them %llu, %llu), joined %llu (a blending or other parameter)",
            when, g.counts[kPostGet], g.postGetOther, g.counts[kPostGetVec4], g.postGetTaken, g.postGetMissing,
            g.differ[kPostGet], g.differ[kPostGetVec4], g.postGetJoins);
    if (g.cameraAt && g.camerasCompared) {
        char parts[600] = "";
        size_t cu = 0;
        for (size_t p = 0; p < kCameraParts; ++p) {
            if (!g.cameraParts[p]) continue;
            const int w = snprintf(parts + cu, sizeof parts - cu, "%s+0x%zX %llu", cu ? ", " : "", p * 16,
                                   g.cameraParts[p]);
            if (w < 0 || size_t(w) >= sizeof parts - cu) break;
            cu += size_t(w);
        }
        const uint64_t same = g.camerasCompared - g.camerasDiffer;
        Log("render queue [%s]: the camera the flush left: the hand-off's at %llu of %llu joins | where not, its parts "
            "differing: %s; its members (all but the padding) in %llu | the flush's SetCamera calls per flush: %.2f "
            "where it left the hand-off's, %.2f where not (the last %.2f ms after the hand-off) | GetCamera answered "
            "%llu from the hand-off's copy or a taken SetCamera's (differing after it %llu), joined %llu | SetCamera "
            "taken %llu, joined %llu",
            when, same, g.camerasCompared, cu ? parts : "none", g.camerasMembers,
            same ? double(g.setsSame) / double(same) : 0.0,
            g.camerasDiffer ? double(g.setsDiffer) / double(g.camerasDiffer) : 0.0,
            g.lastSetsDiffer ? double(g.lastSetCyclesDiffer) / 3.0e6 / double(g.lastSetsDiffer) : 0.0,
            g.counts[kGetCamera], g.differ[kGetCamera], g.cameraJoins, g.counts[kSetCamera], g.setCameraJoins);
    if (g.counts[kGetModelView] || g.counts[kGetProjection] || g.matrixJoins || g.standInSets)
        Log("render queue [%s]: the matrices beside the flush: GetModelViewMatrix answered %llu, GetProjectionMatrix "
            "%llu, from the hand-off's camera or a taken SetCamera's (differing after them %llu, %llu), joined %llu "
            "(other matrices) | a taken SetCamera's made on the block %llu times, %.1f us each (the renderer's at the "
            "join the same %llu, differing %llu); not made within a taken 2D mode %llu",
            when, g.counts[kGetModelView], g.counts[kGetProjection], g.differ[kGetModelView], g.differ[kGetProjection],
            g.matrixJoins, g.standInSets, g.standInSets ? double(g.standInCycles) / 3.0e3 / double(g.standInSets) : 0.0,
            g.standInSame, g.standInDiffer, g.standIn2d);
    }
    if (!g.resPasses.empty() && (g.counts[kResRef] || g.resJoins))
        Log("render queue [%s]: the shader resources' references beside the flush: answered %llu (the caller's "
            "writes made at the join %llu; the others differing after them %llu), joined %llu (the room full)",
            when, g.counts[kResRef], g.resWrites, g.differ[kResRef], g.resJoins);
    if (g.counts[kMeshPosPtr] || g.posJoins || g.counts[kMeshIndices] || g.counts[kSetViewport])
        Log("render queue [%s]: a render mesh's positions read beside the flush: answered %llu (differing after them "
            "%llu), joined %llu (a write, or no system stream); its indices %llu (differing %llu) | SetViewport taken "
            "%llu",
            when, g.counts[kMeshPosPtr], g.differ[kMeshPosPtr], g.posJoins, g.counts[kMeshIndices],
            g.differ[kMeshIndices], g.counts[kSetViewport]);
    if (g.counts[kDownLoad] || g.uploadJoins) {
        const double each = g.counts[kDownLoad] ? double(g.uploadBytes) / 1024.0 / double(g.counts[kDownLoad]) : 0.0;
        Log("render queue [%s]: DownLoadToVideoMemory beside the flush: taken %llu (%.1f KB copied each; their answers "
            "differing when made %llu), joined %llu (a new texture, a format not known, or the room full)",
            when, g.counts[kDownLoad], each, g.uploadDiffer, g.uploadJoins);
    }
    ConsoleGate_Report(when);
#else
    (void)when;
#endif
}

}  // namespace optiperf
