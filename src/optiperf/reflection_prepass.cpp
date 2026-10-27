// Reflections and the flush's preprocess stage: the next 3D-engine work to move out of the renderer's flush, after the
// shadow casters (shadow_prepass.cpp). x64 CryRenderD3D10.dll only. Details: research/docs/reflections.md.
//
// At level 1 the flush (EF_EndEf3D → list processing) runs a preprocess stage before the shadow maps: a handler
// (+0xA6330) goes over the preprocess list's flagged items and, per item and flag,
//   - updates render targets (FX_DrawToRenderTarget). The ocean's reflection renders the scene through
//     I3DEngine::RenderWorld with a mirrored camera, i.e. the 3D engine's whole scene traversal runs inside the flush;
//   - updates panorama impostors (CREPanoramaCluster), also through RenderWorld;
//   - generates far-tree sprites (I3DEngine::GenerateFarTrees).
// A render thread can't run that. And because the reflections come before the shadow maps, the shadow pre-pass's
// casters, rendered at the flush entry, see a frame id one lower than vanilla's in frames that update one.
//
//   - observe: changes nothing. At the flush entry it predicts what the preprocess stage will do, by running the game's
//     own handler on a sorted copy of the preprocess list with every handler stubbed out; in the flush it records what
//     happens, the RenderWorld calls and their cost, and which renderer methods a reflection's traversal calls (a
//     counting copy of the renderer's vtable).
//   - on: at the flush entry (before the shadow pre-pass) the same run calls FX_DrawToRenderTarget for real, as a dry
//     run, after doing what EF_EndEf3D and its list processing do before the preprocess stage (frame time, flags, the
//     HDR scene's flag, the level's polygon items, the per-list setup). Its RenderWorld runs the 3D traversal there; the traversal's nested
//     EF_EndEf3D is taken instead of run: its items go into a capture, with the level's lights, fill lights and polygon
//     count, and the renderer setters it called (camera, fog, sky light) are recorded through the vtable copy. Then the
//     state FX_DrawToRenderTarget and the setters changed is put back (the update decision and its state, the cached
//     camera, fog, sky light), except the frame id until the shadow pre-pass has run. In the flush,
//     FX_DrawToRenderTarget runs as in vanilla, and its RenderWorld call replays the capture instead: setters, the
//     level, the lights, the items, the polygon count, the nested flush. The 3D engine no longer runs there.
//     The handler dispatches its entries sorted by type (panoramas 0x11, render targets 0x13..0x16, far trees 0x17,
//     0x18). Hoisting stops at the first entry it can't take along (panoramas, far trees, render targets other than
//     the pooled ones: see Hoistable): that one and everything after it stay in the flush, so the order of the 3D
//     engine's work is the game's. A capture whose traversal called a renderer method outside the known set is not
//     replayed: its traversal runs in the flush after all.
//   - verify: as on, and each replayed RenderWorld call compares its arguments and the renderer state it is made in
//     with the dry run's, and the state its nested flush starts from with the one the traversal left.
//
// Far trees (FarTrees=on|verify, with the above; research/docs/far-trees.md): the dry run also runs GenerateFarTrees at
// its place in the stage (sprite quads and impostors from 3D-engine data), and the flush skips it. Right before the
// flush, DrawFarTrees runs with DrawObjSprites recorded instead of drawn, and the flush's DrawFarTrees calls replay the
// record. verify generates them again in the flush and compares the quads, and checks each draw decision.
//
// Dynamic texture sources (DynTextures=on|verify, with the above): a render target without textures is a Flash or
// video texture's, whose Update (advance the movie, render it into its texture; rate-limited) FX_DrawToRenderTarget
// calls. The dry run makes that call at its place in the stage, and the flush's is skipped. verify compares what the
// update decides from (source, the object's distance, the frame time) at the flush's call with the dry run's.
//
// [ShadowPrepass] Reflections = off (default) | observe | on | verify     (env OPTIPERF_SHADOW_PREPASS_REFLECTIONS)
//                 FarTrees    = off (default) | on | verify               (env OPTIPERF_SHADOW_PREPASS_FARTREES)
//                 DynTextures = off (default) | on | verify               (env OPTIPERF_SHADOW_PREPASS_DYNTEXTURES)

#include "reflection_prepass.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "calltrace.h"
#include "config.h"
#include "flush_thread.h"
#include "hooks.h"
#include "log.h"
#include "render_queue.h"
#include "scan.h"
#include "shadow_prepass.h"
#include "shadow_prepass_lists.h"
#include "vtable_counter.h"

namespace optiperf {
namespace {

#ifdef _WIN64
// --- patterns (x64 CryRenderD3D10.dll, build 6156) --------------------------------------------------------------------
// Offsets are those of the rel32 instruction (E8 byte) or of the 4-byte displacement inside each pattern.

// the preprocess handler (renderer, items, start, end), its prologue
constexpr const char kPreHead[] =
    "48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 B8 B0 60 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B F1 48 "
    "8B 0D ?? ?? ?? ?? 45 33 E4 48 8B 01 0F 29 B4 24 A0 60 00 00 45 8B F1 49 63 F8 48 8B DA 41 8B EC "
    "44 89 A4 24 00 61 00 00";
constexpr size_t kPreSize = 0x9E6;
// its dispatch over the collected (type, item) entries: returns unless the render callback at +0xB28 is the flush's;
// 0x11 with a panorama element (type 0xB): CREPanoramaCluster::UpdateImposter; 0x13..0x16: the technique's render
// targets, then the resources'; 0x17: I3DEngine::GenerateFarTrees; 0x18: the resources' render targets (all but 0x11
// and 0x17 skipped inside a reflection, m_PersFlags bit 16)
constexpr const char kPreDispatch[] =
    "48 8D 05 ?? ?? ?? ?? 49 39 86 28 0B 00 00 0F 85 ?? ?? ?? ?? 85 ED 0F 84 ?? ?? ?? ?? 48 8D 9C 24 "
    "A8 00 00 00 66 66 90 66 66 90 66 66 90 48 8B 53 10 48 85 D2 0F 84 ?? ?? ?? ?? 4C 8B 2B 48 8B 4B "
    "20 49 8D 45 14 49 89 8E 88 0A 00 00 4D 89 AE 78 0A 00 00 49 89 86 80 0A 00 00 8B 43 F8 83 F8 11 "
    "0F 84 C5 01 00 00 83 F8 12 0F 8E ?? ?? ?? ?? 83 F8 16 0F 8E C7 00 00 00 83 F8 17 0F 84 9D 00 00 "
    "00 83 F8 18 0F 85 ?? ?? ?? ?? 41 0F BA A6 F4 0A 00 00 10 0F 82 ?? ?? ?? ?? 8B 43 08 48 8B 6B 18 "
    "41 B8 00 00 00 00 85 C0 41 8B F0 41 0F 48 C0 44 39 85 F0 00 00 00 8B C8 48 8B 42 58 4C 8B 24 C8 "
    "0F 86 ?? ?? ?? ?? 49 8B F8 66 66 66 90 48 8B 85 E8 00 00 00 48 8B 0C 07 83 79 0C 00 75 2C 48 8B "
    "43 20 48 8B 53 10 4D 8B CD 48 89 44 24 38 8B 43 F8 4C 8B C5 89 44 24 30 48 89 4C 24 28 49 8B CE "
    "4C 89 64 24 20 E8 ?? ?? ?? ?? 83 C6 01 48 83 C7 08 3B B5 F0 00 00 00 72 B4 E9 ?? ?? ?? ?? 48 8B "
    "0D ?? ?? ?? ?? 48 8B 01 FF 90 E0 00 00 00 48 8B C8 48 8B 10 FF 92 40 01 00 00 E9 ?? ?? ?? ?? 41 "
    "0F BA A6 F4 0A 00 00 10 0F 82 ?? ?? ?? ?? 8B 43 08 48 8B 7B 18 41 B8 00 00 00 00 85 C0 41 8B E8 "
    "41 0F 48 C0 8B C8 48 8B 42 58 4C 8B 24 C8 45 39 44 24 40 76 54 49 8B F0 66 90 66 66 90 49 8B 44 "
    "24 38 48 8B 0C 06 83 79 0C 00 75 2C 48 8B 43 20 48 8B 53 10 4D 8B CD 48 89 44 24 38 8B 43 F8 4C "
    "8B C7 89 44 24 30 48 89 4C 24 28 49 8B CE 4C 89 64 24 20 E8 ?? ?? ?? ?? 83 C5 01 48 83 C6 08 41 "
    "3B 6C 24 40 72 B7 45 33 C0 48 85 FF 74 68 83 BF F0 00 00 00 00 41 8B F0 76 5C 49 8B E8 48 8B 87 "
    "E8 00 00 00 48 8B 0C 28 83 79 0C 00 75 2C 48 8B 43 20 48 8B 53 10 4D 8B CD 48 89 44 24 38 8B 43 "
    "F8 4C 8B C7 89 44 24 30 48 89 4C 24 28 49 8B CE 4C 89 64 24 20 E8 ?? ?? ?? ?? 83 C6 01 48 83 C5 "
    "08 3B B7 F0 00 00 00 72 B4 EB 0B 83 79 08 0B 75 05 E8 ?? ?? ?? ??";
constexpr int kDispCallback = 0x03, kDispSystem = 0x121, kDispPanorama = 0x231;
constexpr int kDispDrtt[] = {0x105, 0x1B3, 0x215};
// the per-list processing, its preprocess branch: render callback +0xB28, current object +0xA78 and +0xAB8 = the
// frame's first render object, +0xA80 its matrix; then for list 1 whose first item has flags (batch flags &
// 0xFFFF0000), the per-list setup (renderer, list), the handler on lists[nAW][1], the per-list teardown (renderer)
constexpr const char kListPre[] =
    "83 FD 01 48 8B 83 A8 41 01 00 4C 8B 74 24 78 4C 63 6C 24 70 4C 89 B3 28 0B 00 00 48 8B 08 48 8D "
    "41 14 48 89 8B 78 0A 00 00 48 89 8B B8 0A 00 00 48 89 83 80 0A 00 00 75 49 49 8B F5 4C 8D 3D ?? "
    "?? ?? ?? 48 8D 0C BF 48 69 F6 F0 00 00 00 4A 8B 04 3E F7 44 C8 24 00 00 FF FF 74 26 8B D5 48 8B "
    "CB E8 ?? ?? ?? ?? 4A 8B 14 3E 45 8B CC 44 8B C7 48 8B CB E8 ?? ?? ?? ?? 48 8B CB 03 F8 E8 ?? ?? "
    "?? ??";
constexpr int kListPreLists = 0x3F, kListPreStart = 0x61, kListPreHandler = 0x73, kListPreEnd = 0x7D;
// the list processing at level 1: the preprocess list if nFlags & 0x20 and not m_PersFlags bit 24 (then the shadow
// pass, which the shadow pre-pass's kGate matches)
constexpr const char kGatePre[] =
    "45 85 FF 48 8D 35 ?? ?? ?? ?? 75 5F 45 85 F6 74 43 0F BA A3 F4 0A 00 00 18 72 39 48 63 05 ?? ?? "
    "?? ?? 48 8D 0D ?? ?? ?? ?? 44 88 7C 24 30 45 8D 4F 01 48 89 6C 24 28 44 89 7C 24 20 48 6B C0 78 "
    "44 8B 44 08 8C 8B 94 30 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ??";
constexpr int kGateBase = 0x06, kGateLevel = 0x1E, kGateStarts = 0x48, kGateList = 0x4F;
// the preprocess list's sort (items, count): std::sort (introsort, then insertion sort) with its comparator
constexpr const char kSort[] =
    "48 89 5C 24 10 57 48 83 EC 30 48 63 C2 48 8B D9 48 8D 14 80 48 8D 3C D1 48 3B CF 74 6D 48 8B D7 "
    "48 B8 67 66 66 66 66 66 66 66 45 33 C9 48 2B D1 48 89 74 24 40 48 F7 EA 48 C1 FA 04 48 8B C2 48 "
    "C1 E8 3F 48 03 D0 48 83 FA 01 74 11 66 66 66 90 48 D1 FA 49 83 C1 01 48 83 FA 01 75 F3 48 8D 35 "
    "?? ?? ?? ?? 4D 03 C9 45 33 C0 48 8B D7 48 89 74 24 20 E8 ?? ?? ?? ?? 4C 8B C6 48 8B D7 48 8B CB "
    "E8 ?? ?? ?? ?? 48 8B 74 24 40 48 8B 5C 24 48 48 83 C4 30 5F C3";
// and the list sort's call of it for the preprocess list: &lists[nAW][1].elems[start], count
constexpr const char kSortCall[] =
    "48 8D 05 ?? ?? ?? ?? 4B 8D 0C B6 48 03 FE 45 8B CC 44 8B C6 48 03 FF 8B D5 48 8B 84 F8 ?? ?? ?? "
    "?? 48 8D 0C C8 E8 ?? ?? ?? ??";
constexpr int kSortCallBase = 0x03, kSortCallLists = 0x1D, kSortCallSort = 0x25;
// FX_DrawToRenderTarget (renderer, shader, resources, object, technique, render target, type, element): its start,
// through its first checks of the render target: flags +0x60 bit 13 (nothing to do), textures +0x38 and +0x40. Both
// null: a dynamic texture source's (a Flash or video texture in the resources' texture slot +0x68, of 0x14 at +0x48),
// whose Update(the object's distance +0xE0) it calls, if there is one (+0x148), and returns
constexpr const char kDrttHead[] =
    "4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 56 57 41 56 48 81 EC 90 07 00 00 4C 8B B4 24 D8 07 "
    "00 00 49 8B F1 48 8B C2 4D 85 F6 48 8B F9 75 0E 32 C0 48 81 C4 90 07 00 00 41 5E 5F 5E C3 41 8B "
    "56 60 0F BA E2 0D 89 54 24 78 72 E4 48 89 AC 24 88 07 00 00 49 8B 6E 38 48 85 ED 75 3F 49 39 6E "
    "40 75 39 49 63 46 68 85 C0 78 2A 83 F8 14 7D 25 49 8B 4C C0 48 48 8B 89 48 01 00 00 48 85 C9 74 "
    "14 48 8B 01 F3 41 0F 10 89 E0 00 00 00 FF 50 10 E9 ?? ?? ?? ?? 32 C0 E9 ?? ?? ?? ??";
constexpr size_t kDrttSize = 0x234B;  // 9035 bytes in build 6156
// then the target's pool id (+0x2C) and size (+0x24); a negative id and a texture: the texture's type (+0xF8). Types
// 1, 0x17 and 2 are environment textures, found per camera with their own update state
constexpr const char kDrttPoolId[] = "41 8B 46 2C 48 89 9C 24 B0 07 00 00 41 8B 5E 24";
constexpr const char kDrttTexType[] = "48 85 ED 0F 84 ?? ?? ?? ?? 8B 85 F8 00 00 00 41 3B C0 0F 85 ?? ?? ?? ??";
// a water reflection's update decision and its state: the decision flag, the update state +0x1810..0x1840 (fov, time,
// direction, position), the per-frame history +0x1841, the update counter
constexpr const char kDrttState[] =
    "C6 05 ?? ?? ?? ?? 00 EB 07 C6 05 ?? ?? ?? ?? 01 48 8B 07 33 D2 48 8B CF FF 90 00 04 00 00 44 8B "
    "D8 B8 67 66 66 66 41 F7 EB C1 FA 02 8B CA C1 E9 1F 03 D1 8D 0C 92 03 C9 44 2B D9 80 3D ?? ?? ?? "
    "?? 00 0F 84 92 00 00 00 8B 87 E4 0A 00 00 48 8D 8C 24 90 00 00 00 44 3B E3 F3 0F 11 B7 10 18 00 "
    "00 89 87 38 18 00 00 8B 01 C6 84 24 D8 07 00 00 01 89 87 14 18 00 00 8B 41 04 89 87 18 18 00 00 "
    "8B 41 08 48 8D 8C 24 A0 00 00 00 89 87 1C 18 00 00 8B 01 89 87 20 18 00 00 8B 41 04 89 87 24 18 "
    "00 00 8B 41 08 48 8D 8C 24 80 00 00 00 89 87 28 18 00 00 8B 01 89 87 2C 18 00 00 8B 41 04 89 87 "
    "30 18 00 00 8B 41 08 89 87 34 18 00 00 0F 9F C0 33 C9 88 87 40 18 00 00 EB 1E 83 3D ?? ?? ?? ?? "
    "00 74 4E 48 8B 05 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 3B 88 CC 8F 02 00 73 39 49 63 C3 83 C1 01 C6 84 "
    "38 41 18 00 00 01 89 0D ?? ?? ?? ??";
constexpr int kStateFlag[] = {0x02, 0x0B, 0x3D};  // mov byte [flag], 0 / 1; cmp byte [flag], 0 (each with an imm8)
constexpr int kStateCounter[] = {0xEC, 0x108};    // mov ecx, [counter]; mov [counter], ecx
// its RenderWorld call: r_NoDrawNear = 1 (and a cvar 0 below the water), I3DEngine::RenderWorld(3, camera (the cached
// ocean camera, or one on the stack), "FX_DrawToRenderTarget", draw flags, the target's filter flags), restored after
constexpr const char kDrttWorld[] =
    "45 0F 2F FE 44 8B 25 ?? ?? ?? ?? 8B 1D ?? ?? ?? ?? B8 01 00 00 00 89 05 ?? ?? ?? ?? 73 0A C7 05 "
    "?? ?? ?? ?? 00 00 00 00 4C 8B 16 48 8D 05 ?? ?? ?? ?? 4C 8D 84 24 10 02 00 00 45 84 FF 4C 8D 0D "
    "?? ?? ?? ?? 48 8B CE 4C 0F 45 C0 41 8B 46 64 89 44 24 28 89 54 24 20 BA 03 00 00 00 41 FF 52 10 "
    "44 89 25 ?? ?? ?? ?? 89 1D ?? ?? ?? ?? 81 A7 F4 0A 00 00 FF FF FF BF C6 87 00 3E 01 00 00";
constexpr int kWorldNoDrawNear[] = {0x07, 0x18, 0x63}, kWorldCvar[] = {0x0D, 0x69}, kWorldCvarZero = 0x20;
constexpr int kWorldOceanCamera = 0x2E, kWorldName = 0x40, kWorldReturn = 0x60;
// and at its end, the frame id with recursion bumped (+0x28F68)
constexpr const char kDrttFrame[] =
    "48 8B CF E8 ?? ?? ?? ?? 48 8B 84 24 48 01 00 00 83 87 68 8F 02 00 01 48 89 87 A8 0C 00 00";
// CREPanoramaCluster::UpdateImposter (element), prologue
constexpr const char kPanoramaHead[] =
    "48 89 5C 24 20 55 41 54 41 56 48 81 EC E0 02 00 00 8B 05 ?? ?? ?? ?? 45 33 F6 4C 8B E1 A8 01 48 "
    "8D 1D ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 83 C8 01 44 89 35 ?? ?? ?? ??";
// renderer setters a reflection's traversal calls for its flush: EnableFog (+0x287F8), SetFog (+0x287F0..0x28817;
// it then finishes the color into +0x28818 from the sky light), SetSkyLightRenderParams (+0x28FA0); and the level's
// fill lights (EF_ADDFillLight: 24-byte entries at +0x17A0 + level*16, at most 0x200)
constexpr const char kEnableFog[] = "0F B6 81 F8 87 02 00 88 91 F8 87 02 00 C3";
constexpr const char kSetFog[] =
    "48 83 EC 38 8B 44 24 68 F3 0F 11 89 FC 87 02 00 F3 0F 11 91 00 88 02 00 89 81 F0 87 02 00 89 81 "
    "F4 87 02 00 48 8B 44 24 60 F3 0F 11 99 04 88 02 00";
constexpr const char kSetSkyLight[] = "48 89 91 A0 8F 02 00 C3";
constexpr const char kAddFillLight[] =
    "48 83 EC 48 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 48 89 5C 24 50 48 63 1D ?? ?? ?? ?? 48 8B C3 48 "
    "03 C0 81 BC C1 A8 17 00 00 00 02 00 00 0F 83 ?? ?? ?? ?? 8B 02 4C 8D 44 24 20 48 81 C3 7A 01 00 00";
constexpr int kFillLevel = 0x19;
// EF_EndEf3D's start of a flush, which the dry runs come before: the frame time (+0xAE4) from the timer
// (ITimer::GetCurrTime(ETIMER_GAME)), and the two cvars it sets to 0 and 1 while flushing with flag 0x400 (probably
// r_ShadersAsyncCompiling and r_TexturesStreamingSync)
constexpr const char kEndEfTime[] =
    "48 8B 0D ?? ?? ?? ?? 33 D2 48 89 AC 24 60 01 00 00 48 8B 01 48 89 B4 24 68 01 00 00 FF 50 10 33 "
    "C0 0F BA E7 0A F3 0F 11 83 E4 0A 00 00 8B 35 ?? ?? ?? ?? 8B 2D ?? ?? ?? ?? 73 10 89 05 ?? ?? ?? "
    "?? C7 05 ?? ?? ?? ?? 01 00 00 00";
constexpr int kEndEfTimer = 0x03;
constexpr int kEndEfAsyncRead[] = {0x2F, 0x35};  // mov esi / ebp, [cvar]: kept to put back after the flush
constexpr int kEndEfAsyncSet[] = {0x3D, 0x43};   // = 0 / = 1 (the second with its imm32 after)
constexpr size_t kEndEf3DSize = 0x3B0;           // 944 bytes in build 6156
// then, with flag 0x20, the level's polygons (EF_AddPolygonToScene keeps them per level) turned into render items:
// those with preprocess flags (+0x44) go to the preprocess list as well, built as below (AddPolygonItems)
constexpr const char kPolyItems[] =
    "41 55 41 56 41 57 48 83 EC 30 48 63 05 ?? ?? ?? ?? 45 33 F6 4C 8D 3D ?? ?? ?? ?? 48 C1 E0 04 45 "
    "8B EE 46 39 74 38 08 0F 86 ?? ?? ?? ?? 48 89 5C 24 50 48 89 6C 24 58 4C 89 64 24 20 48 89 74 24 "
    "60 4D 8B E6 48 8D 2D ?? ?? ?? ?? 48 89 7C 24 28 4A 8B 04 38 49 8B 1C 04 44 39 73 44 0F 84 ?? ?? "
    "?? ?? 48 63 0D ?? ?? ?? ?? 48 8B 7B 48 48 8B F1 83 C1 01 3B 0D ?? ?? ?? ?? 89 0D ?? ?? ?? ?? 76 "
    "3E 8B C1 D1 E8 8D 54 08 0A 85 D2 89 15 ?? ?? ?? ?? 75 0C 49 8B D6 48 89 15 ?? ?? ?? ?? EB 27 48 "
    "8B 0D ?? ?? ?? ?? 48 8D 14 92 48 C1 E2 03 E8 ?? ?? ?? ?? 48 8B D0 48 89 05 ?? ?? ?? ?? EB 07 48 "
    "8B 15 ?? ?? ?? ?? 4C 8D 04 B6 4A 89 7C C2 18 8B 47 10 0F BF 8F CC 00 00 00 25 00 00 FF FF 0B C8 "
    "42 89 4C C2 10 8B 87 8C 00 00 00 42 89 44 C2 20 8B 43 44 83 C8 01 42 89 44 C2 24 48 8B 43 38 48 "
    "85 C0 74 05 8B 48 08 EB 03 41 8B CE 48 8B 43 30 C1 E1 0C 0B 48 14 8B 43 40 4A 89 5C C2 08 83 E0 "
    "3F C1 E1 06 0B C8 42 89 0C C2";
constexpr int kPolyLevel = 0x0D, kPolyStorage = 0x17, kPolyRealloc = 0xAE;
constexpr int kPolyCount[] = {0x65, 0x7B}, kPolyElems[] = {0x99, 0xA2, 0xB9, 0xC2};  // list 1's (lists[0][1])
// Far-tree sprites. I3DEngine::GenerateFarTrees (the preprocess stage's type 0x17) hands the 3D engine's far-object
// list to the renderer's GenerateObjSprites, which builds one quad per sprite (0x38 bytes, from the camera, the 3D
// engine's lighting and its statobjs), renders stale impostors (MakeSprites: IStatObj::Render into "$SpriteTex"
// textures) and sorts the quads. I3DEngine::DrawFarTrees, called by CREFarTreeSprites::mfDraw while drawing, then
// calls DrawObjSprites, which draws those quads at level 1 only. The quad list's growth and sort in GenerateObjSprites:
constexpr const char kSpriteQuads[] =
    "48 63 0D ?? ?? ?? ?? F3 44 0F 10 8C 24 98 00 00 00 F3 44 0F 10 94 24 9C 00 00 00 F3 44 0F 10 84 "
    "24 A0 00 00 00 48 8B D9 83 C1 01 3B 0D ?? ?? ?? ?? F3 44 0F 59 CE F3 44 0F 59 D6 89 0D ?? ?? ?? "
    "?? F3 44 0F 59 C6 76 44 8B C1 D1 E8 8D 54 08 0A 85 D2 89 15 ?? ?? ?? ?? 75 0C 49 8B C4 48 89 05 "
    "?? ?? ?? ?? EB 2D 48 8B 0D ?? ?? ?? ?? 48 6B D2 38 E8 ?? ?? ?? ?? 4C 8B 44 24 70 4C 8B 8C 24 60 "
    "02 00 00 48 89 05 ?? ?? ?? ?? EB 07 48 8B 05 ?? ?? ?? ?? 4C 8B F3";
constexpr int kQuadCount[] = {0x03, 0x3D}, kQuadElems[] = {0x60, 0x69, 0x86, 0x8F}, kQuadRealloc = 0x71;
constexpr const char kSpriteSort[] = "48 8B 3D ?? ?? ?? ?? 8B 1D ?? ?? ?? ?? 48 6B DB 38 48 03 DF 48 3B FB 74 68";
constexpr int kSortElems = 0x03, kSortCount = 0x09;
constexpr int kQuadSize = 0x38, kQuadFields = 0x31;  // a quad, and the part of it GenerateObjSprites writes
// DrawObjSprites (renderer slot 161), its start: nothing unless at level 1
constexpr const char kDrawSprites[] = "40 55 48 83 EC 40 83 3D ?? ?? ?? ?? 01 48 8B E9 0F 85 ?? ?? ?? ?? F6 81 00 0F 00 00 08";
constexpr int kDrawSpritesLevel = 0x08;

// offsets fixed in the patterns above
// SHRenderTarget, as FX_DrawToRenderTarget reads it
constexpr int kRtTexture = 0x38, kRtTexture2 = 0x40, kRtPoolId = 0x2C, kRtKind = 0x30, kRtFlags = 0x60;
constexpr int kTexType = 0xF8;
constexpr int kRtTexSlot = 0x68, kResTextures = 0x48, kResTextureSlots = 0x14, kTexDynSource = 0x148;
constexpr int kObjDistance = 0xE0;  // CRenderObject's distance, which a dynamic texture source's update gets
constexpr int kRPersFlags = 0xAF4, kRPersFlags2 = 0xAF8, kRCallback = 0xB28, kRDeviceLost = 0xA60;
// the current object, its matrix and element: set by the dispatch, and by the per-list setup (with +0xAB8) to the
// frame's first render object
constexpr int kRCurObject = 0xA78, kRCurObjectMatrix = 0xA80, kRCurElement = 0xA88, kRPrevObject = 0xAB8;
constexpr int kRObjectArray = 0x141A8, kObjMatrix = 0x14;
constexpr int kRPreprocessTime = 0x13F58;  // the handler adds its time here
constexpr int kRUpdateState = 0x1810, kUpdateStateSize = 0x48;  // a water reflection's update state, +0x1810..0x1857
constexpr int kRFrameRec = 0x28F68;
constexpr int kRFog = 0x287F0, kFogSize = 0x38, kRSkyLight = 0x28FA0;
constexpr int kRLights = 0xF40, kRFillLights = 0x17A0, kFillLightSize = 24;  // per level, TArray (16 bytes) each
constexpr size_t kLightBytes = 0x258;  // CDLight (light_record.h)
constexpr int kRNFlags = 0xEF8, kRHdrActive = 0xEFC, kRTime = 0xAE4;  // the list processing's nFlags, HDR; frame time
constexpr int kRHdrFormat = 0x287A8;  // (the shadow pre-pass's kHdr)
constexpr int kRClip = 0x13DF0, kClipSize = 17;  // a reflection's clip plane, then its enable byte (+0x13E00)
constexpr int kRRtDepth = 0x2CEEC, kRObjects = 0x141A0, kRMaxObjects = 0x4000;
// pointers to the current shader, object, ... in the renderer (verify compares them)
constexpr int kRPtrs[] = {0xA68, 0xA78, 0xA80, 0xA88, 0xA90, 0xAB0, 0xAB8, 0xCA8, 0x141B0};
// CREClientPoly (a polygon of EF_AddPolygonToScene) and what its render item is made of (kPolyItems)
constexpr int kPolyShader = 0x30, kPolyResources = 0x38, kPolyTechnique = 0x40, kPolyFlags = 0x44, kPolyObject = 0x48;
constexpr int kShaderId = 0x14, kResourcesId = 8, kObjFlags = 0x10, kObjRAEId = 0xCC, kObjDynLM = 0x8C;
constexpr uint32_t kItemFlagsMask = 0xFFFF0000;  // preprocess flags: the upper half of an item's batch flags
constexpr size_t kCamBytes = 0x214, kCamCopy = 0x218;  // CCamera, as its assignment copies it; the static's size
// I3DEngine (Mod SDK slots, confirmed by the calls above): RenderWorld 2, DrawFarTrees 39, GenerateFarTrees 40;
// ISystem::GetI3DEngine
constexpr int kVfRenderWorld = 0x10, kVfDrawFarTrees = 0x138, kVfGenerateFarTrees = 0x140, kVfGetI3DEngine = 0xE0;
constexpr int kVfGetCurrTime = 0x10;  // ITimer, as EF_EndEf3D calls it
// IRenderer (Mod SDK slots; the setters and DrawObjSprites are checked against their code at the first flush)
constexpr int kSlotSetCamera = 43, kSlotEnableFog = 138, kSlotSetFog = 139, kSlotSetSkyLight = 193;
constexpr int kSlotDrawObjSprites = 161;
constexpr int kVfGetViewport = 0xA0, kVfGetCamera = 0x160, kVfGetFrameId = 0x400, kVfStartEf = 0x380, kVfEndEf3D = 0x398;
// what a reflection's traversal may call from outside the renderer (slots): queries, producers, resources, its level,
// and the recorded setters; anything else makes its capture unsafe (replayed by running the traversal after all)
constexpr int kAllowedSlots[] = {7,   20,  43,  44,  58,  59,  65,  67,  68,  69,  83,  87,  92,  98,  103, 104,
                                 111, 112, 113, 114, 115, 116, 117, 118, 121, 126, 127, 128, 138, 139, 143, 193,
                                 194, 196, 199, 213};
constexpr int kObjectsReserve = 6000;  // left free for a reflection's render objects (they stay until the flush ends)
constexpr int kMaxCaptures = 8;
#endif  // _WIN64

enum class RMode { Off, Observe, On, Verify };

const char* RModeName(RMode m) {
    return m == RMode::Observe ? "observe" : m == RMode::On ? "on" : m == RMode::Verify ? "verify" : "off";
}

struct RStats {
    uint64_t flushes = 0, preprocessFlushes = 0;
    // predicted (the stubbed or dry run at the flush entry) and seen in the flush, per handler
    uint64_t predDrtt = 0, predPanorama = 0, predFarTrees = 0, polyItems = 0;
    uint64_t seenDrtt = 0, seenPanorama = 0, seenFarTrees = 0, seenDrttDraw = 0;  // draw: outside the preprocess stage
    uint64_t sequencesSame = 0, sequencesDiffer = 0, notPredicted = 0;
    // I3DEngine::RenderWorld inside the flush (observe; on: the ones left there): from FX_DrawToRenderTarget, elsewhere
    uint64_t worldDrtt = 0, worldDrttCycles = 0, worldOther = 0, worldOtherCycles = 0, worldFrames = 0;
    uint64_t preprocessCycles = 0, predictCycles = 0, otherThread = 0;
    int maxEntries = 0;
    // on/verify: the dry runs at the flush entry, and their replay in the flush
    uint64_t hoistFlushes = 0, notReady = 0, noRoom = 0, captures = 0, capturesEmpty = 0, capturesUnsafe = 0;
    uint64_t hoistCycles = 0, stopPanorama = 0, stopFarTrees = 0, stopTarget = 0, stopDynTexture = 0, stopWorld = 0;
    uint64_t noopTargets = 0, cameraBuilt = 0;
    // dynamic texture sources (Flash, video): updated in the dry run, the flush's calls skipped or left there
    uint64_t dynEarly = 0, dynSkipped = 0, dynInFlush = 0, dynUnused = 0, dynCycles = 0;
    uint64_t replayed = 0, replayFallback = 0, unused = 0, putBackFailed = 0, replayCycles = 0, setters = 0;
    uint64_t stackLights = 0;  // lights on the traversal's stack, copied into the captures
    uint64_t polygons = 0, polygonsAdded = 0;
    int maxCaptureObjects = 0, unsafeSlot = -1;
    // far trees: sprites generated before the flush, and the generation and draw calls in the flush (left there,
    // skipped, replayed from the decision recorded right before the flush)
    uint64_t farEarly = 0, farEarlyCycles = 0, farSkipped = 0, farInFlush = 0, farInFlushCycles = 0, farQuads = 0;
    uint64_t drawRecorded = 0, drawCalls = 0, drawReplayed = 0, drawNone = 0, drawOriginal = 0;
    uint64_t drawInReplay = 0;  // DrawFarTrees calls inside a replayed reflection's flush (vanilla: the reflection's list)
    // verify: the replayed calls' arguments and inputs against the dry run's; their flushes' starting state
    uint64_t vCalls = 0, vCallsSame = 0, vInputsSame = 0, vInputsDiffer = 0, vLogged = 0;
    uint64_t vField[15] = {};
    uint64_t vFlushes = 0, vFlushSame = 0;
    uint64_t vFlushField[10] = {};
    // verify, far trees: the quads generated in the flush again against the early ones; the draw decision
    uint64_t vQuads = 0, vQuadsSame = 0, vDraws = 0, vDrawsSame = 0;
    // verify, dynamic textures: what an update decides from, at the flush's call against the dry run's
    uint64_t vDyn = 0, vDynSame = 0;
};

#ifdef _WIN64
const char* const kFieldNames[15] = {"m_PersFlags", "m_PersFlags2", "nFlags",         "HDR",        "frame time",
                                     "frame id with recursion",     "frame id",       "level",      "r_NoDrawNear",
                                     "cvar +0x2E980C",              "render-target depth", "clip plane", "current objects",
                                     "viewport",                    "renderer camera"};
const char* const kFlushNames[10] = {"m_PersFlags",         "m_PersFlags2",  "fog",       "sky light",
                                     "viewport",            "clip plane",    "render-target depth",
                                     "frame id with recursion", "current objects", "renderer camera"};

using PreprocessFn = int (*)(char* renderer, char* items, unsigned start, unsigned end);
using DrttFn = uint64_t (*)(char* renderer, void* shader, void* resources, void* object, void* technique, void* rt,
                            int type, void* element);
using PanoramaFn = uint64_t (*)(void* element);
using SortFn = void (*)(void* items, int count);
using StartBatchFn = void (*)(char* renderer, int list);
using EndBatchFn = void (*)(char* renderer);
using RenderWorldFn = void (*)(void* engine, int flags, const void* camera, const char* name, int drawFlags,
                               int filterFlags);
using EngineFn = void (*)(void* engine);
using GetI3DEngineFn = void* (*)(void* system);
using GetCurrTimeFn = float (*)(void* timer, int which);
using RendererFn = void (*)(char* renderer);
using EndEf3DFn = void (*)(char* renderer, int flags);
using GetFrameIdFn = int (*)(char* renderer, bool includeRecursive);
using GetViewportFn = void (*)(char* renderer, int* x, int* y, int* w, int* h);
using GetCameraFn = const char* (*)(char* renderer);
using SetCameraFn = void (*)(char* renderer, const void* camera);
using EnableFogFn = bool (*)(char* renderer, bool enable);
using SetFogFn = void (*)(char* renderer, float density, float fogStart, float fogEnd, const float* color, int mode);
using SetSkyLightFn = void (*)(char* renderer, const void* params);
using DrawSpritesFn = void (*)(char* renderer, void* list, float maxViewDist, void* objManager, float zoom);

enum Kind : uint8_t { kDrtt, kPanorama, kFarTrees };
struct Event {  // one call the preprocess stage makes
    Kind kind;
    int type;
    void* rt;
    void* element;
    void* object;
    bool operator==(const Event& o) const {
        return kind == o.kind && type == o.type && rt == o.rt && element == o.element && object == o.object;
    }
};

struct Inputs {  // what a RenderWorld call is made with, besides its arguments
    uint32_t p4, p8, nFlags;
    uint8_t hdr;
    float time;
    int frameRec, frame, level, noDrawNear, cvar, rtDepth;
    uint8_t clip[kClipSize];
    void* ptrs[9];
    int viewport[4];
    uint8_t camera[kCamBytes];
};

struct FlushState {  // what a reflection's nested flush starts from, besides its items, lights and polygons (verify)
    uint32_t p4, p8;
    uint8_t fog[kFogSize];
    void* sky;
    int viewport[4];
    uint8_t clip[kClipSize];
    int rtDepth, frameRec;
    void* ptrs[9];
    uint8_t camera[kCamBytes];
};

struct Setter {  // a renderer setter a traversal called, and its arguments
    int slot;
    bool afterStart;  // after the traversal's EF_StartEf
    bool enable;
    float density, fogStart, fogEnd;
    float color[4];
    int mode;
    const void* ptr;
    std::vector<uint8_t> camera;
};

struct PtrList {  // TArray<T*>
    void** elems;
    uint32_t count, capacity;
};
struct ByteList {  // TArray<T>, as bytes
    uint8_t* elems;
    uint32_t count, capacity;
};

enum CaptureState { kCapPending = 0, kCapReplayed, kCapStale };
struct Capture {  // one reflection rendered early: its RenderWorld call, and what the traversal left for its flush
    int flags = 0, drawFlags = 0, filterFlags = 0;
    const char* name = nullptr;
    bool ocean = false;
    uint8_t camera[kCamBytes];
    Inputs in;
    bool ended = false, unsafe = false;
    int endFlags = 0, objects = 0, state = kCapPending;
    int polyStart = 0, polyEnd = 0;  // its level's polygon count before the traversal and at its flush
    sp::Stash items;
    std::vector<void*> lights;
    std::vector<uint8_t> stackLights;  // copies of the lights that lived on the traversal's stack (600 bytes each)
    std::vector<uint8_t> fill;
    std::vector<Setter> setters;
    std::vector<uint64_t> calls;  // outside calls per vtable slot before the traversal
    FlushState flush;             // verify: the state its flush started from
};
#endif

struct RState {
    RMode mode = RMode::Off;
    bool installed = false, engineHooked = false, engineFailed = false, full = false;
    ReflectionTarget t;
    const ShadowPrepassTarget* shadow = nullptr;  // what the shadow pre-pass located
    char* base = nullptr;
    size_t size = 0;
#ifdef _WIN64
    PreprocessFn preprocess = nullptr;
    DrttFn drtt = nullptr;
    PanoramaFn panorama = nullptr;
    RenderWorldFn renderWorld = nullptr;
    EngineFn farTrees = nullptr;
    VtableCounter counter;
    std::vector<Event> predicted, seen;
    std::vector<sp::RendItem> copy;
    struct RtInfo {  // one render target the preprocess stage updates
        void* rt;
        int type, poolId, kind, texType;
        uint32_t flags;
        uint64_t calls, worlds, oceanWorlds, cycles;
    };
    std::vector<RtInfo> rts;
    RtInfo* currentRt = nullptr;
    std::vector<Capture> captures;
    Capture* capturing = nullptr;
    const Capture* replaying = nullptr;  // while a replayed capture's nested flush runs
    // far trees
    EngineFn drawFarTrees = nullptr;
    DrawSpritesFn drawSprites = nullptr;
    void* engine = nullptr;
    struct SpriteDraw {  // a DrawObjSprites call, as DrawFarTrees made it (recorded) or makes it (verify)
        bool called;
        void* list;
        float maxViewDist;
        void* objManager;
        float zoom;
        bool operator==(const SpriteDraw& o) const {
            return called == o.called && (!called || (list == o.list && objManager == o.objManager &&
                                                      std::memcmp(&maxViewDist, &o.maxViewDist, 4) == 0 &&
                                                      std::memcmp(&zoom, &o.zoom, 4) == 0));
        }
    };
    SpriteDraw drawRecord{}, drawSeen{};
    std::vector<uint8_t> quads;  // verify: the quads generated before the flush
    // dynamic texture sources
    struct DynUpdate {  // an update the dry run made: the preprocess entry, and what the update decided from
        void* rt;
        void* object;
        void* element;
        int type;
        void* source;
        float distance, time;
        uint64_t result;
    };
    std::vector<DynUpdate> dynUpdates;  // this flush
    size_t dynCursor = 0;
#endif
    char* renderer = nullptr;
    DWORD mainThread = 0;
    bool inFlush = false, dry = false, hoisting = false, stopped = false, inPreprocess = false, predictedValid = false;
    bool rendererCounted = false, counterTried = false, frameHeld = false;
    RMode farMode = RMode::Off, dynMode = RMode::Off;
    bool farReady = false, drawReady = false;   // far-tree hooks in: generation early; draw decision recorded
    bool farEarly = false, drawRecorded = false, recordingDraw = false, watchingDraw = false;  // this flush
    int captureCount = 0, cursor = 0, heldFrameRec = 0;
    int rtOffset = 0;  // render targets the flush has pushed by its preprocess stage and the dry run hasn't (Hoist)
    int worldThisFlush = 0, loggedDiffer = 0, loggedWorld = 0, loggedFallback = 0;
    uint64_t sceneRuns = 0;  // scene traversals run inside the flush on the main thread (ReflectionPrepass_SceneRuns)
    bool scenesLeft = true;  // this flush may run one (ReflectionPrepass_MayRunScenes)
    RStats st;
};
RState g;

RMode ParseMode(const wchar_t* key, const wchar_t* env, bool observe) {
    const std::wstring v = ConfigString(L"ShadowPrepass", key, env, L"off");
    if (observe && _wcsicmp(v.c_str(), L"observe") == 0) return RMode::Observe;
    if (_wcsicmp(v.c_str(), L"on") == 0 || v == L"1") return RMode::On;
    if (_wcsicmp(v.c_str(), L"verify") == 0) return RMode::Verify;
    return RMode::Off;
}
RMode ReadRMode() { return ParseMode(L"Reflections", L"OPTIPERF_SHADOW_PREPASS_REFLECTIONS", true); }
RMode ReadFarMode() { return ParseMode(L"FarTrees", L"OPTIPERF_SHADOW_PREPASS_FARTREES", false); }
RMode ReadDynMode() { return ParseMode(L"DynTextures", L"OPTIPERF_SHADOW_PREPASS_DYNTEXTURES", false); }

#ifdef _WIN64
// the flush's thread (the main thread unless FlushThread=worker): the preprocess stage's calls come from the flush;
// the dry run before it, and the captures, are the main thread's
bool OnFlush() {
    if (OnFlushThread()) return true;
    ++g.st.otherThread;
    return false;
}

const char* KindName(Kind k) { return k == kDrtt ? "draw-to-RT" : k == kPanorama ? "panorama" : "far trees"; }

template <class T>
T& At(char* p, int offset) {
    return *reinterpret_cast<T*>(p + offset);
}

sp::ItemList* Lists() { return reinterpret_cast<sp::ItemList*>(g.shadow->lists); }

// a renderer method, from its own vtable (also while the counting copy is in)
void* Method(char* r, int offset) {
    return g.rendererCounted ? g.counter.Original(offset / 8) : (*reinterpret_cast<void***>(r))[offset / 8];
}

// the number of polygons a recursion level holds (EF_AddPolygonToScene); a level's flush turns them all into items
int* PolygonCount(int level) { return reinterpret_cast<int*>(g.shadow->polyLists + level * 16 + 8); }

// appends to a log line, truncating instead of failing when it is full
void Append(char* buf, size_t size, size_t& n, const char* fmt, ...) {
    if (n + 1 >= size) return;
    va_list args;
    va_start(args, fmt);
    const int w = vsnprintf(buf + n, size - n, fmt, args);
    va_end(args);
    if (w > 0) n = (std::min)(size - 1, n + size_t(w));
}

bool AllZero(const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}

// What a dry run does with a render target the preprocess stage updates, in FX_DrawToRenderTarget's own order:
//   - kTake: the pooled ones (the ocean's reflection is pool id 0), which it updates in its own code, with the state the
//     dry run puts back; those it returns from at once (FRT_RENDTYPE_CURSCENE);
//   - kNothing: no textures and no dynamic texture source: it returns at once too;
//   - kLeaveDynamic: a dynamic texture source's (Flash, video): its update runs in the dry run with DynTextures=on or
//     verify (the flush's call is skipped), else it stays in the flush;
//   - kLeave: environment textures (found per camera, with their own update state), copies of the scene
//     (FRT_RENDTYPE_COPYSCENE) and of the current object (FRT_RENDTYPE_CUROBJECT).
// What stays in the flush keeps every entry after it there too.
enum Take { kTake, kNothing, kLeaveDynamic, kLeave };
Take Hoistable(void* rtv, void* resourcesv) {
    char* rt = static_cast<char*>(rtv);
    const uint32_t flags = At<uint32_t>(rt, kRtFlags);
    if (flags & 0x2000) return kTake;
    if (!At<char*>(rt, kRtTexture) && !At<void*>(rt, kRtTexture2)) {
        char* res = static_cast<char*>(resourcesv);
        const int slot = At<int>(rt, kRtTexSlot);
        if (slot < 0 || slot >= kResTextureSlots) return kNothing;
        char* tex = res ? At<char*>(res, kResTextures + slot * 8) : nullptr;
        if (!tex) return kLeave;  // (the game would read through it)
        return At<void*>(tex, kTexDynSource) ? kLeaveDynamic : kNothing;
    }
    if (flags & (0x8000 | 0x1000)) return kLeave;
    return At<int>(rt, kRtPoolId) >= 0 ? kTake : kLeave;
}

// the dynamic texture source FX_DrawToRenderTarget updates for a render target without textures (kLeaveDynamic)
void* DynSource(void* rtv, void* resourcesv) {
    char* rt = static_cast<char*>(rtv);
    char* res = static_cast<char*>(resourcesv);
    const int slot = At<int>(rt, kRtTexSlot);
    if (At<char*>(rt, kRtTexture) || At<void*>(rt, kRtTexture2) || !res || slot < 0 || slot >= kResTextureSlots)
        return nullptr;
    char* tex = At<char*>(res, kResTextures + slot * 8);
    return tex ? At<void*>(tex, kTexDynSource) : nullptr;
}

float FrameTime() {
    void* timer = *g.t.timer;
    return timer ? reinterpret_cast<GetCurrTimeFn>((*static_cast<void***>(timer))[kVfGetCurrTime / 8])(timer, 0) : 0.f;
}

bool Allowed(int slot) {
    for (int s : kAllowedSlots)
        if (s == slot) return true;
    return false;
}

// --- the recorded setters (their slots in the vtable copy point here) --------------------------------------------------
Setter* NewSetter(int slot) {
    if (!g.capturing || GetCurrentThreadId() != g.mainThread) return nullptr;
    Setter& s = g.capturing->setters.emplace_back();
    s.slot = slot;
    s.afterStart = *g.shadow->level >= 2;
    return &s;
}
void RecSetCamera(char* r, const void* camera) {
    if (Setter* s = NewSetter(kSlotSetCamera))
        s->camera.assign(static_cast<const uint8_t*>(camera), static_cast<const uint8_t*>(camera) + kCamCopy);
    reinterpret_cast<SetCameraFn>(g.counter.Original(kSlotSetCamera))(r, camera);
}
bool RecEnableFog(char* r, bool enable) {
    if (Setter* s = NewSetter(kSlotEnableFog)) s->enable = enable;
    return reinterpret_cast<EnableFogFn>(g.counter.Original(kSlotEnableFog))(r, enable);
}
void RecSetFog(char* r, float density, float fogStart, float fogEnd, const float* color, int mode) {
    if (Setter* s = NewSetter(kSlotSetFog)) {
        s->density = density;
        s->fogStart = fogStart;
        s->fogEnd = fogEnd;
        std::memcpy(s->color, color, sizeof s->color);
        s->mode = mode;
    }
    reinterpret_cast<SetFogFn>(g.counter.Original(kSlotSetFog))(r, density, fogStart, fogEnd, color, mode);
}
void RecSetSkyLight(char* r, const void* params) {
    if (Setter* s = NewSetter(kSlotSetSkyLight)) s->ptr = params;
    reinterpret_cast<SetSkyLightFn>(g.counter.Original(kSlotSetSkyLight))(r, params);
}

void Apply(char* r, const Setter& s) {
    switch (s.slot) {
        case kSlotSetCamera:
            reinterpret_cast<SetCameraFn>(g.counter.Original(kSlotSetCamera))(r, s.camera.data());
            break;
        case kSlotEnableFog:
            reinterpret_cast<EnableFogFn>(g.counter.Original(kSlotEnableFog))(r, s.enable);
            break;
        case kSlotSetFog:
            reinterpret_cast<SetFogFn>(g.counter.Original(kSlotSetFog))(r, s.density, s.fogStart, s.fogEnd, s.color,
                                                                        s.mode);
            break;
        case kSlotSetSkyLight:
            reinterpret_cast<SetSkyLightFn>(g.counter.Original(kSlotSetSkyLight))(r, s.ptr);
            break;
    }
}

// --- verify --------------------------------------------------------------------------------------------------------------
void ReadInputs(char* r, Inputs& in) {
    in.p4 = At<uint32_t>(r, kRPersFlags);
    in.p8 = At<uint32_t>(r, kRPersFlags2);
    in.nFlags = At<uint32_t>(r, kRNFlags);
    in.hdr = At<uint8_t>(r, kRHdrActive);
    in.time = At<float>(r, kRTime);
    in.frameRec = reinterpret_cast<GetFrameIdFn>(Method(r, kVfGetFrameId))(r, true);
    in.frame = reinterpret_cast<GetFrameIdFn>(Method(r, kVfGetFrameId))(r, false);
    in.level = *g.shadow->level;
    in.noDrawNear = *g.t.noDrawNear;
    in.cvar = *g.t.cvar980c;
    in.rtDepth = At<int>(r, kRRtDepth);
    std::memcpy(in.clip, r + kRClip, kClipSize);
    for (int i = 0; i < 9; ++i) in.ptrs[i] = At<void*>(r, kRPtrs[i]);
    reinterpret_cast<GetViewportFn>(Method(r, kVfGetViewport))(r, &in.viewport[0], &in.viewport[1], &in.viewport[2],
                                                               &in.viewport[3]);
    std::memcpy(in.camera, reinterpret_cast<GetCameraFn>(Method(r, kVfGetCamera))(r), kCamBytes);
}

uint32_t DifferingFields(const Inputs& a, const Inputs& b) {
    uint32_t m = 0;
    auto bit = [&](int i, bool differs) { m |= differs ? 1u << i : 0u; };
    bit(0, a.p4 != b.p4);
    bit(1, a.p8 != b.p8);
    bit(2, a.nFlags != b.nFlags);
    bit(3, a.hdr != b.hdr);
    bit(4, std::memcmp(&a.time, &b.time, 4) != 0);
    bit(5, a.frameRec != b.frameRec);
    bit(6, a.frame != b.frame);
    bit(7, a.level != b.level);
    bit(8, a.noDrawNear != b.noDrawNear);
    bit(9, a.cvar != b.cvar);
    bit(10, a.rtDepth + g.rtOffset != b.rtDepth);  // (the flush's HDR scene target: see Hoist)
    bit(11, std::memcmp(a.clip, b.clip, kClipSize) != 0);
    bit(12, std::memcmp(a.ptrs, b.ptrs, sizeof a.ptrs) != 0);
    bit(13, std::memcmp(a.viewport, b.viewport, sizeof a.viewport) != 0);
    bit(14, std::memcmp(a.camera, b.camera, kCamBytes) != 0);
    return m;
}

void Verify(char* r, const Capture& c, bool callSame) {
    ++g.st.vCalls;
    if (callSame) ++g.st.vCallsSame;
    Inputs now;
    ReadInputs(r, now);
    const uint32_t m = DifferingFields(c.in, now);
    if (!m) {
        ++g.st.vInputsSame;
        if (callSame) return;
    } else {
        ++g.st.vInputsDiffer;
        for (int i = 0; i < 15; ++i)
            if (m & (1u << i)) ++g.st.vField[i];
    }
    if (++g.st.vLogged > 20) return;
    char line[1024] = "";
    size_t n = 0;
    if (!callSame) Append(line, sizeof line, n, " RenderWorld's arguments;");
    for (int i = 0; i < 15; ++i)
        if (m & (1u << i)) Append(line, sizeof line, n, " %s;", kFieldNames[i]);
    if (m & 1) Append(line, sizeof line, n, " m_PersFlags 0x%08X / 0x%08X;", c.in.p4, now.p4);
    if (m & 2) Append(line, sizeof line, n, " m_PersFlags2 0x%08X / 0x%08X;", c.in.p8, now.p8);
    if (m & (1u << 4)) Append(line, sizeof line, n, " frame time %.6f / %.6f;", double(c.in.time), double(now.time));
    if (m & (1u << 5)) Append(line, sizeof line, n, " frame id with recursion %d / %d;", c.in.frameRec, now.frameRec);
    if (m & (1u << 10)) Append(line, sizeof line, n, " render-target depth %d / %d;", c.in.rtDepth, now.rtDepth);
    if (m & (1u << 12))
        for (int i = 0; i < 9; ++i)
            if (c.in.ptrs[i] != now.ptrs[i])
                Append(line, sizeof line, n, " +0x%X %p / %p;", kRPtrs[i], c.in.ptrs[i], now.ptrs[i]);
    Log("reflections: VERIFY difference #%llu at RenderWorld (dry run / flush):%s", g.st.vLogged, line);
}

void ReadFlushState(char* r, FlushState& f) {
    f.p4 = At<uint32_t>(r, kRPersFlags);
    f.p8 = At<uint32_t>(r, kRPersFlags2);
    std::memcpy(f.fog, r + kRFog, kFogSize);
    f.sky = At<void*>(r, kRSkyLight);
    reinterpret_cast<GetViewportFn>(Method(r, kVfGetViewport))(r, &f.viewport[0], &f.viewport[1], &f.viewport[2],
                                                               &f.viewport[3]);
    std::memcpy(f.clip, r + kRClip, kClipSize);
    f.rtDepth = At<int>(r, kRRtDepth);
    f.frameRec = reinterpret_cast<GetFrameIdFn>(Method(r, kVfGetFrameId))(r, true);
    for (int i = 0; i < 9; ++i) f.ptrs[i] = At<void*>(r, kRPtrs[i]);
    std::memcpy(f.camera, reinterpret_cast<GetCameraFn>(Method(r, kVfGetCamera))(r), kCamBytes);
}

// the state a replayed flush starts from against the one the traversal left for it
void CompareFlush(const Capture& c, const FlushState& now) {
    ++g.st.vFlushes;
    const FlushState& a = c.flush;
    uint32_t m = 0;
    auto bit = [&](int i, bool differs) { m |= differs ? 1u << i : 0u; };
    bit(0, a.p4 != now.p4);
    bit(1, a.p8 != now.p8);
    bit(2, std::memcmp(a.fog, now.fog, kFogSize) != 0);
    bit(3, a.sky != now.sky);
    bit(4, std::memcmp(a.viewport, now.viewport, sizeof a.viewport) != 0);
    bit(5, std::memcmp(a.clip, now.clip, kClipSize) != 0);
    bit(6, a.rtDepth + g.rtOffset != now.rtDepth);
    bit(7, a.frameRec != now.frameRec);
    bit(8, std::memcmp(a.ptrs, now.ptrs, sizeof a.ptrs) != 0);
    bit(9, std::memcmp(a.camera, now.camera, kCamBytes) != 0);
    if (!m) {
        ++g.st.vFlushSame;
        return;
    }
    for (int i = 0; i < 10; ++i)
        if (m & (1u << i)) ++g.st.vFlushField[i];
    if (++g.st.vLogged > 20) return;
    char line[1024] = "";
    size_t n = 0;
    for (int i = 0; i < 10; ++i)
        if (m & (1u << i)) Append(line, sizeof line, n, " %s;", kFlushNames[i]);
    if (m & 1) Append(line, sizeof line, n, " m_PersFlags 0x%08X / 0x%08X;", a.p4, now.p4);
    if (m & 2) Append(line, sizeof line, n, " m_PersFlags2 0x%08X / 0x%08X;", a.p8, now.p8);
    if (m & 4)
        for (int i = 0; i < kFogSize; i += 4)
            if (std::memcmp(a.fog + i, now.fog + i, 4) != 0) {
                Append(line, sizeof line, n, " fog +0x%X first;", kRFog + i);
                break;
            }
    if (m & (1u << 8))
        for (int i = 0; i < 9; ++i)
            if (a.ptrs[i] != now.ptrs[i]) Append(line, sizeof line, n, " +0x%X %p / %p;", kRPtrs[i], a.ptrs[i], now.ptrs[i]);
    Log("reflections: VERIFY difference #%llu at a replayed flush (traversal / replay):%s", g.st.vLogged, line);
}

// --- hooks -------------------------------------------------------------------------------------------------------------
int PreprocessHook(char* r, char* items, unsigned start, unsigned end) {
    if (!g.inFlush || g.dry || g.hoisting || !OnFlush()) return g.preprocess(r, items, start, end);
    const uint64_t t0 = __rdtsc();
    g.inPreprocess = true;
    const int n = g.preprocess(r, items, start, end);
    g.inPreprocess = false;
    g.st.preprocessCycles += __rdtsc() - t0;
    return n;
}

uint64_t DrttHook(char* r, void* shader, void* resources, void* object, void* technique, void* rt, int type,
                  void* element) {
    if (g.dry) {  // the stubbed run: record and do nothing
        g.predicted.push_back(Event{kDrtt, type, rt, element, object});
        return 1;
    }
    if (g.hoisting) {  // the dry run: for real, while the preprocess stage can still be taken along in order
        g.predicted.push_back(Event{kDrtt, type, rt, element, object});
        if (g.stopped) return 1;
        switch (Hoistable(rt, resources)) {
            case kNothing:
                ++g.st.noopTargets;
                [[fallthrough]];
            case kTake:
                return g.drtt(r, shader, resources, object, technique, rt, type, element);
            case kLeaveDynamic:
                if (g.dynMode != RMode::Off && object) {
                    // a Flash or video texture: its update (rate-limited by the frame time, sized by the object's
                    // distance) runs here, once, at its place in the stage; the flush's call is skipped
                    RState::DynUpdate u{rt, object, element, type, DynSource(rt, resources),
                                        At<float>(static_cast<char*>(object), kObjDistance), FrameTime(), 0};
                    const uint64_t t0 = __rdtsc();
                    u.result = g.drtt(r, shader, resources, object, technique, rt, type, element);
                    g.st.dynCycles += __rdtsc() - t0;
                    g.dynUpdates.push_back(u);
                    ++g.st.dynEarly;
                    return u.result;
                }
                ++g.st.stopDynTexture;
                break;
            case kLeave:
                ++g.st.stopTarget;
                break;
        }
        g.stopped = true;
        return 1;
    }
    if (!g.inFlush || !OnFlush()) return g.drtt(r, shader, resources, object, technique, rt, type, element);
    if (!g.inPreprocess) {
        ++g.st.seenDrttDraw;
        return g.drtt(r, shader, resources, object, technique, rt, type, element);
    }
    g.seen.push_back(Event{kDrtt, type, rt, element, object});
    if (g.dynCursor < g.dynUpdates.size()) {  // a dynamic texture source updated in the dry run: not again
        const RState::DynUpdate& u = g.dynUpdates[g.dynCursor];
        if (u.rt == rt && u.object == object && u.element == element && u.type == type) {
            ++g.dynCursor;
            ++g.st.dynSkipped;
            if (g.dynMode == RMode::Verify) {
                ++g.st.vDyn;
                const float distance = At<float>(static_cast<char*>(object), kObjDistance), time = FrameTime();
                if (DynSource(rt, resources) == u.source && std::memcmp(&distance, &u.distance, 4) == 0 &&
                    std::memcmp(&time, &u.time, 4) == 0) {
                    ++g.st.vDynSame;
                } else if (++g.st.vLogged <= 20) {
                    Log("reflections: VERIFY difference #%llu, dynamic texture %p: source %p / %p, distance %.4f / %.4f, "
                        "frame time %.6f / %.6f (dry run / flush)",
                        g.st.vLogged, rt, u.source, DynSource(rt, resources), double(u.distance), double(distance),
                        double(u.time), double(time));
                }
            }
            return u.result;
        }
    }
    if (DynSource(rt, resources)) ++g.st.dynInFlush;
    RState::RtInfo* info = nullptr;
    for (auto& i : g.rts)
        if (i.rt == rt && i.type == type) info = &i;
    if (!info && g.rts.size() < 32) {
        char* p = static_cast<char*>(rt);
        char* tex = At<char*>(p, kRtTexture);
        g.rts.push_back(RState::RtInfo{rt, type, At<int>(p, kRtPoolId), At<int>(p, kRtKind),
                                       tex ? At<int>(tex, kTexType) : -1, At<uint32_t>(p, kRtFlags), 0, 0, 0, 0});
        info = &g.rts.back();
    }
    if (info) ++info->calls;
    RState::RtInfo* const outer = g.currentRt;
    g.currentRt = info;
    const uint64_t result = g.drtt(r, shader, resources, object, technique, rt, type, element);
    g.currentRt = outer;
    return result;
}

uint64_t PanoramaHook(void* element) {
    if (g.dry || g.hoisting) {  // (a panorama renders the scene too: hoisting stops here, it stays in the flush)
        g.predicted.push_back(Event{kPanorama, 0x11, nullptr, element, nullptr});
        if (g.hoisting && !g.stopped) {
            g.stopped = true;
            ++g.st.stopPanorama;
        }
        return 1;
    }
    if (g.inFlush && g.inPreprocess && OnFlush()) g.seen.push_back(Event{kPanorama, 0x11, nullptr, element, nullptr});
    return g.panorama(element);
}

// --- far trees ---------------------------------------------------------------------------------------------------------
void SnapshotQuads() {
    const int n = *g.t.spriteCount;
    const uint8_t* q = reinterpret_cast<const uint8_t*>(*g.t.spriteQuads);
    if (n > 0 && q)
        g.quads.assign(q, q + size_t(n) * kQuadSize);
    else
        g.quads.clear();
}

// verify: the quads the flush's own GenerateFarTrees made against those made before the flush. Compared: the bytes it
// writes (two pointers, position, direction, size, colours, texture coordinates, view class); +0x31..0x37 is padding,
// which holds what the list's memory held before.
void CompareQuads() {
    ++g.st.vQuads;
    const int n = *g.t.spriteCount;
    const uint8_t* q = reinterpret_cast<const uint8_t*>(*g.t.spriteQuads);
    const size_t count = n > 0 && q ? size_t(n) : 0;
    size_t at = 0;  // first differing byte
    bool same = count * kQuadSize == g.quads.size();
    for (size_t i = 0; same && i < count; ++i) {
        const uint8_t* a = q + i * kQuadSize;
        const uint8_t* b = g.quads.data() + i * kQuadSize;
        if (std::memcmp(a, b, kQuadFields) == 0) continue;
        size_t o = 0;
        while (a[o] == b[o]) ++o;
        at = i * kQuadSize + o;
        same = false;
    }
    if (same) {
        ++g.st.vQuadsSame;
        return;
    }
    if (++g.st.vLogged > 20) return;
    Log("reflections: VERIFY difference #%llu, far trees: %zu quads before the flush, %d in it; first difference in quad "
        "%zu at +0x%zX",
        g.st.vLogged, g.quads.size() / kQuadSize, n, at / kQuadSize, at % kQuadSize);
}

void FarTreesHook(void* engine) {
    if (g.dry || g.hoisting) {
        g.predicted.push_back(Event{kFarTrees, 0x17, nullptr, nullptr, nullptr});
        if (g.hoisting && !g.stopped && g.farReady) {
            // generated here, at their place in the stage: after the reflections, before the shadow casters
            const uint64_t t0 = __rdtsc();
            g.farTrees(engine);
            g.st.farEarlyCycles += __rdtsc() - t0;
            g.farEarly = true;
            ++g.st.farEarly;
            g.st.farQuads += uint64_t((std::max)(0, *g.t.spriteCount));
            if (g.farMode == RMode::Verify) SnapshotQuads();
            return;
        }
        if (g.hoisting && !g.stopped) {
            g.stopped = true;
            ++g.st.stopFarTrees;
        }
        return;
    }
    if (!g.inFlush || !g.inPreprocess || !OnFlush()) return g.farTrees(engine);
    g.seen.push_back(Event{kFarTrees, 0x17, nullptr, nullptr, nullptr});
    if (g.farEarly && g.farMode == RMode::On) {  // already generated: the 3D engine isn't called here
        ++g.st.farSkipped;
        return;
    }
    const uint64_t t0 = __rdtsc();
    g.farTrees(engine);
    if (g.farEarly) {
        CompareQuads();  // (verify: generated again as the flush would)
    } else {
        ++g.st.farInFlush;
        g.st.farInFlushCycles += __rdtsc() - t0;
    }
}

// DrawObjSprites: recorded instead of drawn while DrawFarTrees runs right before the flush; noted in verify
void DrawSpritesHook(char* r, void* list, float maxViewDist, void* objManager, float zoom) {
    if (g.recordingDraw || g.watchingDraw) {
        RState::SpriteDraw& d = g.recordingDraw ? g.drawRecord : g.drawSeen;
        d = RState::SpriteDraw{true, list, maxViewDist, objManager, zoom};
        if (g.recordingDraw) return;
    }
    g.drawSprites(r, list, maxViewDist, objManager, zoom);
}

// Right before the flush: what DrawFarTrees will do when the far-tree element is drawn (it only decides whether to call
// DrawObjSprites, from 3D-engine state that the flush doesn't change)
void RecordFarTreeDraw() {
    g.drawRecord = RState::SpriteDraw{};
    g.recordingDraw = true;
    g.drawFarTrees(g.engine);
    g.recordingDraw = false;
    g.drawRecorded = true;
    ++g.st.drawRecorded;
}

// I3DEngine::DrawFarTrees, from CREFarTreeSprites::mfDraw in the flush: the recorded call instead of the 3D engine
void DrawFarTreesHook(void* engine) {
    if (!g.inFlush || !g.drawRecorded || !OnFlushThread()) {
        if (g.inFlush) ++g.st.drawOriginal;
        return g.drawFarTrees(engine);
    }
    ++g.st.drawCalls;
    // In vanilla a reflection's flush runs inside its traversal, at the 3D engine's next recursion level, where
    // DrawFarTrees draws that level's far objects; the record is the frame's. None of the test demos' reflections
    // draws far trees (counted here, so that one that does shows)
    if (g.replaying) ++g.st.drawInReplay;
    if (g.farMode == RMode::Verify) {  // the 3D engine's decision now, against the recorded one
        g.drawSeen = RState::SpriteDraw{};
        g.watchingDraw = true;
        g.drawFarTrees(engine);
        g.watchingDraw = false;
        ++g.st.vDraws;
        if (g.drawSeen == g.drawRecord) {
            ++g.st.vDrawsSame;
        } else if (++g.st.vLogged <= 20) {
            Log("reflections: VERIFY difference #%llu, far-tree draw: recorded %s (list %p), now %s (list %p)",
                g.st.vLogged, g.drawRecord.called ? "a call" : "none", g.drawRecord.list,
                g.drawSeen.called ? "a call" : "none", g.drawSeen.list);
        }
        return;
    }
    const RState::SpriteDraw& d = g.drawRecord;
    if (!d.called) {
        ++g.st.drawNone;
        return;
    }
    ++g.st.drawReplayed;
    g.drawSprites(g.renderer, d.list, d.maxViewDist, d.objManager, d.zoom);
}

// The dry run's RenderWorld: the 3D traversal runs here; its nested EF_EndEf3D is taken (ReflectionPrepass_OnNested...)
void CaptureWorld(void* engine, int flags, const void* camera, const char* name, int drawFlags, int filterFlags) {
    char* r = g.renderer;
    Capture& c = g.captures[size_t(g.captureCount++)];
    c.flags = flags;
    c.drawFlags = drawFlags;
    c.filterFlags = filterFlags;
    c.name = name;
    c.ocean = camera == g.t.oceanCamera;
    std::memcpy(c.camera, camera, kCamBytes);
    ReadInputs(r, c.in);
    c.ended = c.unsafe = false;
    c.endFlags = 0;
    c.state = kCapPending;
    c.polyStart = c.polyEnd = *PolygonCount(*g.shadow->level + 1);
    c.items.Reset(nullptr);
    c.lights.clear();
    c.stackLights.clear();
    c.fill.clear();
    c.setters.clear();
    c.calls.resize(size_t(g.counter.Slots()));
    for (int i = 0; i < g.counter.Slots(); ++i) c.calls[size_t(i)] = g.counter.Outside(i);
    const int objects = At<int>(r, kRObjects);
    g.capturing = &c;
    g.counter.Begin();
    g.renderWorld(engine, flags, camera, name, drawFlags, filterFlags);
    g.counter.End();
    g.capturing = nullptr;
    c.objects = At<int>(r, kRObjects) - objects;
    g.st.maxCaptureObjects = (std::max)(g.st.maxCaptureObjects, c.objects);
    for (int i = 0; i < g.counter.Slots(); ++i)
        if (g.counter.Outside(i) != c.calls[size_t(i)] && !Allowed(i)) {
            c.unsafe = true;
            g.st.unsafeSlot = i;
        }
    ++g.st.captures;
    if (!c.ended) ++g.st.capturesEmpty;
    if (c.unsafe) {
        ++g.st.capturesUnsafe;
        g.stopped = true;
    }
    g.st.setters += c.setters.size();
    g.st.polygons += uint64_t(c.polyEnd - c.polyStart);
}

// Grows a renderer TArray to n elements through the engine's allocator, the way the engine grows it.
bool Fit(ByteList* list, uint32_t n, size_t elem) {
    if (n <= list->capacity) return true;
    const uint32_t cap = n + (n >> 1) + 10;
    void* p = reinterpret_cast<sp::ReallocFn>(g.shadow->realloc)(list->elems, size_t(cap) * elem);
    if (!p) return false;
    list->elems = static_cast<uint8_t*>(p);
    list->capacity = cap;
    return true;
}

// In the flush, FX_DrawToRenderTarget's RenderWorld call: what the traversal left, put back for its own flush.
void ReplayWorld(void* engine, int flags, const void* camera, const char* name, int drawFlags, int filterFlags) {
    char* r = g.renderer;
    Capture& c = g.captures[size_t(g.cursor++)];
    const bool same = c.flags == flags && c.drawFlags == drawFlags && c.filterFlags == filterFlags && c.name == name &&
                      c.ocean == (camera == g.t.oceanCamera) && std::memcmp(c.camera, camera, kCamBytes) == 0;
    if (g.mode == RMode::Verify) Verify(r, c, same);
    if (!same || c.unsafe) {  // the traversal after all, inside the flush as in vanilla
        ++g.st.replayFallback;
        for (int i = g.cursor - 1; i < g.captureCount; ++i) g.captures[size_t(i)].state = kCapStale;
        g.cursor = g.captureCount;
        // its level's polygons as before the dry run's traversal (those of the captures dropped here go with them)
        *PolygonCount(*g.shadow->level + 1) = c.polyStart;
        if (++g.loggedFallback <= 10) {
            char at[32] = " the camera the same";  // (else its first byte that differs)
            for (size_t i = 0; i < kCamBytes; ++i)
                if (c.camera[i] != static_cast<const uint8_t*>(camera)[i]) {
                    snprintf(at, sizeof at, " the camera from +0x%zX", i);
                    break;
                }
            Log("reflections: flush %llu: RenderWorld %s: the traversal runs in the flush (differing:%s%s%s%s%s;%s)",
                g.st.flushes, same ? "after an unsafe capture" : "with other arguments than the dry run's",
                c.flags != flags ? " flags" : "", c.drawFlags != drawFlags ? " draw flags" : "",
                c.filterFlags != filterFlags ? " filter" : "", c.name != name ? " name" : "",
                c.ocean != (camera == g.t.oceanCamera) ? " ocean camera" : "", at);
        }
        ++g.sceneRuns;
        RenderQueue_OnSceneRun();
        return g.renderWorld(engine, flags, camera, name, drawFlags, filterFlags);
    }
    // the last capture replayed with nothing left to the flush: no traversal in it from here on
    if (g.cursor == g.captureCount && !g.stopped) RenderQueue_OnScenesFree();
    const uint64_t t0 = __rdtsc();
    for (const Setter& s : c.setters)
        if (!s.afterStart) Apply(r, s);
    if (c.ended) {
        reinterpret_cast<RendererFn>(Method(r, kVfStartEf))(r);  // the level (clears its dynamic lights)
        const int level = *g.shadow->level;
        auto* lights = reinterpret_cast<ByteList*>(r + kRLights + level * 16);
        auto* fill = reinterpret_cast<ByteList*>(r + kRFillLights + level * 16);
        const uint32_t nLights = uint32_t(c.lights.size()), nFill = uint32_t(c.fill.size() / kFillLightSize);
        if (Fit(lights, nLights, sizeof(void*)) && Fit(fill, nFill, kFillLightSize)) {
            if (nLights) std::memcpy(lights->elems, c.lights.data(), nLights * sizeof(void*));
            lights->count = nLights;
            if (nFill) std::memcpy(fill->elems, c.fill.data(), c.fill.size());
            fill->count = nFill;
        } else {
            ++g.st.putBackFailed;
        }
        for (const Setter& s : c.setters)
            if (s.afterStart) Apply(r, s);
        if (!sp::PutBack(Lists(), c.items, reinterpret_cast<sp::ReallocFn>(g.shadow->realloc))) ++g.st.putBackFailed;
        // its flush turns the level's polygons into items: those the traversal had added by then, as in vanilla (the
        // later captures' and the shadow casters' are dropped: the casters' never get drawn, shadow flushes don't)
        int* const polygons = PolygonCount(level);
        *polygons = c.polyEnd;
        if (g.mode == RMode::Verify) {
            FlushState now;
            ReadFlushState(r, now);
            CompareFlush(c, now);
        }
        g.replaying = &c;
        reinterpret_cast<EndEf3DFn>(Method(r, kVfEndEf3D))(r, c.endFlags);  // its flush, into the reflection's target
        g.replaying = nullptr;
        if (*polygons != c.polyEnd) ++g.st.polygonsAdded;
    }
    c.state = kCapReplayed;
    ++g.st.replayed;
    g.st.replayCycles += __rdtsc() - t0;
}

void RenderWorldHook(void* engine, int flags, const void* camera, const char* name, int drawFlags, int filterFlags) {
    // (the dry run's traversals are captured on the main thread before the flush; the replays are in the flush)
    if (!g.inFlush || g.dry || GetCurrentThreadId() != (g.hoisting ? g.mainThread : FlushThread()))
        return g.renderWorld(engine, flags, camera, name, drawFlags, filterFlags);
    const bool fromDrtt = static_cast<char*>(_ReturnAddress()) == g.t.renderWorldReturn;
    if (g.hoisting) {
        if (fromDrtt && g.captureCount < kMaxCaptures && !g.stopped && !g.capturing)
            return CaptureWorld(engine, flags, camera, name, drawFlags, filterFlags);
        // anything else is left to the flush, with everything after it: the dry run goes on without it
        if (g.capturing) g.capturing->unsafe = true;  // (a scene rendered inside the traversal)
        if (!g.stopped) {
            g.stopped = true;
            ++g.st.stopWorld;
        }
        return;
    }
    if (fromDrtt && g.cursor < g.captureCount) return ReplayWorld(engine, flags, camera, name, drawFlags, filterFlags);
    // left in the flush: measured (observe: also the renderer calls it makes)
    const bool count = g.mode == RMode::Observe && fromDrtt && g.rendererCounted;
    if (count) g.counter.Begin();
    ++g.sceneRuns;
    RenderQueue_OnSceneRun();
    const uint64_t t0 = __rdtsc();
    g.renderWorld(engine, flags, camera, name, drawFlags, filterFlags);
    const uint64_t dt = __rdtsc() - t0;
    if (count) g.counter.End();
    if (fromDrtt) {
        ++g.st.worldDrtt;
        g.st.worldDrttCycles += dt;
        ++g.worldThisFlush;
        if (g.currentRt) {
            ++g.currentRt->worlds;
            g.currentRt->cycles += dt;
            if (camera == g.t.oceanCamera) ++g.currentRt->oceanWorlds;
        }
    } else {
        ++g.st.worldOther;
        g.st.worldOtherCycles += dt;
    }
    if (++g.loggedWorld <= 10)
        Log("reflections: RenderWorld inside the flush (%s): flags 0x%X, \"%s\", draw flags 0x%X, filter 0x%X, %.2f ms",
            fromDrtt ? "FX_DrawToRenderTarget" : "elsewhere", flags, name ? name : "", unsigned(drawFlags),
            unsigned(filterFlags), double(dt) / 3.0e6);
}

// The 3D engine's hooks need the engine object, which exists by the first flush; so does the renderer's vtable.
void HookEngine(char* r) {
    g.engineHooked = true;
    void* system = *g.t.system;
    void* engine = system ? reinterpret_cast<GetI3DEngineFn>((*static_cast<void***>(system))[kVfGetI3DEngine / 8])(system)
                          : nullptr;
    HMODULE e3d = GetModuleHandleW(L"Cry3DEngine.dll");
    if (!engine || !e3d) {
        g.engineFailed = true;
        Log("reflections: off: no I3DEngine");
        return;
    }
    void** vt = *static_cast<void***>(engine);
    char* e3dBase = reinterpret_cast<char*>(e3d);
    void* world = CallTrace_Original(vt[kVfRenderWorld / 8]);  // (the tracer's thunks, if on)
    void* trees = CallTrace_Original(vt[kVfGenerateFarTrees / 8]);
    if (!IsInCode(e3dBase, static_cast<const char*>(world), 16) || !IsInCode(e3dBase, static_cast<const char*>(trees), 16)) {
        g.engineFailed = true;
        Log("reflections: off: I3DEngine's RenderWorld/GenerateFarTrees are not in Cry3DEngine.dll's code");
        return;
    }
    HookSpec hooks[] = {
        {"I3DEngine::RenderWorld", world, reinterpret_cast<void*>(&RenderWorldHook), reinterpret_cast<void**>(&g.renderWorld)},
        {"I3DEngine::GenerateFarTrees", trees, reinterpret_cast<void*>(&FarTreesHook), reinterpret_cast<void**>(&g.farTrees)},
    };
    if (!InstallHooks(hooks, 2)) {
        g.engineFailed = true;
        return;
    }
    Log("reflections: hooked I3DEngine::RenderWorld (Cry3DEngine.dll+0x%X) and GenerateFarTrees (+0x%X)",
        unsigned(static_cast<char*>(world) - e3dBase), unsigned(static_cast<char*>(trees) - e3dBase));
    g.counterTried = true;
    g.rendererCounted = g.counter.Init(r, g.base, g.size, 400);
    if (g.mode == RMode::On || g.mode == RMode::Verify) {
        // the recorded setters: the vtable slots must be the functions found by pattern
        void** rvt = *reinterpret_cast<void***>(r);
        const bool slots = g.rendererCounted && g.counter.Slots() > kSlotSetSkyLight && !g.t.fullWhy &&
                           CallTrace_Original(rvt[kSlotEnableFog]) == g.t.enableFog &&
                           CallTrace_Original(rvt[kSlotSetFog]) == g.t.setFog &&
                           CallTrace_Original(rvt[kSlotSetSkyLight]) == g.t.setSkyLight;
        if (slots) {
            g.counter.SetTarget(kSlotSetCamera, reinterpret_cast<void*>(&RecSetCamera));
            g.counter.SetTarget(kSlotEnableFog, reinterpret_cast<void*>(&RecEnableFog));
            g.counter.SetTarget(kSlotSetFog, reinterpret_cast<void*>(&RecSetFog));
            g.counter.SetTarget(kSlotSetSkyLight, reinterpret_cast<void*>(&RecSetSkyLight));
            g.full = true;
            Log("reflections: rendering them before the flush (mode=%s)", RModeName(g.mode));
        } else {
            Log("reflections: mode=%s not possible (%s): observing instead", RModeName(g.mode),
                g.t.fullWhy ? g.t.fullWhy : "the renderer's setters are not at the expected vtable slots");
            g.mode = RMode::Observe;
        }
    }
    if (g.dynMode != RMode::Off && !g.full) {
        Log("reflections: dynamic textures stay in the flush (they need Reflections=on or verify)");
        g.dynMode = RMode::Off;
    } else if (g.dynMode != RMode::Off) {
        Log("reflections: dynamic texture sources (Flash, video) updated before the flush (mode=%s)", RModeName(g.dynMode));
    }
    if (g.farMode == RMode::Off) return;
    // far trees: generated in the dry run (needs it), their draw decision recorded right before the flush
    g.engine = engine;
    const char* why = !g.full ? "they need Reflections=on or verify" : g.t.farWhy;
    void* drawTrees = CallTrace_Original(vt[kVfDrawFarTrees / 8]);
    void* drawSprites = CallTrace_Original((*reinterpret_cast<void***>(r))[kSlotDrawObjSprites]);
    if (!why) {
        static const Pattern sprites = ParsePattern(kDrawSprites);
        char* ds = static_cast<char*>(drawSprites);
        if (!IsInCode(e3dBase, static_cast<const char*>(drawTrees), 16) || !IsInCode(g.base, ds, sprites.size()) ||
            !MatchesAt(ds, sprites) ||
            ds + kDrawSpritesLevel + 5 + int32_t(ReadU32(ds + kDrawSpritesLevel)) != reinterpret_cast<char*>(g.shadow->level))
            why = "DrawFarTrees or DrawObjSprites is not as expected";
    }
    if (!why) {
        HookSpec treeHooks[] = {
            {"I3DEngine::DrawFarTrees", drawTrees, reinterpret_cast<void*>(&DrawFarTreesHook),
             reinterpret_cast<void**>(&g.drawFarTrees)},
            {"CD3D9Renderer::DrawObjSprites", drawSprites, reinterpret_cast<void*>(&DrawSpritesHook),
             reinterpret_cast<void**>(&g.drawSprites)},
        };
        if (!InstallHooks(treeHooks, 2)) why = "could not write the hooks";
    }
    if (why) {
        Log("reflections: far trees stay in the flush (%s)", why);
        g.farMode = RMode::Off;
        return;
    }
    g.farReady = g.drawReady = true;
    Log("reflections: far trees generated before the flush, their draw decided there too (mode=%s; DrawFarTrees "
        "Cry3DEngine.dll+0x%X, DrawObjSprites +0x%X)",
        RModeName(g.farMode), unsigned(static_cast<char*>(drawTrees) - e3dBase), unsigned(static_cast<char*>(drawSprites) - g.base));
}

// --- the flush entry ---------------------------------------------------------------------------------------------------
// The level's polygons with preprocess flags, as EF_EndEf3D adds them to the preprocess list before its list
// processing (kPolyItems): same fields, same order.
void AddPolygonItems(int level) {
    char* storage = g.shadow->polyLists + level * 16;
    char** polys = At<char**>(storage, 0);
    const uint32_t n = At<uint32_t>(storage, 8);
    for (uint32_t i = 0; i < n; ++i) {
        char* poly = polys[i];
        const uint32_t pflags = At<uint32_t>(poly, kPolyFlags);
        char* obj = At<char*>(poly, kPolyObject);
        char* shader = At<char*>(poly, kPolyShader);
        if (!pflags || !obj || !shader) continue;
        char* res = At<char*>(poly, kPolyResources);
        sp::RendItem it{};
        it.obj = obj;
        it.objSort = uint32_t(int32_t(At<int16_t>(obj, kObjRAEId))) | (At<uint32_t>(obj, kObjFlags) & 0xFFFF0000u);
        it.dynLM = At<uint32_t>(obj, kObjDynLM);
        it.batchFlags = pflags | 1;
        const uint32_t resId = res ? At<uint32_t>(res, kResourcesId) : 0;
        it.elem = poly;
        it.sortVal = (((resId << 12) | At<uint32_t>(shader, kShaderId)) << 6) | (At<uint32_t>(poly, kPolyTechnique) & 0x3F);
        g.copy.push_back(it);
        ++g.st.polyItems;
    }
}

// Whether the list processing will run the preprocess stage on lists[0][1] (as of now), and its sorted copy.
bool SortedPreprocessList(char* r, int flags) {
    if (!g.farTrees) return false;  // (the runs below must be able to skip GenerateFarTrees)
    if (At<uint8_t>(r, kRDeviceLost) || *g.shadow->noDrawShaders) return false;  // EF_EndEf3D skips the list processing
    if (!(flags & 0x20) || (At<uint32_t>(r, kRPersFlags) & 0x1000000)) return false;  // the list processing's own gate
    const int level = *g.shadow->level;
    sp::ItemList* list = Lists() + 1;  // [nAW 0][EFSLIST_PREPROCESS]
    const uint32_t start = g.shadow->starts[(level - 1) * sp::kLists + 1], end = list->count;
    if (end > start)
        g.copy.assign(list->elems + start, list->elems + end);
    else
        g.copy.clear();
    if (g.t.polyItems) AddPolygonItems(level);  // (flag 0x20, checked above, is also EF_EndEf3D's for them)
    if (g.copy.empty()) return false;
    reinterpret_cast<SortFn>(g.t.sortPreprocess)(g.copy.data(), int(g.copy.size()));
    return (g.copy[0].batchFlags & kItemFlagsMask) != 0;  // the per-list processing calls the handler only then
}

void CountPredicted() {
    g.predictedValid = true;
    g.st.maxEntries = (std::max)(g.st.maxEntries, int(g.predicted.size()));
    for (const Event& e : g.predicted)
        ++(e.kind == kDrtt ? g.st.predDrtt : e.kind == kPanorama ? g.st.predPanorama : g.st.predFarTrees);
}

// observe: the preprocess handler on the sorted copy with all of its handlers stubbed out
void Predict(char* r, int flags) {
    const uint64_t t0 = __rdtsc();
    if (!SortedPreprocessList(r, flags)) return;
    ++g.st.preprocessFlushes;
    char* const saveCallback = At<char*>(r, kRCallback);
    char* const saveObject = At<char*>(r, kRCurObject);
    char* const saveMatrix = At<char*>(r, kRCurObjectMatrix);
    char* const saveElement = At<char*>(r, kRCurElement);
    const float saveTime = At<float>(r, kRPreprocessTime);
    At<char*>(r, kRCallback) = g.t.renderCallback;
    g.dry = true;
    g.preprocess(r, reinterpret_cast<char*>(g.copy.data()), 0, unsigned(g.copy.size()));
    g.dry = false;
    At<char*>(r, kRCallback) = saveCallback;
    At<char*>(r, kRCurObject) = saveObject;
    At<char*>(r, kRCurObjectMatrix) = saveMatrix;
    At<char*>(r, kRCurElement) = saveElement;
    At<float>(r, kRPreprocessTime) = saveTime;
    g.st.predictCycles += __rdtsc() - t0;
    CountPredicted();
}

// on/verify: the preprocess handler on the sorted copy, FX_DrawToRenderTarget for real (see the top of the file)
void Hoist(char* r, int flags) {
    const uint64_t t0 = __rdtsc();
    if (!SortedPreprocessList(r, flags)) {
        g.scenesLeft = !g.farTrees;  // (no preprocess stage in the flush: none of its traversals)
        return;
    }
    ++g.st.preprocessFlushes;
    void* const timer = *g.t.timer;
    char* const* const objects = At<char**>(r, kRObjectArray);
    if (!timer || !objects || !objects[0]) {
        ++g.st.notReady;
        return;
    }
    if (At<int>(r, kRObjects) + kObjectsReserve >= kRMaxObjects) {
        ++g.st.noRoom;
        return;
    }
    const ShadowPrepassTarget& sh = *g.shadow;
    // What EF_EndEf3D does before its list processing: the frame time (left set: it sets the same again), two cvars
    // with flag 0x400. Then the list processing: nFlags, the HDR scene and its flag bits.
    At<float>(r, kRTime) = reinterpret_cast<GetCurrTimeFn>((*static_cast<void***>(timer))[kVfGetCurrTime / 8])(timer, 0);
    const int saveAsync0 = *g.t.noAsync[0], saveAsync1 = *g.t.noAsync[1];
    if (flags & 0x400) {
        *g.t.noAsync[0] = 0;
        *g.t.noAsync[1] = 1;
    }
    const uint32_t save4 = At<uint32_t>(r, kRPersFlags), save8 = At<uint32_t>(r, kRPersFlags2);
    const uint32_t saveNFlags = At<uint32_t>(r, kRNFlags);
    const uint8_t saveHdr = At<uint8_t>(r, kRHdrActive);
    const bool hdr = (flags & 1) && *sh.hdrRendering && !*sh.measureOverdraw &&
                     (*sh.hdrAllowNonFP || At<int>(r, kRHdrFormat) <= 1) && !(save4 & 0x1000000);
    At<uint32_t>(r, kRPersFlags2) = (save8 & ~0x200u) | (hdr ? 0x200u : 0u) | ((flags & 0x80) ? 0x10000u : 0u);
    // with an HDR scene, its start (+0x9E530) also sets m_PersFlags 0x4000000 and pushes the scene's render target. The
    // dry run doesn't push it (it would have to clear it, here): its render-target stack is one shallower than the
    // flush's. Nothing the traversal calls reads the depth (verify allows for the offset), and its replay draws on the
    // flush's stack.
    At<uint32_t>(r, kRPersFlags) =
        (save4 & ~0x20000000u) | ((flags & 0x100) ? 0x20000000u : 0u) | (hdr ? 0x4000000u : 0u);
    g.rtOffset = hdr ? 1 : 0;
    At<uint32_t>(r, kRNFlags) = uint32_t(flags);
    At<uint8_t>(r, kRHdrActive) = hdr ? 1 : 0;
    // what the dry runs change and must put back (the frame id after the shadow pre-pass)
    uint8_t state[kUpdateStateSize], fog[kFogSize], ocean[kCamCopy];
    std::memcpy(state, r + kRUpdateState, sizeof state);
    std::memcpy(fog, r + kRFog, sizeof fog);
    std::memcpy(ocean, g.t.oceanCamera, sizeof ocean);
    // The reflection camera FX_DrawToRenderTarget keeps in a static is built at its first use (behind a guard). If a dry
    // run builds it, it stays: the flush's call finds it built, as it would have found it after building it itself.
    const bool cameraBuilt = !AllZero(g.t.oceanCamera, kCamCopy);
    const uint8_t updateFlag = *g.t.updateFlag;
    const int updateCounter = *g.t.updateCounter;
    void* const sky = At<void*>(r, kRSkyLight);
    const int frameRec = At<int>(r, kRFrameRec);
    char* const saveCallback = At<char*>(r, kRCallback);
    char* const saveObject = At<char*>(r, kRCurObject);
    char* const savePrev = At<char*>(r, kRPrevObject);
    char* const saveMatrix = At<char*>(r, kRCurObjectMatrix);
    char* const saveElement = At<char*>(r, kRCurElement);
    const float saveTime = At<float>(r, kRPreprocessTime);

    // the per-list processing around the handler (kListPre)
    At<char*>(r, kRCallback) = g.t.renderCallback;
    At<char*>(r, kRCurObject) = objects[0];
    At<char*>(r, kRPrevObject) = objects[0];
    At<char*>(r, kRCurObjectMatrix) = objects[0] + kObjMatrix;
    reinterpret_cast<StartBatchFn>(g.t.startBatch)(r, 1);
    g.captureCount = 0;
    g.cursor = 0;
    g.stopped = false;
    g.hoisting = true;
    g.preprocess(r, reinterpret_cast<char*>(g.copy.data()), 0, unsigned(g.copy.size()));
    g.hoisting = false;
    g.scenesLeft = g.captureCount > 0 || g.stopped;  // (a capture may fall back; what it left may traverse)
    reinterpret_cast<EndBatchFn>(g.t.endBatch)(r);

    At<char*>(r, kRCallback) = saveCallback;
    At<char*>(r, kRCurObject) = saveObject;
    At<char*>(r, kRPrevObject) = savePrev;
    At<char*>(r, kRCurObjectMatrix) = saveMatrix;
    At<char*>(r, kRCurElement) = saveElement;
    At<float>(r, kRPreprocessTime) = saveTime;
    std::memcpy(r + kRUpdateState, state, sizeof state);
    std::memcpy(r + kRFog, fog, sizeof fog);
    if (cameraBuilt)
        std::memcpy(g.t.oceanCamera, ocean, sizeof ocean);
    else if (!AllZero(g.t.oceanCamera, kCamCopy))
        ++g.st.cameraBuilt;
    *g.t.updateFlag = updateFlag;
    *g.t.updateCounter = updateCounter;
    At<void*>(r, kRSkyLight) = sky;
    At<uint32_t>(r, kRPersFlags) = save4;
    At<uint32_t>(r, kRPersFlags2) = save8;
    At<uint32_t>(r, kRNFlags) = saveNFlags;
    At<uint8_t>(r, kRHdrActive) = saveHdr;
    *g.t.noAsync[0] = saveAsync0;
    *g.t.noAsync[1] = saveAsync1;
    if (At<int>(r, kRFrameRec) != frameRec) {  // kept until the shadow pre-pass has run: its casters see it, as in vanilla
        g.heldFrameRec = frameRec;
        g.frameHeld = true;
    }
    ++g.st.hoistFlushes;
    g.st.hoistCycles += __rdtsc() - t0;
    CountPredicted();
}

// --- locating ----------------------------------------------------------------------------------------------------------
char* Rip(char* field, int tail) { return field + 4 + tail + int32_t(ReadU32(field)); }

char* Unique(char* base, const char* text, const char** why, const char* what) {
    const Pattern p = ParsePattern(text);
    const auto hits = FindInCode(base, p, 2);
    if (hits.size() != 1) *why = what;
    return hits.size() == 1 ? hits[0] : nullptr;
}

char* UniqueIn(char* start, size_t size, const char* text, const char** why, const char* what) {
    const Pattern p = ParsePattern(text);
    const auto hits = FindInRange(start, size, p, 2);
    if (hits.size() != 1) *why = what;
    return hits.size() == 1 ? hits[0] : nullptr;
}

bool Matches(char* base, char* at, const char* text) {
    const Pattern p = ParsePattern(text);
    return IsInCode(base, at, p.size()) && MatchesAt(at, p);
}

// whether code [from, from + size) has a direct call to target
bool Calls(char* from, size_t size, const char* target) {
    for (size_t i = 0; i + 5 <= size; ++i)
        if (uint8_t(from[i]) == 0xE8 && RelTarget(from + i) == target) return true;
    return false;
}

// The pieces on/verify need besides observe's. Null if all found and consistent (then filled in), else why not.
const char* LocateFull(char* base, const ShadowPrepassTarget& shadow, char* listPre, char* world, ReflectionTarget* out) {
    const char* why = nullptr;
    ReflectionTarget t = *out;
    char* drtt = t.drawToRT;
    char* state = UniqueIn(drtt, kDrttSize, kDrttState, &why, "FX_DrawToRenderTarget: the update decision not found");
    char* frame = state ? UniqueIn(drtt, kDrttSize, kDrttFrame, &why, "FX_DrawToRenderTarget: the frame id not found") : nullptr;
    char* pool = frame ? UniqueIn(drtt, kDrttSize, kDrttPoolId, &why, "FX_DrawToRenderTarget: the pool id not found") : nullptr;
    char* tex = pool ? UniqueIn(drtt, kDrttSize, kDrttTexType, &why, "FX_DrawToRenderTarget: the texture type not found")
                     : nullptr;
    char* fogOn = tex ? Unique(base, kEnableFog, &why, "EnableFog not found") : nullptr;
    char* fog = fogOn ? Unique(base, kSetFog, &why, "SetFog not found") : nullptr;
    char* sky = fog ? Unique(base, kSetSkyLight, &why, "SetSkyLightRenderParams not found") : nullptr;
    char* fill = sky ? Unique(base, kAddFillLight, &why, "EF_ADDFillLight not found") : nullptr;
    if (fill && !IsInCode(base, shadow.endEf3D, kEndEf3DSize)) return "EF_EndEf3D is too close to the end of the code";
    char* onset = fill ? UniqueIn(shadow.endEf3D, kEndEf3DSize, kEndEfTime, &why, "EF_EndEf3D: its frame time not found")
                      : nullptr;
    char* polys = onset ? Unique(base, kPolyItems, &why, "EF_EndEf3D: its polygon items not found") : nullptr;
    if (!polys) return why;
    if (!Matches(base, drtt, kDrttHead)) return "FX_DrawToRenderTarget: its checks of the render target are not as expected";
    t.updateFlag = reinterpret_cast<uint8_t*>(Rip(state + kStateFlag[0], 1));
    for (int o : kStateFlag)
        if (Rip(state + o, 1) != reinterpret_cast<char*>(t.updateFlag)) return "the update decision's flag differs";
    t.updateCounter = reinterpret_cast<int*>(Rip(state + kStateCounter[0], 0));
    for (int o : kStateCounter)
        if (Rip(state + o, 0) != reinterpret_cast<char*>(t.updateCounter)) return "the update counter differs";
    t.noDrawNear = reinterpret_cast<int*>(Rip(world + kWorldNoDrawNear[0], 0));
    for (int o : kWorldNoDrawNear)
        if (Rip(world + o, 0) != reinterpret_cast<char*>(t.noDrawNear)) return "r_NoDrawNear differs";
    t.cvar980c = reinterpret_cast<int*>(Rip(world + kWorldCvar[0], 0));
    if (Rip(world + kWorldCvar[1], 0) != reinterpret_cast<char*>(t.cvar980c) ||
        Rip(world + kWorldCvarZero, 4) != reinterpret_cast<char*>(t.cvar980c))
        return "the cvar zeroed for the reflection differs";
    if (Rip(fill + kFillLevel, 0) != reinterpret_cast<char*>(shadow.level)) return "EF_ADDFillLight: another recursion level";
    // EF_EndEf3D's start of a flush: the timer, the cvars of flag 0x400, the level's polygon items (its own call)
    t.timer = reinterpret_cast<void**>(Rip(onset + kEndEfTimer, 0));
    for (int i = 0; i < 2; ++i) t.noAsync[i] = reinterpret_cast<int*>(Rip(onset + kEndEfAsyncRead[i], 0));
    if (Rip(onset + kEndEfAsyncSet[0], 0) != reinterpret_cast<char*>(t.noAsync[0]) ||
        Rip(onset + kEndEfAsyncSet[1], 4) != reinterpret_cast<char*>(t.noAsync[1]))
        return "EF_EndEf3D: its cvars for flag 0x400 differ";
    bool same = Rip(polys + kPolyLevel, 0) == reinterpret_cast<char*>(shadow.level) &&
                Rip(polys + kPolyStorage, 0) == shadow.polyLists && RelTarget(polys + kPolyRealloc) == shadow.realloc &&
                Calls(shadow.endEf3D, kEndEf3DSize, polys);
    for (int o : kPolyCount) same &= Rip(polys + o, 0) == shadow.lists + 16 + 8;
    for (int o : kPolyElems) same &= Rip(polys + o, 0) == shadow.lists + 16;
    if (!same) return "EF_EndEf3D: its polygon items are not as expected";
    if (!IsInImage(base, reinterpret_cast<char*>(t.updateFlag), 1) || !IsInImage(base, reinterpret_cast<char*>(t.updateCounter), 4) ||
        !IsInImage(base, reinterpret_cast<char*>(t.noDrawNear), 4) || !IsInImage(base, reinterpret_cast<char*>(t.cvar980c), 4) ||
        !IsInImage(base, t.oceanCamera, kCamCopy) || !IsInImage(base, reinterpret_cast<char*>(t.timer), 8) ||
        !IsInImage(base, reinterpret_cast<char*>(t.noAsync[0]), 4) || !IsInImage(base, reinterpret_cast<char*>(t.noAsync[1]), 4))
        return "the globals used are outside the module";
    t.startBatch = RelTarget(listPre + kListPreStart);
    t.endBatch = RelTarget(listPre + kListPreEnd);
    if (!IsInCode(base, t.startBatch, 16) || !IsInCode(base, t.endBatch, 16)) return "the per-list setup is not in the code";
    t.enableFog = fogOn;
    t.setFog = fog;
    t.setSkyLight = sky;
    t.polyItems = polys;
    *out = t;
    return nullptr;
}

// The far-tree sprites' quad list (FarTrees=on|verify): its growth and its sort in GenerateObjSprites, which must agree.
// Null if found (then filled in), else why not.
const char* LocateFar(char* base, const ShadowPrepassTarget& shadow, ReflectionTarget* out) {
    const char* why = nullptr;
    char* grow = Unique(base, kSpriteQuads, &why, "GenerateObjSprites: its quad list not found");
    char* sort = grow ? Unique(base, kSpriteSort, &why, "GenerateObjSprites: its quad sort not found") : nullptr;
    if (!sort) return why;
    if (!(sort > grow && sort - grow < 0x1000)) return "GenerateObjSprites: its quad list and sort are apart";
    char* elems = Rip(grow + kQuadElems[0], 0);
    char* count = Rip(grow + kQuadCount[0], 0);
    bool same = Rip(sort + kSortElems, 0) == elems && Rip(sort + kSortCount, 0) == count &&
                RelTarget(grow + kQuadRealloc) == shadow.realloc;
    for (int o : kQuadElems) same &= Rip(grow + o, 0) == elems;
    for (int o : kQuadCount) same &= Rip(grow + o, 0) == count;
    if (!same || !IsInImage(base, elems, 8) || !IsInImage(base, count, 4))
        return "GenerateObjSprites: its quad list is not as expected";
    out->spriteQuads = reinterpret_cast<char**>(elems);
    out->spriteCount = reinterpret_cast<int*>(count);
    return nullptr;
}
#endif  // _WIN64

void Report(const char* when) {
    if (!g.installed) return;
    const RStats& s = g.st;
    Log("reflections [%s]: mode=%s | flushes=%llu with a preprocess stage=%llu | predicted: draw-to-RT=%llu panorama=%llu "
        "far trees=%llu (polygon items %llu) | in the preprocess stage: draw-to-RT=%llu panorama=%llu far trees=%llu; "
        "draw-to-RT while drawing=%llu | sequences same=%llu differ=%llu not predicted=%llu | most calls in a flush=%d",
        when, RModeName(g.mode), s.flushes, s.preprocessFlushes, s.predDrtt, s.predPanorama, s.predFarTrees, s.polyItems,
        s.seenDrtt, s.seenPanorama, s.seenFarTrees, s.seenDrttDraw, s.sequencesSame, s.sequencesDiffer, s.notPredicted,
        s.maxEntries);
    Log("reflections [%s]: RenderWorld inside the flush: from FX_DrawToRenderTarget=%llu (%.2f ms each) in %llu flushes, "
        "elsewhere=%llu (%.2f ms each) | preprocess stage %.3f ms per flush, prediction %.3f ms | calls from other "
        "threads=%llu",
        when, s.worldDrtt, double(s.worldDrttCycles) / 3.0e6 / double(s.worldDrtt ? s.worldDrtt : 1), s.worldFrames,
        s.worldOther, double(s.worldOtherCycles) / 3.0e6 / double(s.worldOther ? s.worldOther : 1),
        double(s.preprocessCycles) / 3.0e6 / double(s.flushes ? s.flushes : 1),
        double(s.predictCycles) / 3.0e6 / double(s.flushes ? s.flushes : 1), s.otherThread);
    if (g.full)
        Log("reflections [%s]: rendered early: dry runs in %llu flushes (not ready %llu, no room %llu), captures=%llu "
            "(empty %llu, unsafe %llu, last unsafe slot %d), setters recorded=%llu, polygons=%llu, most render objects in "
            "one=%d, reflection camera built by a dry run=%llu | stopped at: panorama=%llu far trees=%llu dynamic "
            "texture=%llu other render target=%llu other RenderWorld=%llu; targets with nothing to do passed=%llu | "
            "replayed=%llu, traversal in the flush after all=%llu, unused=%llu, put-back failures=%llu, polygons added "
            "while replaying=%llu, lights on the traversal's stack copied=%llu | %.3f ms per dry-run flush, %.3f ms per "
            "replay",
            when, s.hoistFlushes, s.notReady, s.noRoom, s.captures, s.capturesEmpty, s.capturesUnsafe, s.unsafeSlot,
            s.setters, s.polygons, s.maxCaptureObjects, s.cameraBuilt, s.stopPanorama, s.stopFarTrees, s.stopDynTexture,
            s.stopTarget, s.stopWorld, s.noopTargets, s.replayed, s.replayFallback, s.unused, s.putBackFailed,
            s.polygonsAdded, s.stackLights, double(s.hoistCycles) / 3.0e6 / double(s.hoistFlushes ? s.hoistFlushes : 1),
            double(s.replayCycles) / 3.0e6 / double(s.replayed ? s.replayed : 1));
    if (g.farMode != RMode::Off)
        Log("reflections [%s]: far trees: mode=%s | generated before the flush=%llu (%.3f ms each, %.1f quads), skipped "
            "in the flush=%llu, generated in the flush=%llu (%.3f ms each) | draw decisions recorded=%llu; DrawFarTrees "
            "calls in the flush=%llu: replayed=%llu, nothing to draw=%llu, left to the 3D engine=%llu; inside a replayed "
            "reflection's flush=%llu",
            when, RModeName(g.farMode), s.farEarly, double(s.farEarlyCycles) / 3.0e6 / double(s.farEarly ? s.farEarly : 1),
            double(s.farQuads) / double(s.farEarly ? s.farEarly : 1), s.farSkipped, s.farInFlush,
            double(s.farInFlushCycles) / 3.0e6 / double(s.farInFlush ? s.farInFlush : 1), s.drawRecorded, s.drawCalls,
            s.drawReplayed, s.drawNone, s.drawOriginal, s.drawInReplay);
#ifdef _WIN64
    if (g.farMode == RMode::Verify)
        Log("reflections [%s]: VERIFY far trees: quads generated in the flush again=%llu, same as before it=%llu | draw "
            "decisions checked=%llu, same=%llu",
            when, s.vQuads, s.vQuadsSame, s.vDraws, s.vDrawsSame);
    if (g.dynMode != RMode::Off)
        Log("reflections [%s]: dynamic textures: mode=%s | updated in the dry run=%llu (%.3f ms each); in the flush: "
            "skipped=%llu, left there=%llu, updated early but not called there=%llu",
            when, RModeName(g.dynMode), s.dynEarly, double(s.dynCycles) / 3.0e6 / double(s.dynEarly ? s.dynEarly : 1),
            s.dynSkipped, s.dynInFlush, s.dynUnused);
    if (g.dynMode == RMode::Verify)
        Log("reflections [%s]: VERIFY dynamic textures: skipped calls checked=%llu, same source, distance and frame "
            "time=%llu",
            when, s.vDyn, s.vDynSame);
    if (g.mode == RMode::Verify) {
        char line[1024] = "", fline[512] = "";
        size_t n = 0, fn = 0;
        for (int i = 0; i < 15; ++i)
            if (s.vField[i]) Append(line, sizeof line, n, " %s=%llu", kFieldNames[i], s.vField[i]);
        for (int i = 0; i < 10; ++i)
            if (s.vFlushField[i]) Append(fline, sizeof fline, fn, " %s=%llu", kFlushNames[i], s.vFlushField[i]);
        Log("reflections [%s]: VERIFY replayed RenderWorld calls=%llu: arguments same=%llu | inputs same=%llu "
            "differ=%llu%s%s | replayed flushes=%llu: starting state same=%llu%s%s",
            when, s.vCalls, s.vCallsSame, s.vInputsSame, s.vInputsDiffer, n ? ":" : "", line, s.vFlushes, s.vFlushSame,
            fn ? ", differ:" : "", fline);
    }
    for (const auto& i : g.rts)
        Log("reflections [%s]: render target %p type 0x%X: pool id %d, kind %d, flags 0x%X, texture type %d | calls=%llu, "
            "RenderWorld=%llu (ocean camera %llu), %.2f ms each",
            when, i.rt, i.type, i.poolId, i.kind, i.flags, i.texType, i.calls, i.worlds, i.oceanWorlds,
            double(i.cycles) / 3.0e6 / double(i.worlds ? i.worlds : 1));
    if (g.rendererCounted && (s.worldDrtt || s.captures)) {  // renderer methods the reflection traversals called
        char line[2048] = "";
        size_t n = 0;
        for (int i = 0; i < g.counter.Slots(); ++i) {
            const uint64_t out = g.counter.Outside(i), in = g.counter.Inside(i);
            if (out || in) Append(line, sizeof line, n, " %d:%llu/%llu", i, out, in);
        }
        Log("reflections [%s]: renderer vtable calls during reflection traversals (slot:from other modules/from the "
            "renderer):%s",
            when, line);
    }
#endif
}

}  // namespace

bool ReflectionPrepass_Locate(char* base, const ShadowPrepassTarget& sp, ReflectionTarget* out, const char** why) {
#ifndef _WIN64
    (void)base;
    (void)sp;
    (void)out;
    *why = "64-bit renderer only";
    return false;
#else
    ReflectionTarget t;
    char* head = Unique(base, kPreHead, why, "the preprocess handler not found");
    char* disp = head ? UniqueIn(head, kPreSize, kPreDispatch, why, "the preprocess dispatch not found") : nullptr;
    char* listPre = disp ? Unique(base, kListPre, why, "the per-list preprocess branch not found") : nullptr;
    char* gate = listPre ? Unique(base, kGatePre, why, "the list processing's preprocess call not found") : nullptr;
    char* sortCall = gate ? Unique(base, kSortCall, why, "the preprocess list's sort call not found") : nullptr;
    if (!sortCall) return false;
    if (!IsInCode(base, head, kPreSize)) {
        *why = "the preprocess handler is too close to the end of the code";
        return false;
    }
    // the handler as the per-list processing calls it, on lists[nAW][1], from the list processing at level 1
    char* listFn = RelTarget(gate + kGateList);
    if (RelTarget(listPre + kListPreHandler) != head || Rip(listPre + kListPreLists, 0) != sp.lists + 16 ||
        !(listPre > listFn && listPre - listFn < 0x100) || Rip(gate + kGateBase, 0) != base ||
        Rip(gate + kGateLevel, 0) != reinterpret_cast<char*>(sp.level) ||
        base + int32_t(ReadU32(gate + kGateStarts)) + 0x78 != reinterpret_cast<char*>(sp.starts) + 4) {
        *why = "the preprocess stage is not called as expected";
        return false;
    }
    // the sort: the list sort's call for the preprocess list, into the expected std::sort
    t.sortPreprocess = RelTarget(sortCall + kSortCallSort);
    if (Rip(sortCall + kSortCallBase, 0) != base || base + int32_t(ReadU32(sortCall + kSortCallLists)) != sp.lists ||
        !Matches(base, t.sortPreprocess, kSort)) {
        *why = "the preprocess list's sort is not as expected";
        return false;
    }
    // the dispatch's targets: FX_DrawToRenderTarget (three calls), the panorama update, the flush's render callback
    t.drawToRT = RelTarget(disp + kDispDrtt[0]);
    for (int o : kDispDrtt)
        if (RelTarget(disp + o) != t.drawToRT) {
            *why = "the preprocess dispatch calls different render-target functions";
            return false;
        }
    t.panorama = RelTarget(disp + kDispPanorama);
    t.renderCallback = Rip(disp + kDispCallback, 0);
    t.system = reinterpret_cast<void**>(Rip(disp + kDispSystem, 0));
    if (!IsInCode(base, t.drawToRT, kDrttSize) || !Matches(base, t.panorama, kPanoramaHead) ||
        !IsInCode(base, t.renderCallback, 16) || t.system != sp.system) {
        *why = "the preprocess dispatch's targets are not as expected";
        return false;
    }
    char* world = UniqueIn(t.drawToRT, kDrttSize, kDrttWorld, why, "FX_DrawToRenderTarget: its RenderWorld call not found");
    if (!world) return false;
    const char* name = Rip(world + kWorldName, 0);
    if (!IsInImage(base, name, 22) || std::strcmp(name, "FX_DrawToRenderTarget") != 0) {
        *why = "FX_DrawToRenderTarget: its RenderWorld call is not as expected";
        return false;
    }
    t.renderWorldReturn = world + kWorldReturn;
    t.oceanCamera = Rip(world + kWorldOceanCamera, 0);
    t.preprocess = head;
    t.fullWhy = LocateFull(base, sp, listPre, world, &t);
    t.farWhy = t.fullWhy ? "rendering reflections early not possible" : LocateFar(base, sp, &t);
    *out = t;
    return true;
#endif
}

void ReflectionPrepass_Install(const wchar_t* moduleName, char* base, const ShadowPrepassTarget& sp) {
    if (g.installed) return;
    g.mode = ReadRMode();
    if (g.mode == RMode::Off) return;
    g.farMode = ReadFarMode();
    g.dynMode = ReadDynMode();
#ifndef _WIN64
    (void)moduleName;
    (void)base;
    (void)sp;
#else
    const char* why = nullptr;
    if (!ReflectionPrepass_Locate(base, sp, &g.t, &why)) {
        Log("%ls: reflections left alone: %s", moduleName, why);
        return;
    }
    g.shadow = &sp;
    g.base = base;
    g.rts.reserve(32);  // (currentRt points into it: no reallocation)
    g.captures.resize(kMaxCaptures);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    g.size = nt->OptionalHeader.SizeOfImage;
    HookSpec hooks[] = {
        {"the preprocess handler", g.t.preprocess, reinterpret_cast<void*>(&PreprocessHook), reinterpret_cast<void**>(&g.preprocess)},
        {"FX_DrawToRenderTarget", g.t.drawToRT, reinterpret_cast<void*>(&DrttHook), reinterpret_cast<void**>(&g.drtt)},
        {"CREPanoramaCluster::UpdateImposter", g.t.panorama, reinterpret_cast<void*>(&PanoramaHook),
         reinterpret_cast<void**>(&g.panorama)},
    };
    if (!InstallHooks(hooks, 3)) {
        Log("%ls: reflections left alone: could not write the hooks", moduleName);
        return;
    }
    g.installed = true;
    Log("%ls: reflections: mode=%s, preprocess handler +0x%X, FX_DrawToRenderTarget +0x%X (RenderWorld returns to "
        "+0x%X), panorama update +0x%X, sort +0x%X; rendering them early: %s; far trees: mode=%s, %s",
        moduleName, RModeName(g.mode), unsigned(g.t.preprocess - base), unsigned(g.t.drawToRT - base),
        unsigned(g.t.renderWorldReturn - base), unsigned(g.t.panorama - base), unsigned(g.t.sortPreprocess - base),
        g.t.fullWhy ? g.t.fullWhy : "possible", RModeName(g.farMode), g.t.farWhy ? g.t.farWhy : "possible");
#endif
}

void ReflectionPrepass_OnFlushEntry(char* r, int flags) {
    if (!g.installed) return;
#ifdef _WIN64
    if (!g.mainThread) g.mainThread = GetCurrentThreadId();
    if (GetCurrentThreadId() != g.mainThread) return;
    g.renderer = r;
    if (!g.engineHooked) HookEngine(r);
    ++g.st.flushes;
    g.predicted.clear();
    g.seen.clear();
    g.predictedValid = false;
    g.captureCount = 0;
    g.cursor = 0;
    g.frameHeld = false;
    g.farEarly = g.drawRecorded = false;
    g.dynUpdates.clear();
    g.dynCursor = 0;
    g.inFlush = true;
    g.scenesLeft = true;
    if (g.full)
        Hoist(r, flags);
    else
        Predict(r, flags);
#else
    (void)r;
    (void)flags;
#endif
}

#ifdef _WIN64
namespace {
void RestoreFrame(char* r) {
    if (!g.frameHeld) return;
    At<int>(r, kRFrameRec) = g.heldFrameRec;  // the flush's FX_DrawToRenderTarget bumps it again
    g.frameHeld = false;
}
}  // namespace
#endif

void ReflectionPrepass_BeforeFlush(char* r) {
    if (!g.installed || !g.inFlush) return;
#ifdef _WIN64
    if (GetCurrentThreadId() != g.mainThread) return;
    RestoreFrame(r);
    if (g.drawReady) RecordFarTreeDraw();  // (after the shadow pre-pass, as the draw comes after the shadow maps)
#else
    (void)r;
#endif
}

bool ReflectionPrepass_WillTake(int level) {
#ifdef _WIN64
    return g.capturing && GetCurrentThreadId() == g.mainThread && level == 2 && !g.capturing->ended;
#else
    (void)level;
    return false;
#endif
}

#ifdef _WIN64
// A light the traversal keeps on its stack (the 3D engine's temporary sun for a reflection) is gone by the time the
// flush replays the capture: copied into the capture, now, while it is there (vanilla's nested flush reads it here).
void CopyStackLights(Capture& c) {
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    auto onStack = [&](const void* p) { return uintptr_t(p) >= low && uintptr_t(p) < high; };
    size_t n = 0;
    for (void* l : c.lights) n += onStack(l) ? 1 : 0;
    if (!n) return;
    c.stackLights.resize(n * kLightBytes);  // (once: the capture's lights point into it)
    size_t k = 0;
    for (void*& l : c.lights)
        if (onStack(l)) {
            uint8_t* copy = c.stackLights.data() + k++ * kLightBytes;
            std::memcpy(copy, l, kLightBytes);
            l = copy;
        }
    g.st.stackLights += n;
}
#endif

bool ReflectionPrepass_OnNestedEndEf3D(char* r, int flags) {
#ifdef _WIN64
    if (!g.capturing || GetCurrentThreadId() != g.mainThread) return false;
    const int level = *g.shadow->level;
    Capture& c = *g.capturing;
    if (level != 2 || c.ended) {  // a deeper level, or a second one: flushed where it is, the capture is not replayed
        c.unsafe = true;
        return false;
    }
    c.ended = true;
    c.endFlags = flags;
    c.polyEnd = *PolygonCount(level);
    if (g.mode == RMode::Verify) ReadFlushState(r, c.flush);
    sp::TakeLevel(Lists(), g.shadow->starts + (level - 1) * sp::kLists, c.items);
    auto* lights = reinterpret_cast<PtrList*>(r + kRLights + level * 16);
    c.lights.assign(lights->elems, lights->elems + lights->count);
    CopyStackLights(c);
    auto* fill = reinterpret_cast<ByteList*>(r + kRFillLights + level * 16);
    c.fill.assign(fill->elems, fill->elems + size_t(fill->count) * kFillLightSize);
    fill->count = 0;               // (its flush would have used them up)
    *g.shadow->level = level - 1;  // closes the level without its flush: its render objects stay until the flush ends
    return true;
#else
    (void)r;
    (void)flags;
    return false;
#endif
}

void ReflectionPrepass_OnFlushEnd(char* r) {
    if (!g.installed || !g.inFlush) return;
#ifdef _WIN64
    g.inFlush = false;
    RestoreFrame(r);  // (if BeforeFlush didn't run)
    for (int i = g.cursor; i < g.captureCount; ++i) ++g.st.unused;
    g.st.dynUnused += g.dynUpdates.size() - g.dynCursor;  // (updated early, though the flush made no such call)
    g.captureCount = g.cursor = 0;
    if (g.worldThisFlush) ++g.st.worldFrames;
    g.worldThisFlush = 0;
    for (const Event& e : g.seen)
        ++(e.kind == kDrtt ? g.st.seenDrtt : e.kind == kPanorama ? g.st.seenPanorama : g.st.seenFarTrees);
    if (!g.predictedValid) {  // (no prediction this flush: not ready, no room, ...)
        if (!g.seen.empty()) ++g.st.notPredicted;
        return;
    }
    if (g.predicted.empty() && g.seen.empty()) return;
    if (g.predicted == g.seen) {
        ++g.st.sequencesSame;
        return;
    }
    ++g.st.sequencesDiffer;
    if (++g.loggedDiffer > 10) return;
    char line[1024] = "";
    size_t n = 0;
    for (const auto* v : {&g.predicted, &g.seen}) {
        Append(line, sizeof line, n, v == &g.predicted ? " predicted:" : " | seen:");
        for (size_t i = 0; i < v->size() && i < 12; ++i) {
            const Event& e = (*v)[i];
            Append(line, sizeof line, n, " %s 0x%X rt %p el %p", KindName(e.kind), e.type, e.rt, e.element);
        }
    }
    Log("reflections: flush %llu: the preprocess stage did other calls than predicted:%s", g.st.flushes, line);
#else
    (void)r;
#endif
}

bool ReflectionPrepass_Early() {
#ifdef _WIN64
    return g.full;
#else
    return false;
#endif
}

bool ReflectionPrepass_MayRunScenes() {
#ifdef _WIN64
    return !g.installed || !g.full || g.scenesLeft;
#else
    return true;
#endif
}

const uint64_t* ReflectionPrepass_SceneRuns() {
#ifdef _WIN64
    return g.installed && g.engineHooked && !g.engineFailed ? &g.sceneRuns : nullptr;
#else
    return nullptr;
#endif
}

const uint8_t* ReflectionPrepass_ReplayCamera() {
#ifdef _WIN64
    return g.replaying && OnFlushThread() ? g.replaying->camera : nullptr;
#else
    return nullptr;
#endif
}

void ReflectionPrepass_ForEachCapture(void (*fn)(const sp::Stash&, void*), void* ctx) {
#ifdef _WIN64
    for (int i = g.cursor; i < g.captureCount; ++i)
        if (g.captures[size_t(i)].state == kCapPending) fn(g.captures[size_t(i)].items, ctx);
#else
    (void)fn;
    (void)ctx;
#endif
}

void ReflectionPrepass_ForEachCapturedLights(void (*fn)(void**, size_t, void*), void* ctx) {
#ifdef _WIN64
    for (int i = g.cursor; i < g.captureCount; ++i) {
        Capture& c = g.captures[size_t(i)];
        if (c.state == kCapPending && !c.lights.empty()) fn(c.lights.data(), c.lights.size(), ctx);
    }
#else
    (void)fn;
    (void)ctx;
#endif
}

void ReflectionPrepass_ForEachRecordedSkyLight(void (*fn)(const void**, void*), void* ctx) {
#ifdef _WIN64
    for (int i = g.cursor; i < g.captureCount; ++i) {
        Capture& c = g.captures[size_t(i)];
        if (c.state != kCapPending) continue;
        for (Setter& s : c.setters)
            if (s.slot == kSlotSetSkyLight) fn(&s.ptr, ctx);
    }
#else
    (void)fn;
    (void)ctx;
#endif
}

bool ReflectionPrepass_FarQuads(char** quads, int* count) {
#ifdef _WIN64
    if (!g.farEarly || g.farMode != RMode::On || !g.t.spriteQuads || !g.t.spriteCount) return false;
    *quads = *g.t.spriteQuads;
    *count = *g.t.spriteCount;
    return *quads && *count > 0;
#else
    (void)quads;
    (void)count;
    return false;
#endif
}

void ReflectionPrepass_Report(const char* when) { Report(when); }

}  // namespace optiperf
