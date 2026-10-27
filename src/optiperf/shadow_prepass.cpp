// Shadow pre-pass: Phase 2 of research/docs/cpu-multithreading-plan.md, step 1 (details in
// research/docs/shadow-prepass.md). x64 CryRenderD3D10.dll only.
//
// The renderer's flush (EF_EndEf3D) renders the shadow maps itself: for each shadow frustum, PrepareDepthMap opens a
// nested recursion level, calls Render on every shadow caster (3D-engine and entity code, which calls the renderer's
// producers again) and flushes that level into the map. A render thread can't run that 3D-engine code, so the casters
// have to be rendered before the flush. This patch does that, still on the main thread:
//   - pre-pass: at EF_EndEf3D entry (At=flush), or at the start of the shadow pass (At=shadows, for comparison), it
//     predicts which frustums PrepareDepthMap will render and renders their casters as PrepareDepthMap's caster loop
//     does (same SRendParams, same renderer flags, same nested level). Then it moves the items they produced out of the
//     lists into a stash and closes the level without recycling its render objects: they stay valid until the flush
//     ends, like everything else of the frame.
//   - replay (Mode=on): when PrepareDepthMap comes to a frustum with a stash, its caster list is swapped for a single
//     placeholder caster whose Render puts the stash back. Everything else is the game's own code.
//   - verify (Mode=verify): as on, but the caster list is swapped for spies, one per caster, which pass the getters on
//     to the real caster. When PrepareDepthMap's loop calls a spy's Render, the spy compares what the call gets
//     (SRendParams, renderer flags, recursion level, frame ids, cameras) with what the pre-pass gave the real caster,
//     and the first one puts the stash back. Nothing is rendered twice.
//   - compare (Mode=compare): the original casters render as well, and at the nested level's EF_EndEf3D their items
//     are compared with the stash (items exactly, render objects by contents). Rendering twice moves some casters'
//     own state on (vegetation bending, some entities' matrices), so its findings need reading with that in mind.
// A point light's map (omni-directional) has up to six sides, rendered in one PrepareDepthMap call: each side culls the
// casters against its own camera and flushes its own nested level. The pre-pass builds those cameras with the game's
// own CCamera code and renders each side separately; the replay puts one side's stash back per side. Maps the pre-pass
// doesn't handle (the geometry-shader path, no room for more render objects) are left to the original code.
//
// What the producers see is the same: the analysis behind this (which renderer state the code reachable from the
// casters reads) is in the doc. In short, they read the recursion level, m_PersFlags bit 20 (shadow generation, set
// here as PrepareDepthMap sets it) and m_PersFlags2 bit 17 (set only while a reflection is rendered), plus cvars.
//
// Locating: no version or hash checks. Each function and code block the patch relies on is matched by a pattern, the
// globals and member offsets come from the matched code, and they must agree wherever they appear.
//
// [ShadowPrepass] Mode = off (default) | on | verify | compare     (env OPTIPERF_SHADOW_PREPASS)
//                 At   = flush (default) | shadows                  (env OPTIPERF_SHADOW_PREPASS_AT)

#include "shadow_prepass.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "allocator.h"
#include "calltrace.h"
#include "census.h"
#include "engine_data.h"
#include "config.h"
#include "cvar_snapshot.h"
#include "env_snapshot.h"
#include "flush_thread.h"
#include "frame_phases.h"
#include "producer_timing.h"
#include "hooks.h"
#include "light_snapshot.h"
#include "log.h"
#include "material_snapshot.h"
#include "particle_prepass.h"
#include "reflection_prepass.h"
#include "render_queue.h"
#include "scan.h"
#include "shadow_prepass_lists.h"
#include "traversal_record.h"
#include "skin_prepass.h"
#include "sys_snapshot.h"

namespace optiperf {
namespace {

#ifdef _WIN64
// --- patterns (x64 CryRenderD3D10.dll, build 6156) --------------------------------------------------------------------
// Field offsets below are those of the 4-byte displacement or rel32 inside each pattern.

// CD3D9Renderer::EF_EndEf3D, from its start to the r_NoDrawShaders test (the device-lost test at +0xA60 included)
constexpr const char kEndEf3D[] =
    "48 89 5C 24 18 57 48 81 EC 50 01 00 00 83 3D ?? ?? ?? ?? 02 0F 29 B4 24 40 01 00 00 8B FA 48 8B "
    "D9 75 15 F6 C2 20 74 10 83 3D ?? ?? ?? ?? 01 75 07 B2 01 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 "
    "8B 01 FF 50 28 83 3D ?? ?? ?? ?? 01 0F 28 F0 7D 19 48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 "
    "8B 01 FF 50 10 E9 ?? ?? ?? ?? 48 8B 03 48 8B CB FF 90 48 06 00 00 80 BB 60 0A 00 00 00 74 0C 83 "
    "2D ?? ?? ?? ?? 01 E9 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 74 78";
constexpr int kEndEf3DLevel[] = {0x2A, 0x47, 0x81};  // cmp [level], 1 (x2); sub [level], 1
constexpr int kEndEf3DNoDraw = 0x8D;                // cmp [r_NoDrawShaders], 0

// PrepareAllDepthMaps: returns if r_UseShadowsPool; for each light of the level with DLF_CASTSHADOW_MAPS (+0x1D4 &
// 0x10): the geometry-shader variant if r_ShadowGenGS and directional (& 2), else PrepareDepthMap (vtable +0x678) for
// each of up to 8 frustums (+0x1F0)
constexpr const char kPrepareAll[] =
    "40 56 48 83 EC 30 83 3D ?? ?? ?? ?? 00 48 8B F1 0F 85 FD 00 00 00 48 63 15 ?? ?? ?? ?? 48 8B C2 "
    "48 03 C0 8B 8C C1 48 0F 00 00 85 C9 0F 84 E1 00 00 00 4C 89 64 24 28 45 33 E4 85 C9 0F 84 CC 00 "
    "00 00 48 89 5C 24 40 48 89 6C 24 48 48 89 7C 24 50 4C 89 6C 24 20 45 33 ED 66 66 90 66 66 66 90 "
    "48 8D 82 F4 00 00 00 48 03 C0 48 8B 04 C6 49 8B 6C 05 00 8B 85 D4 01 00 00 A8 10 74 5A 83 3D ?? "
    "?? ?? ?? 00 74 11 A8 02 74 0D 48 8B D5 48 8B CE E8 ?? ?? ?? ?? EB 40 48 8B BD F0 01 00 00 48 85 "
    "FF 74 34 48 8B 0F 33 DB 48 85 C9 74 2A 66 66 90 83 FB 08 74 22 48 8B 06 48 8B D1 4C 8B C5 48 8B "
    "CE FF 90 78 06 00 00 48 8B 4F 08 48 83 C7 08 83 C3 01 48 85 C9 75 D9 48 63 15 ?? ?? ?? ?? 41 83 "
    "C4 01 49 83 C5 08 48 8B C2 48 03 C0 44 3B A4 C6 48 0F 00 00 0F 82 66 FF FF FF 4C 8B 6C 24 20 48 "
    "8B 7C 24 50 48 8B 6C 24 48 48 8B 5C 24 40 4C 8B 64 24 28 48 83 C4 30 5E C3";
constexpr int kPrepareAllPool = 0x08, kPrepareAllGS = 0x7F;
constexpr int kPrepareAllLevel[] = {0x19, 0xDA};

// the call in the list processing: if (r_ShadowsForwardPass && !(nFlags & SHDF_ZPASS_ONLY)) PrepareAllDepthMaps()
constexpr const char kGate[] = "83 3D ?? ?? ?? ?? 00 74 0E 41 F6 C5 08 75 08 48 8B CB E8 ?? ?? ?? ??";
constexpr int kGateForward = 0x02, kGateCall = 0x12;
// the start of the list processing, before the shadow pass: m_PersFlags2 0x10000 if nFlags & 0x80, and 0x200 for an
// HDR scene: (nFlags & 1) && r_HDRRendering && !r_MeasureOverdraw && (r_HDRAllowNonFP || HDR format (+0x287A8) <= 1)
// && !(m_PersFlags & 0x1000000)
constexpr const char kHdr[] =
    "45 84 C0 79 0A 81 8B F8 0A 00 00 00 00 01 00 83 3D ?? ?? ?? ?? 00 74 0C B2 01 48 8B CB E8 ?? ?? "
    "?? ?? EB 0A 33 D2 48 8B CB E8 ?? ?? ?? ?? 41 8B FD 33 F6 B9 03 00 00 00 83 E7 01 0F 84 ?? ?? ?? "
    "?? 39 35 ?? ?? ?? ?? B0 01 0F 44 C6 0F B6 C8 8B 05 ?? ?? ?? ?? 85 C0 0F 45 CE 39 35 ?? ?? ?? ?? "
    "75 09 83 BB A8 87 02 00 01 7F 48 84 C9 74 59 0F BA A3 F4 0A 00 00 18 72 48 85 C0 75 3D C6 83 FC "
    "0E 00 00 01 83 3D ?? ?? ?? ?? 01 75 0A B2 01 48 8B CB E8 ?? ?? ?? ?? 40 38 B3 FC 0E 00 00 0F 84 "
    "?? ?? ?? ?? 81 8B F8 0A 00 00 00 02 00 00";
constexpr int kHdrRendering = 0x43, kHdrOverdraw = 0x51, kHdrAllowNonFP = 0x5C, kHdrLevel = 0x86;
constexpr int kRHdrFormat = 0x287A8;

// PrepareDepthMap(frustum, light), from its start through the early-outs (no casters, map up to date, r_ShadowGen 0)
constexpr const char kPdmPrologue[] =
    "48 85 D2 0F 84 ?? ?? ?? ?? 4C 8B DC 4D 89 43 18 56 57 48 81 EC E8 09 00 00 48 8B 82 10 01 00 00 "
    "48 8B FA 48 8B F1 48 85 C0 0F 84 ?? ?? ?? ?? 83 78 08 00 0F 8E ?? ?? ?? ?? 8B 81 F0 91 02 00 49 "
    "89 5B 08 49 89 6B E8 39 82 F8 00 00 00 4D 89 6B D8 4D 89 73 D0 49 BD 3F 3F 3F 3F 3F 3F 3F 3F 74 "
    "0D 89 82 F8 00 00 00 44 89 AA 86 00 00 00 48 8B 01 33 DB 44 8B F3 FF 90 38 06 00 00 84 C0 74 3B "
    "83 3D ?? ?? ?? ?? 01 75 32 48 8B 0D ?? ?? ?? ?? 33 D2 8B 81 F4 91 02 00 F7 B1 CC 8F 02 00 C6 87 "
    "85 00 00 00 3F 4C 63 F2 41 38 9C 3E 86 00 00 00 76 09 41 C6 84 3E 86 00 00 00 3F 48 8B 87 C8 00 "
    "00 00 48 8B EB 48 85 C0 74 7A 38 9F 84 00 00 00 48 8B E8 74 16 49 63 C6 0F B6 8C 38 86 00 00 00 "
    "84 8F 85 00 00 00 0F 94 C2 EB 0D 49 63 C6 38 9C 38 86 00 00 00 0F 96 C2 48 8B 8D 98 00 00 00 48 "
    "85 C9 74 18 48 8B 87 70 03 00 00 48 39 85 D8 00 00 00 75 08 84 D2 0F 85 ?? ?? ?? ?? 48 85 C9 74 "
    "10 48 8B 87 70 03 00 00 48 39 85 D8 00 00 00 74 07 44 89 AF 86 00 00 00 39 1D ?? ?? ?? ?? 0F 84 "
    "?? ?? ?? ??";
constexpr int kPdmShadowGen = 0x13A;
constexpr size_t kPdmSize = 0x2A00;  // the function with its fragments is 0x28B5 bytes in build 6156

// inside PrepareDepthMap, in this order:
// +0xF0 (deferred: a point light's sides in one 3x2 texture) = r_ShadowsDeferredMode && +0x84 (point light)
constexpr const char kPdmDeferred[] =
    "39 1D ?? ?? ?? ?? 74 11 38 9F 84 00 00 00 74 09 C6 87 F0 00 00 00 01 EB 06 88 9F F0 00 00 00";
constexpr int kPdmDeferredCvar = 0x02;
// the map's format and +0x8B ("normal" map): 0x1F/0x20 if neither +0x82 nor +0x83, else 0x13
constexpr const char kPdmFormat[] =
    "38 9F 83 00 00 00 75 3E 38 9F 82 00 00 00 75 36 83 3D ?? ?? ?? ?? 04 44 8B E3 C6 87 8B 00 00 00 "
    "01 41 0F 94 C4 41 83 C4 1F 38 9F 84 00 00 00 74 0C 84 D2 75 08 88 9F 8A 00 00 00 EB 22 C6 87 8A "
    "00 00 00 01 EB 19 41 BC 13 00 00 00 88 9F 8A 00 00 00 C6 87 8C 00 00 00 01 88 9F 8B 00 00 00";
constexpr int kPdmFormatCvar = 0x12;  // cmp [r_ShadowTexFormat], 4
// m_PersFlags (+0xAF4) and m_PersFlags2 (+0xAF8) for shadow generation
constexpr const char kPdmFlags[] =
    "8B 86 F4 0A 00 00 89 84 24 88 01 00 00 0F BA F0 1A 0F BA E8 14 89 86 F4 0A 00 00 80 BF 82 00 00 "
    "00 00 74 0D 41 8B C5 0F BA E8 08 89 86 F8 0A 00 00 F6 87 80 03 00 00 02 75 0A 81 8E F8 0A 00 00 "
    "00 00 80 00 41 83 FC 15 74 30 41 83 FC 13 74 2A 41 83 FC 14 74 24 41 83 FC 11 74 1E 41 83 FC 1D "
    "74 18 41 83 FC 1C 74 12 41 83 FC 1E 74 0C 41 83 FC 1F 74 06 41 83 FC 20 75 07 83 8E F8 0A 00 00 "
    "04 41 83 FC 15 74 12 41 83 FC 13 74 0C 41 83 FC 14 74 06 41 83 FC 11 75 0A 81 8E F8 0A 00 00 00 "
    "00 04 00";
// a "normal" map is cleared, which also sets m_PersFlags2 0x4000000
constexpr const char kPdmClear[] =
    "80 BF 8B 00 00 00 00 0F 84 ?? ?? ?? ?? 48 8B 06 41 83 C8 FF BA 00 F0 00 00 48 8B CE FF 90 B0 06 "
    "00 00 81 8E F8 0A 00 00 00 00 00 04";
// the caster loop, from SetCullMode to the nested EF_EndEf3D
constexpr const char kPdmLoop[] =
    "4C 8B 1E 33 D2 48 8B CE 41 FF 93 48 04 00 00 48 8B 0D ?? ?? ?? ?? 89 1D ?? ?? ?? ?? 48 8B 01 FF "
    "90 80 02 00 00 F3 0F 10 40 0C F3 0F 10 48 1C F3 0F 11 84 24 D0 01 00 00 F3 0F 10 40 2C F3 0F 11 "
    "8C 24 D4 01 00 00 C7 05 ?? ?? ?? ?? 01 00 00 00 48 83 BF 10 01 00 00 00 F3 0F 11 84 24 D8 01 00 "
    "00 0F 84 ?? ?? ?? ?? 48 8B 06 48 8B CE FF 90 80 03 00 00 48 8B 87 10 01 00 00 44 8B EB 83 78 08 "
    "00 0F 8E 72 01 00 00 4C 8B E3 90 66 66 66 90 66 66 66 90 66 66 66 90 48 8B 87 10 01 00 00 48 8D "
    "54 24 68 48 8B 08 49 8B 1C 0C 48 8B CB 48 8B 03 FF 50 10 80 BF 84 00 00 00 00 74 1A 48 8D 54 24 "
    "68 48 8D 8C 24 A0 04 00 00 E8 ?? ?? ?? ?? 84 C0 0F 84 08 01 00 00 0F BA A7 80 03 00 00 1F 73 19 "
    "48 8B 03 4C 8B 87 70 03 00 00 33 D2 48 8B CB FF 10 84 C0 0F 85 E5 00 00 00 48 8D 8C 24 C0 02 00 "
    "00 33 D2 41 B8 C8 00 00 00 E8 ?? ?? ?? ?? 8B 8C 24 14 03 00 00 C7 84 24 FC 02 00 00 02 00 00 00 "
    "F3 0F 11 BC 24 F8 02 00 00 F3 0F 11 BC 24 40 03 00 00 41 0F 28 D3 83 C9 0F C6 84 24 60 03 00 00 "
    "01 C7 84 24 DC 02 00 00 04 00 00 00 89 8C 24 14 03 00 00 33 C0 66 90 F3 0F 10 84 04 D0 01 00 00 "
    "F3 0F 10 4C 04 68 0F 2F C1 73 0E F3 0F 5C C1 F3 0F 59 C0 F3 0F 58 D0 EB 17 F3 0F 10 4C 04 74 0F "
    "2F C1 76 0C F3 0F 5C C8 F3 0F 59 C9 F3 0F 58 D1 48 83 C0 04 48 83 F8 0C 7C BD F3 0F 51 C2 48 89 "
    "9C 24 38 03 00 00 F3 0F 11 84 24 10 03 00 00 0F BA 63 0C 09 73 0B 0F BA E9 09 89 8C 24 14 03 00 "
    "00 48 8B 03 48 8D 94 24 C0 02 00 00 48 8B CB FF 50 08 48 85 ED 74 07 83 85 E0 00 00 00 01 48 8B "
    "87 10 01 00 00 41 83 C5 01 49 83 C4 08 44 3B 68 08 0F 8C A0 FE FF FF 33 DB 48 8B 06 33 D2 48 8B "
    "CE 48 89 BE 88 17 00 00 FF 90 98 03 00 00";
constexpr int kPdmLoopSystem = 0x12;

// CD3D9Renderer::EF_StartEf: its start, the polygon-list reset of level 0 and the per-level bookkeeping at its end
constexpr const char kStartEfHead[] =
    "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 40 45 33 F6 4C 8B F9 48 8D 3D ?? ?? ?? "
    "?? 44 39 35 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8D 35 ?? ?? ?? ?? 0F 85 ?? ?? ?? ??";
constexpr int kStartEfCounts = 0x1D, kStartEfLevel = 0x24, kStartEfStarts = 0x2B, kStartEfCountsEnd = 0x32;
constexpr const char kStartEfPoly[] =
    "44 89 35 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 44 89 30 48 83 C0 10 48 3B C1 7C "
    "F4 49 8B CF 45 89 B7 08 29 01 00";
constexpr int kStartEfPolyFirst = 0x0A, kStartEfPolyEnd = 0x11;
constexpr const char kStartEfTail[] =
    "48 63 0D ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 6B C9 78 48 03 C8 66 66 66 90 66 66 66 90 66 66 66 "
    "90 BA 0F 00 00 00 66 66 66 90 66 66 90 66 66 66 90 8B 07 48 83 C1 04 48 83 C7 10 48 83 EA 01 89 "
    "41 FC 75 ED 48 3B FE 7C D8 41 8B 87 A0 41 01 00 45 89 B7 C0 41 01 00 48 63 0D ?? ?? ?? ?? 41 89 "
    "84 CF EC 89 02 00 48 63 0D ?? ?? ?? ?? 41 8B 87 E0 41 01 00 41 89 84 CF F0 89 02 00 83 05 ?? ?? "
    "?? ?? 01";
constexpr int kStartEfTailStarts = 0x0A;
constexpr int kStartEfTailLevel[] = {0x03, 0x5A, 0x69};  // movsxd from [level] (x3); then add [level], 1 at +0x7E
constexpr int kStartEfTailInc = 0x7E;

// SRendItem::mfAdd: appends to m_RendItems[nAW][nList], growing it through the module's realloc
constexpr const char kAddItem[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 48 83 EC 20 48 63 5C 24 60 41 8B C1 "
    "4C 8B E1 33 FF 49 8B F0 48 8B EA 48 6B DB 0F 48 03 D8 48 8D 05 ?? ?? ?? ?? 48 C1 E3 04 48 03 D8 "
    "4C 63 6B 08 41 8D 4D 01 3B 4B 0C 89 4B 08 76 27 8B C1 D1 E8 8D 4C 08 0A 85 C9 89 4B 0C 75 05 48 "
    "89 3B EB 13 48 8D 14 89 48 8B 0B 48 C1 E2 03 E8 ?? ?? ?? ?? 48 89 03 4C 8B 03 4B 8D 54 AD 00 49 "
    "89 6C D0 18 8B 45 10 0F BF 8D CC 00 00 00 25 00 00 FF FF 0B C8 41 89 4C D0 10 8B 85 8C 00 00 00 "
    "41 89 44 D0 20 8B 46 14 0B 44 24 68 41 89 44 D0 24 48 8B 46 08 48 85 C0 74 03 8B 78 08 48 8B 06 "
    "48 8B 5C 24 40 48 8B 6C 24 48 8B 48 14 8B 46 10 48 8B 74 24 50 C1 E7 0C 83 E0 3F 4D 89 64 D0 08 "
    "0B CF C1 E1 06 0B C8 41 89 0C D0 48 83 C4 20 41 5D 41 5C 5F C3";
constexpr int kAddItemLists = 0x35, kAddItemRealloc = 0x6F;
// CryModuleRealloc(ptr, bytes): CryMalloc when ptr is null, else CryRealloc; returns the block
constexpr const char kRealloc[] =
    "48 89 74 24 20 57 48 83 EC 20 8B 05 ?? ?? ?? ?? 48 8B FA 48 8B F1 85 C0 75 0B E8 ?? ?? ?? ?? 8B "
    "05 ?? ?? ?? ?? 48 85 F6 75 3C 85 C0 75 05 E8 ?? ?? ?? ?? 48 8D 54 24 30 48 8B CF FF 15 ?? ?? ?? "
    "?? 48 8B 4C 24 30 48 01 3D ?? ?? ?? ?? 48 01 0D ?? ?? ?? ?? 83 05 ?? ?? ?? ?? 01 48 8B 74 24 48 "
    "48 83 C4 20 5F C3 48 8B D7 48 8B CE 48 89 5C 24 38 FF 15 ?? ?? ?? ?? 4C 8D 44 24 40 48 8B D7 48 "
    "8B CE 48 8B D8 FF 15 ?? ?? ?? ?? 48 8B 4C 24 40 83 05 ?? ?? ?? ?? 01 48 01 0D ?? ?? ?? ?? 48 01 "
    "3D ?? ?? ?? ?? 48 8B 74 24 48 81 E3 FF FF FF 7F 48 01 1D ?? ?? ?? ?? 48 8B 5C 24 38 48 83 C4 20 "
    "5F C3";

// PrepareDepthMap's side loop (a point light's map has 6 sides, a directional map 1), in this order between kPdmFlags
// and kPdmClear. Its start: the constants it keeps in registers (|x| mask, 0.05, -0.0) and the side mask of a point
// light's map (bit i of +0x85 & dirty renders side i, and clears the dirty bit)
constexpr const char kPdmLoopHead[] =
    "44 0F 29 9C 24 60 09 00 00 44 0F 29 A4 24 50 09 00 00 F3 44 0F 10 25 ?? ?? ?? ?? 49 63 C4 B9 70 "
    "00 00 00 4C 8D B4 24 C4 02 00 00 48 89 84 24 A0 03 00 00 48 63 C3 48 89 8C 24 70 01 00 00 44 0F "
    "29 B4 24 30 09 00 00 44 0F 29 BC 24 20 09 00 00 44 0F 29 8C 24 80 09 00 00 F3 44 0F 10 3D ?? ?? "
    "?? ?? F3 44 0F 10 35 ?? ?? ?? ?? 48 89 84 24 C0 01 00 00 44 0F 29 94 24 70 09 00 00 44 0F 29 AC "
    "24 40 09 00 00 66 90 66 66 90 80 BF 84 00 00 00 00 74 44 45 0F B6 84 39 86 00 00 00 8A 97 85 00 "
    "00 00 41 8B CF 41 8A C0 23 D0 B8 01 00 00 00 D3 E0 84 D0 0F 84 ?? ?? ?? ?? B8 01 00 00 00 D2 E0 "
    "48 8B 8C 24 70 01 00 00 F6 D0 41 22 C0 41 88 84 39 86 00 00 00 EB 09 41 C6 84 39 86 00 00 00 00";
constexpr int kLoopAbsMask = 0x17, kLoopThreshold = 0x5E, kLoopNegZero = 0x67;
// each side's camera starts as a default CCamera (at rsp+0x4A0)
constexpr const char kPdmCamInit[] = "48 8D 8C 24 A0 04 00 00 E8 ?? ?? ?? ?? 80 BF 84 00 00 00 00";
constexpr int kCamInitCall = 0x08;
// a point-light side's camera: the side table (direction, up, roll in degrees, 7 floats per side), the look-at basis
// with the roll (sinf/cosf), the light position as translation, then CCamera::UpdateFrustum and SetFrustum(size, size,
// fov, near, far): fov 90 degrees, or a global (95 degrees) in deferred mode (+0xF0). Fields: the constants (1, -1, -90,
// 90, degrees to radians, FLT_MIN x3, -0.0, pi/2, the fov global, degrees to radians as a double), the calls (E8).
constexpr const char kPdmOmni[] =
    "45 0F 57 DB F3 0F 10 3D ?? ?? ?? ?? F3 44 0F 10 0D ?? ?? ?? ?? 48 8D 8C 24 E0 01 00 00 48 8D 94 "
    "24 20 01 00 00 F3 0F 10 05 ?? ?? ?? ?? F3 0F 11 BC 24 C0 02 00 00 F3 44 0F 11 9C 24 C4 02 00 00 "
    "F3 0F 11 84 24 D8 02 00 00 F3 0F 10 05 ?? ?? ?? ?? F3 44 0F 11 9C 24 C8 02 00 00 F3 0F 11 84 24 "
    "F4 02 00 00 F3 44 0F 11 9C 24 CC 02 00 00 F3 44 0F 11 9C 24 D0 02 00 00 F3 0F 11 BC 24 D4 02 00 "
    "00 F3 44 0F 11 8C 24 DC 02 00 00 F3 44 0F 11 9C 24 E0 02 00 00 F3 44 0F 11 9C 24 E4 02 00 00 F3 "
    "44 0F 11 9C 24 E8 02 00 00 F3 44 0F 11 9C 24 EC 02 00 00 F3 0F 11 BC 24 F0 02 00 00 F3 44 0F 11 "
    "9C 24 F8 02 00 00 F3 0F 11 BC 24 FC 02 00 00 F3 44 0F 11 9C 24 00 03 00 00 F3 44 0F 11 9C 24 04 "
    "03 00 00 F3 44 0F 11 9C 24 08 03 00 00 F3 44 0F 11 8C 24 0C 03 00 00 F3 44 0F 11 9C 24 10 03 00 "
    "00 F3 44 0F 11 9C 24 14 03 00 00 F3 44 0F 11 8C 24 18 03 00 00 F3 44 0F 11 9C 24 1C 03 00 00 F3 "
    "44 0F 11 9C 24 20 03 00 00 F3 44 0F 11 9C 24 24 03 00 00 F3 0F 11 BC 24 28 03 00 00 F3 44 0F 11 "
    "9C 24 2C 03 00 00 F3 44 0F 11 9C 24 30 03 00 00 F3 44 0F 11 9C 24 34 03 00 00 F3 0F 11 BC 24 38 "
    "03 00 00 F3 44 0F 11 9C 24 3C 03 00 00 F3 0F 11 BC 24 40 03 00 00 F3 44 0F 11 9C 24 44 03 00 00 "
    "F3 44 0F 11 9C 24 48 03 00 00 F3 44 0F 11 9C 24 4C 03 00 00 F3 44 0F 11 9C 24 50 03 00 00 F3 44 "
    "0F 11 8C 24 54 03 00 00 F3 44 0F 11 9C 24 58 03 00 00 F3 0F 11 BC 24 5C 03 00 00 F3 44 0F 11 9C "
    "24 60 03 00 00 F3 44 0F 11 9C 24 64 03 00 00 F3 41 0F 10 46 14 F3 0F 59 05 ?? ?? ?? ?? F3 41 0F "
    "10 76 08 F3 45 0F 10 56 0C F3 41 0F 10 5E 10 F3 45 0F 10 46 FC F3 41 0F 10 26 F3 41 0F 10 6E 04 "
    "F3 0F 11 B4 24 E0 01 00 00 F3 44 0F 11 94 24 E4 01 00 00 F3 0F 11 9C 24 E8 01 00 00 48 8B 01 48 "
    "89 02 8B 41 08 F3 0F 11 84 24 B8 00 00 00 89 42 08 41 0F 28 C0 41 0F 54 C4 41 0F 2F C3 0F 87 99 "
    "00 00 00 0F 28 C4 41 0F 54 C4 41 0F 2F C3 0F 87 88 00 00 00 0F 28 C5 41 0F 54 C4 41 0F 2F C3 77 "
    "7B 48 8D 4C 24 68 F3 0F 11 7C 24 68 F3 44 0F 11 5C 24 6C 48 8D 94 24 40 02 00 00 F3 44 0F 11 5C "
    "24 70 F3 44 0F 11 5C 24 74 F3 0F 11 7C 24 78 F3 44 0F 11 5C 24 7C F3 44 0F 11 9C 24 80 00 00 00 "
    "F3 44 0F 11 9C 24 84 00 00 00 F3 0F 11 BC 24 88 00 00 00 48 8B 01 48 89 02 48 8B 41 08 48 89 42 "
    "08 48 8B 41 10 48 89 42 10 48 8B 41 18 48 89 42 18 8B 41 20 89 42 20 E9 F3 03 00 00 41 0F 28 C8 "
    "44 0F 28 EF 0F 28 D5 F3 0F 59 D5 F3 41 0F 59 C8 0F 28 C4 F3 0F 59 C4 F3 0F 58 D0 F3 0F 58 D1 F3 "
    "0F 58 15 ?? ?? ?? ?? F3 0F 51 C2 F3 44 0F 5E E8 45 0F 28 CD F3 45 0F 59 C8 45 0F 28 C5 F3 44 0F "
    "59 ED 45 0F 2E CB F3 44 0F 11 8C 24 B0 00 00 00 F3 44 0F 59 C4 F3 44 0F 11 44 24 58 75 56 45 0F "
    "2E C3 75 50 41 0F 54 F4 41 0F 2F F7 77 46 45 0F 54 D4 45 0F 2F D7 77 3C F3 0F 5C DF 41 0F 54 DC "
    "41 0F 2F DF 77 2E 41 0F 28 CE 41 0F 28 D3 41 0F 28 F3 F3 41 0F 5C CD F3 0F 11 94 24 24 01 00 00 "
    "F3 0F 11 B4 24 28 01 00 00 F3 0F 11 8C 24 20 01 00 00 EB 1B F3 0F 10 B4 24 28 01 00 00 F3 0F 10 "
    "94 24 24 01 00 00 F3 0F 10 8C 24 20 01 00 00 0F 28 E2 F3 41 0F 59 D1 0F 28 C6 F3 41 0F 59 C0 F3 "
    "41 0F 59 E5 F3 41 0F 59 F1 0F 28 EF 41 0F 28 D8 45 0F 28 F8 F3 44 0F 11 4C 24 6C F3 44 0F 11 44 "
    "24 78 F3 44 0F 11 AC 24 84 00 00 00 F3 0F 5C E0 41 0F 28 C5 F3 0F 59 C1 F3 0F 59 D9 0F 28 CC F3 "
    "0F 5C F0 F3 0F 5C DA F3 0F 59 CC 0F 28 C6 0F 28 D3 F3 0F 59 C6 F3 0F 59 D3 F3 0F 58 D0 F3 0F 58 "
    "D1 F3 0F 58 15 ?? ?? ?? ?? F3 0F 51 C2 F3 0F 5E E8 44 0F 28 D5 F3 44 0F 59 D4 0F 28 E5 F3 0F 59 "
    "EB 0F 28 C5 44 0F 28 F5 F3 44 0F 11 94 24 A8 00 00 00 F3 41 0F 59 C0 F3 0F 59 E6 F3 45 0F 59 F1 "
    "F3 0F 11 6C 24 48 F3 44 0F 11 54 24 68 F3 0F 11 AC 24 80 00 00 00 F3 0F 10 B4 24 B8 00 00 00 F3 "
    "0F 11 64 24 40 F3 0F 11 64 24 74 44 0F 28 E4 F3 45 0F 59 E5 F3 45 0F 59 FA 41 0F 2E F3 F3 44 0F "
    "5C E0 41 0F 28 C5 F3 41 0F 59 C2 41 0F 28 CC F3 41 0F 59 CC F3 44 0F 5C F0 0F 28 C4 F3 41 0F 59 "
    "C1 F3 44 0F 5C F8 41 0F 28 C6 F3 41 0F 59 C6 41 0F 28 D7 F3 41 0F 59 D7 F3 0F 58 D0 F3 0F 58 D1 "
    "0F 28 CF F3 0F 58 15 ?? ?? ?? ?? F3 0F 51 C2 F3 0F 5E C8 F3 44 0F 59 E1 F3 44 0F 59 F1 F3 44 0F "
    "59 F9 F3 44 0F 11 64 24 70 F3 44 0F 11 74 24 7C F3 44 0F 11 BC 24 88 00 00 00 0F 84 6C 01 00 00 "
    "0F 28 C6 E8 ?? ?? ?? ?? 44 0F 28 D0 0F 28 C6 E8 ?? ?? ?? ?? F3 0F 10 9C 24 A8 00 00 00 F3 0F 10 "
    "35 ?? ?? ?? ?? 41 0F 28 FC 41 0F 28 E6 F3 0F 58 FB F3 41 0F 5C F2 44 0F 28 C8 41 0F 28 C9 44 0F "
    "28 C6 F3 41 0F 59 FB F3 0F 10 84 24 B0 00 00 00 F3 0F 59 CB F3 45 0F 59 C4 F3 0F 58 F8 0F 28 D0 "
    "41 0F 28 C2 F3 44 0F 58 C1 F3 0F 11 7C 24 6C F3 0F 10 3D ?? ?? ?? ?? F3 0F 59 C3 F3 41 0F 59 D3 "
    "F3 45 0F 59 E1 F3 0F 10 5C 24 58 F3 44 0F 58 E0 F3 44 0F 58 C2 0F 28 C6 F3 41 0F 59 C6 F3 45 0F "
    "59 F1 0F 28 CB F3 41 0F 59 CB F3 44 0F 58 E2 F3 0F 10 54 24 40 F3 41 0F 59 F7 0F 28 EA F3 0F 58 "
    "E2 F3 44 0F 11 44 24 68 F3 44 0F 11 64 24 70 F3 41 0F 59 E9 F3 41 0F 59 E3 F3 0F 58 E3 F3 0F 58 "
    "E8 41 0F 28 DA 41 0F 28 C7 F3 45 0F 59 F9 F3 0F 59 DA 41 0F 28 D5 F3 0F 58 E9 F3 41 0F 58 DE F3 "
    "44 0F 10 74 24 48 F3 41 0F 59 D3 F3 45 0F 59 D6 F3 0F 58 D9 F3 41 0F 58 C6 F3 45 0F 58 D7 41 0F "
    "28 CE F3 41 0F 59 C9 F3 41 0F 59 C3 F3 0F 58 CE F3 44 0F 58 D2 F3 41 0F 58 C5 F3 0F 58 CA F3 0F "
    "11 6C 24 74 F3 0F 11 64 24 78 F3 0F 11 8C 24 80 00 00 00 F3 0F 11 5C 24 7C F3 0F 11 84 24 84 00 "
    "00 00 F3 44 0F 11 94 24 88 00 00 00 F3 44 0F 10 0D ?? ?? ?? ?? F3 44 0F 10 35 ?? ?? ?? ?? 48 8D "
    "4C 24 68 48 8D 94 24 40 02 00 00 48 8B 01 48 89 02 48 8B 41 08 48 89 42 08 48 8B 41 10 48 89 42 "
    "10 48 8B 41 18 48 89 42 18 8B 41 20 89 42 20 F3 0F 10 84 24 40 02 00 00 F3 0F 10 8C 24 44 02 00 "
    "00 F3 0F 10 B7 DC 00 00 00 48 8D 8C 24 A0 04 00 00 48 8D 54 24 68 41 B8 30 00 00 00 F3 44 0F 10 "
    "87 E0 00 00 00 F3 0F 11 44 24 68 F3 0F 10 84 24 48 02 00 00 F3 0F 11 44 24 70 F3 0F 10 84 24 C4 "
    "00 00 00 F3 0F 11 4C 24 6C F3 0F 10 8C 24 4C 02 00 00 F3 0F 11 44 24 74 F3 0F 10 84 24 50 02 00 "
    "00 F3 0F 11 44 24 7C F3 0F 10 84 24 C0 00 00 00 F3 0F 11 4C 24 78 F3 0F 10 8C 24 54 02 00 00 F3 "
    "0F 11 84 24 84 00 00 00 F3 0F 10 84 24 58 02 00 00 F3 0F 11 84 24 88 00 00 00 F3 0F 10 84 24 60 "
    "02 00 00 F3 0F 11 8C 24 80 00 00 00 F3 0F 10 8C 24 5C 02 00 00 F3 0F 11 84 24 90 00 00 00 F3 0F "
    "10 84 24 C8 00 00 00 F3 0F 11 84 24 94 00 00 00 F3 0F 11 8C 24 8C 00 00 00 E8 ?? ?? ?? ?? 48 8D "
    "8C 24 A0 04 00 00 E8 ?? ?? ?? ?? 80 BF F0 00 00 00 00 75 2D 8B 97 F4 00 00 00 F3 0F 10 1D ?? ?? "
    "?? ?? 48 8D 8C 24 A0 04 00 00 44 8B C2 F3 44 0F 11 44 24 28 F3 0F 11 74 24 20 E8 ?? ?? ?? ?? EB "
    "37 F3 0F 5A 05 ?? ?? ?? ?? 8B 97 F4 00 00 00 48 8D 8C 24 A0 04 00 00 44 8B C2 F3 44 0F 11 44 24 "
    "28 F3 0F 11 74 24 20 F2 0F 59 05 ?? ?? ?? ?? F2 0F 5A D8 E8 ?? ?? ?? ??";
constexpr int kOmniOne = 0x08, kOmniMinusOne = 0x11, kOmniMinus90 = 0x29, kOmniPlus90 = 0x4D, kOmniDegToRad = 0x1D9;
constexpr int kOmniFltMin[] = {0x303, 0x425, 0x4E7};
constexpr int kOmniSinf = 0x523, kOmniCosf = 0x52F, kOmniNegZero = 0x541, kOmniUpdate = 0x7C6, kOmniHalfPi = 0x7DE;
constexpr int kOmniSetFrustum[] = {0x7FA, 0x833};
constexpr int kOmniFov = 0x805, kOmniDegToRadD = 0x82B;
// the caster loop's box test: call FUN_380AF050(camera, box)
constexpr int kPdmLoopCull = 0xC9;

// Offsets that are fixed bytes in the patterns above, so a match guarantees them.
// renderer (CD3D9Renderer)
constexpr int kRLights = 0xF40;          // TArray<CDLight*> per recursion level, 16 bytes each (count at +8)
constexpr int kRPersFlags = 0xAF4, kRPersFlags2 = 0xAF8;
constexpr int kRDeviceLost = 0xA60;
constexpr int kRResetId = 0x291F0;       // bumped on resolution changes: shadow maps of an older id are all dirty
constexpr int kRObjects = 0x141A0;       // render objects in use this frame (at most 0x4000)
constexpr int kRMaxObjects = 0x4000;
constexpr int kVfIsMultiGpu = 0x638, kVfStartEf = 0x380;
// IRenderer::GetCamera and GetFrameID (Mod SDK slots 44 and 128; PrepareDepthMap calls the first itself): verify only
constexpr int kVfGetCamera = 0x160, kVfGetFrameId = 0x400;
// light (CDLight)
constexpr int kLFlags = 0x1D4, kLFrustums = 0x1F0;  // DLF_CASTSHADOW_MAPS 0x10, DLF_DIRECTIONAL 2; up to 8 frustums
// frustum (ShadowMapFrustum)
constexpr int kFCasters = 0x110;  // PodArray<IShadowCaster*>*
constexpr int kFResetId = 0xF8, kFDirty = 0x86, kFSideMask = 0x85, kFOmni = 0x84, kFTexture = 0xC8;
constexpr int kFOwner = 0x370, kFFlags = 0x380, kFFlag82 = 0x82, kFFlag83 = 0x83;
// shadow texture
constexpr int kTTexture = 0x98, kTOwner = 0xD8, kTCasters = 0xE0;
// ISystem::GetViewCamera
constexpr int kVfGetViewCamera = 0x280;
// SRendParams as the caster loop fills it (200 bytes)
constexpr int kParamsSize = 200;
constexpr int kPTechnique = 0x1C, kPAlpha = 0x38, kPRenderList = 0x3C, kPDistance = 0x50, kPObjFlags = 0x54;
constexpr int kPRenderNode = 0x78, kPRenderQuality = 0x80, kPAfterWater = 0xA0;
constexpr int kCasterRndFlags = 0xC;  // bit 9 adds object flag 0x200
// point-light maps (kPdmDeferred, the texture check after kPdmFormat, kPdmOmni)
constexpr int kFNear = 0xDC, kFFar = 0xE0, kFSize = 0xE4;  // size: at least 32, as PrepareDepthMap stores it
constexpr int kFLightPos = 0x34C, kFProjTranslation = 0x358;  // the side cameras sit at their sum
constexpr int kTFormat = 0xA0, kTType = 0xA4, kTWidth = 0xB0, kTHeight = 0xB4;  // type: 1 = 2D, 3 = cube
// CCamera (x64): the Matrix34 at 0, the planes and their box-corner indices the cull test reads at 0xF8..0x1A0
constexpr size_t kCameraSize = 0x208, kCameraPlanes = 0xF8, kCameraPlanesEnd = 0x1A0;
constexpr int kLoopCameraAt = 0x4A0, kLoopBoxAt = 0x68;  // PrepareDepthMap's frame: the side camera, the caster box
#endif  // _WIN64

enum class Mode { Off, On, Verify, Compare };
enum class At { Flush, Shadows };
enum StashState { kPending = 0, kReplayed, kCompared, kStale };
constexpr int kSides = 6;         // of a point light's map
constexpr int kMaxStashes = 256;  // per flush: one per directional map, one per rendered side of a point light's

struct Stats {
    uint64_t flushes = 0, prepasses = 0, frustums = 0, upToDate = 0, omniLeft = 0, gsLights = 0, noRoom = 0, overflow = 0;
    uint64_t rendered = 0, omniRendered = 0, sides = 0, casters = 0, items = 0;
    // the replay, per side (a directional map has one)
    uint64_t pdmCalls = 0, replayed = 0, backAtEnd = 0, notReached = 0, withoutStash = 0, stale = 0, otherSides = 0;
    uint64_t unused = 0, putBackFailed = 0;
    // compare: outputs of the original casters against the stash
    uint64_t compared = 0, mismatched = 0, elemsByContents = 0, nearMatrices = 0;
    // verify: inputs of each Render call, pre-pass against PrepareDepthMap's loop
    uint64_t vSides = 0, vSidesSame = 0, vCalls = 0, vCount = 0, vOrder = 0, vParams = 0, vFlags = 0, vFlags2 = 0;
    uint64_t vLevel = 0, vFrameRec = 0, vFrame = 0, vCamera = 0, vView = 0, vLogged = 0;
    uint64_t vSideCameras = 0, vSideCamerasDiffer = 0, vSideCamerasMissed = 0;  // point-light sides' cameras
    uint32_t vFlagsMask = 0, vFlags2Mask = 0;
    uint64_t polygons = 0, multiGpu = 0, otherThread = 0;
    uint64_t prepassCycles = 0, replayCycles = 0;
    // the outer flush's timing: from its entry to the original's call (the Phase 2 parts' work before it), the original
    // itself, and from one entry to the next (a frame)
    uint64_t entryCycles = 0, flushCycles = 0, frameCycles = 0, frames = 0, lastEntry = 0;
    // the parts' steps after the outer flush (kAfterSteps, then the reflections'), each's time, and how many ran
    uint64_t afterStepCycles[16] = {}, afterRuns = 0;
    int maxObjects = 0;
};

#ifdef _WIN64
using EndEf3DFn = void (*)(char* renderer, int flags);
using PrepareAllFn = void (*)(char* renderer);
using PrepareDepthMapFn = void (*)(char* renderer, char* frustum, char* light);
using RendererFn = void (*)(char* renderer);
using IsMultiGpuFn = bool (*)(char* renderer);
using GetViewCameraFn = const float* (*)(void* system);
// IShadowCaster: HasOcclusionmap(nLod, pLightOwner), Render(const SRendParams&), GetBBoxVirtual() (AABB returned
// through a hidden pointer after `this`)
using HasOcclusionmapFn = bool (*)(void* self, int lod, void* owner);
using RenderFn = bool (*)(void* self, const void* params);
using GetBBoxFn = void* (*)(void* self, float* box);
using GetCharacterFn = void* (*)(void* self, unsigned slot, void* matrix, bool onlyVisible);
using GetCameraFn = const float* (*)(char* renderer);
using GetFrameIdFn = int (*)(char* renderer, bool includeRecursive);
// point-light sides: the game's CCamera code, its box test and the CRT's sinf/cosf
using CamInitFn = void* (*)(void* camera);
using CamUpdateFn = void (*)(void* camera);
using CamSetFrustumFn = void (*)(void* camera, int width, int height, float fov, float zNear, float zFar);
using CullBoxFn = bool (*)(const void* camera, const float* box);
using FloatFn = float (*)(float);

// verify mode: stands in for one caster in PrepareDepthMap's list
struct Spy {
    void* const* vtbl;
    uint32_t pad;
    uint32_t rndFlags;  // +0xC, tested by the caster loop: the real caster's
    void* real;
};

struct PodArrayPtr {  // PodArray<T*> (x64): elements, count, capacity
    void** elems;
    int count;
    int capacity;
};
#endif

// The frustum PrepareDepthMap is replaying: its stashes in side order, and the side its loop is on (a side ends at its
// nested EF_EndEf3D).
struct Replay {
    sp::Stash* sides[kSides] = {};
    int count = 0, cursor = 0;
    int renders = 0;             // Render calls of the current side's loop (the placeholder's, or the spies')
    bool active = false;
    bool cameraChecked = false;  // verify: the current side's camera was compared
    sp::Stash* Current() const { return active && cursor < count ? sides[cursor] : nullptr; }
};

struct State {
    Mode mode = Mode::Off;
    At at = At::Flush;
    bool installed = false;
    bool omni = false;  // point-light maps too: the code of their side cameras was found
    ShadowPrepassTarget t;
#ifdef _WIN64
    EndEf3DFn endEf3D = nullptr;
    PrepareAllFn prepareAll = nullptr;
    PrepareDepthMapFn prepareDepthMap = nullptr;
#endif
    DWORD mainThread = 0;
    std::vector<sp::Stash> stashes;
    int used = 0;  // stashes of the current flush
    Replay replay;
    // verify: the spies of the frustum being replayed, and whether the current side differs
#ifdef _WIN64
    std::vector<Spy> spies;
#endif
    std::vector<void*> spyElems;
    char* renderer = nullptr;
    bool spyDiffers = false;
    bool loggedFirst = false;
    Stats st;
};
State g;

Mode ReadMode() {
    const std::wstring v = ConfigString(L"ShadowPrepass", L"Mode", L"OPTIPERF_SHADOW_PREPASS", L"off");
    if (_wcsicmp(v.c_str(), L"on") == 0 || v == L"1") return Mode::On;
    if (_wcsicmp(v.c_str(), L"verify") == 0) return Mode::Verify;
    if (_wcsicmp(v.c_str(), L"compare") == 0) return Mode::Compare;
    return Mode::Off;
}

At ReadAt() {
    const std::wstring v = ConfigString(L"ShadowPrepass", L"At", L"OPTIPERF_SHADOW_PREPASS_AT", L"flush");
    return _wcsicmp(v.c_str(), L"shadows") == 0 ? At::Shadows : At::Flush;
}

const char* ModeName(Mode m) {
    return m == Mode::On ? "on" : m == Mode::Verify ? "verify" : m == Mode::Compare ? "compare" : "off";
}

#ifdef _WIN64
// the parts' steps after the outer flush, in their order (the reflections' last, apart: it takes the renderer)
void EndOfFlush();
struct AfterStep {
    const char* name;
    void (*fn)();
};
const AfterStep kAfterSteps[] = {{"frame phases", &FramePhases_FlushEnd},   {"census", &Census_AfterFlush},
                                 {"lights", &LightSnapshot_AfterFlush},     {"engine data", &EngineData_AfterFlush},
                                 {"system", &SysSnapshot_AfterFlush},       {"cvars", &CvarSnapshot_AfterFlush},
                                 {"environment", &EnvSnapshot_AfterFlush},  {"particles", &ParticlePrepass_AfterFlush},
                                 {"skinning", &SkinPrepass_AfterFlush},     {"materials", &MaterialSnapshot_AfterFlush},
                                 {"the shadow pre-pass", &EndOfFlush}};
static_assert(std::size(kAfterSteps) < sizeof(Stats::afterStepCycles) / sizeof(uint64_t),
              "room for each step's time, and the reflections'");
#endif

void Report(const char* when) {
    if (!g.installed) return;
    const Stats& s = g.st;
    Log("shadow prepass [%s]: mode=%s at=%s point lights=%s | flushes=%llu pre-passes=%llu | maps=%llu: rendered "
        "early=%llu (point lights %llu) in %llu sides, up to date=%llu, point lights left to the game=%llu, no room=%llu, "
        "too many=%llu; geometry-shader lights=%llu | casters=%llu items=%llu, most render objects in a frame=%d of %d",
        when, ModeName(g.mode), g.at == At::Flush ? "flush" : "shadows", g.omni ? "yes" : "no", s.flushes, s.prepasses,
        s.frustums, s.rendered, s.omniRendered, s.sides, s.upToDate, s.omniLeft, s.noRoom, s.overflow, s.gsLights,
        s.casters, s.items, s.maxObjects,
#ifdef _WIN64
        kRMaxObjects
#else
        0
#endif
    );
    Log("shadow prepass [%s]: PrepareDepthMap calls=%llu: sides replayed=%llu (put back at the side's end: %llu), not "
        "reached=%llu | maps without stash=%llu, caster list changed=%llu, other sides than predicted=%llu | unused "
        "stashes=%llu, put-back failures=%llu | polygons added by casters=%llu, multi-GPU flushes=%llu, calls from other "
        "threads=%llu",
        when, s.pdmCalls, s.replayed, s.backAtEnd, s.notReached, s.withoutStash, s.stale, s.otherSides, s.unused,
        s.putBackFailed, s.polygons, s.multiGpu, s.otherThread);
    if (g.mode == Mode::Verify)
        Log("shadow prepass [%s]: VERIFY sides=%llu with identical inputs=%llu | Render calls=%llu: calls missing or extra "
            "in %llu sides, other caster=%llu, SRendParams=%llu, m_PersFlags=%llu (bits 0x%08X), m_PersFlags2=%llu (bits "
            "0x%08X), level=%llu, frame id with recursion=%llu, frame id=%llu, renderer camera=%llu, view camera=%llu | "
            "point-light side cameras compared=%llu, different=%llu, not checked=%llu",
            when, s.vSides, s.vSidesSame, s.vCalls, s.vCount, s.vOrder, s.vParams, s.vFlags, s.vFlagsMask, s.vFlags2,
            s.vFlags2Mask, s.vLevel, s.vFrameRec, s.vFrame, s.vCamera, s.vView, s.vSideCameras, s.vSideCamerasDiffer,
            s.vSideCamerasMissed);
    if (g.mode == Mode::Compare)
        Log("shadow prepass [%s]: COMPARE sides compared=%llu mismatched=%llu | elements matched by contents=%llu, objects "
            "with matrices equal within %.0e=%llu",
            when, s.compared, s.mismatched, s.elemsByContents, double(sp::kMatrixTolerance), s.nearMatrices);
    Log("shadow prepass [%s]: cycles per pre-pass=%.0f, per replayed side=%.0f", when,
        double(s.prepassCycles) / double(s.prepasses ? s.prepasses : 1),
        double(s.replayCycles) / double(s.replayed ? s.replayed : 1));
    const double fl = double(s.flushes ? s.flushes : 1), fr = double(s.frames ? s.frames : 1);
    Log("shadow prepass [%s]: per outer flush: before the original %.3f ms, the original %.3f ms | between flushes %.3f ms",
        when, double(s.entryCycles) / 3.0e6 / fl, double(s.flushCycles) / 3.0e6 / fl, double(s.frameCycles) / 3.0e6 / fr);
#ifdef _WIN64
    if (s.afterRuns) {  // (the steps after the flush: at its end, or at the join of a flush beside the main thread)
        char steps[600] = "";
        size_t used = 0;
        const double runs = double(s.afterRuns);
        for (size_t i = 0; i <= std::size(kAfterSteps); ++i) {
            const char* const name = i < std::size(kAfterSteps) ? kAfterSteps[i].name : "the reflections";
            const int w = snprintf(steps + used, sizeof steps - used, "%s%s %.1f", used ? ", " : "", name,
                                   double(s.afterStepCycles[i]) / 3.0e3 / runs);
            if (w < 0 || size_t(w) >= sizeof steps - used) break;
            used += size_t(w);
        }
        Log("shadow prepass [%s]: the steps after the flush, us per flush: %s", when, steps);
    }
#endif
}

#ifdef _WIN64
// --- runtime ---------------------------------------------------------------------------------------------------------
template <class T>
T& At_(char* p, int offset) {
    return *reinterpret_cast<T*>(p + offset);
}

sp::ItemList* Lists() { return reinterpret_cast<sp::ItemList*>(g.t.lists); }

// EF_EndEf3D's thread: the main thread (the outer flush, the reflections' captures before it) or the flush's (the
// nested flushes in it: a shadow map's side, a reflection left to the flush). They differ with FlushThread=worker.
bool OnPartThread() {
    const DWORD tid = GetCurrentThreadId();
    if (tid == g.mainThread || tid == FlushThread()) return true;
    ++g.st.otherThread;
    return false;
}

// the flush's thread: the shadow maps are prepared in the flush
bool OnFlush() {
    if (OnFlushThread()) return true;
    ++g.st.otherThread;
    return false;
}

// the outer flush handed to the flush's thread, and what comes after it (on the main thread: right after it, or with
// FlushThread=overlap where the main thread joins it)
struct OuterFlush {
    char* r;
    int flags;
    uint64_t t1;  // its start
};
OuterFlush g_outer;

void EndOfFlush();

void RunOuterFlush(void* p) {
    const auto* f = static_cast<const OuterFlush*>(p);
    RenderQueue_OnFlushStart();  // (FlushThread=overlap: the ocean's grid updated here when the flush is to draw it)
    g.endEf3D(f->r, f->flags);
}

// (the step in progress, for a stall's log: ShadowPrepass_AfterStep)
const char* volatile g_afterStep = nullptr;

void AfterOuterFlush(void* p) {
    const auto* f = static_cast<const OuterFlush*>(p);
    g.st.flushCycles += __rdtsc() - f->t1;
    ++g.st.afterRuns;
    for (size_t i = 0; i < std::size(kAfterSteps); ++i) {
        const AfterStep& s = kAfterSteps[i];
        g_afterStep = s.name;
        const uint64_t c = __rdtsc();
        s.fn();
        g.st.afterStepCycles[i] += __rdtsc() - c;
    }
    g_afterStep = "the reflections";
    const uint64_t c = __rdtsc();
    ReflectionPrepass_OnFlushEnd(f->r);
    g.st.afterStepCycles[std::size(kAfterSteps)] += __rdtsc() - c;
    g_afterStep = nullptr;
}

int PolygonCount(int level) { return g.t.polyLists ? *reinterpret_cast<int*>(g.t.polyLists + level * 16 + 8) : 0; }

uintptr_t CasterHash(const PodArrayPtr* list) {
    uintptr_t h = 0;
    for (int i = 0; i < list->count; ++i) h = h * 31 + reinterpret_cast<uintptr_t>(list->elems[i]);
    return h;
}

int SideCount(uint8_t mask) {
    int n = 0;
    for (; mask; mask = uint8_t(mask & (mask - 1))) ++n;
    return n;
}

// deferred mode (PrepareDepthMap sets +0xF0 to this): a point light's six sides share one 3x2 texture
bool Deferred(char* f) { return At_<uint8_t>(f, kFOmni) && *g.t.shadowsDeferred; }

// The sides PrepareDepthMap will render, from its early-outs and texture checks, without their side effects: bit 0 for
// a directional map, bit i for side i of a point light's; 0 if it renders nothing (the map is up to date).
uint8_t SideMask(char* r, char* f) {
    auto* casters = At_<PodArrayPtr*>(f, kFCasters);
    if (!casters || casters->count <= 0) return 0;
    const bool omni = At_<uint8_t>(f, kFOmni) != 0;
    uint8_t dirty = At_<uint8_t>(f, kFDirty);  // GPU 0 (multi-GPU flushes are skipped)
    if (At_<int>(f, kFResetId) != At_<int>(r, kRResetId)) dirty = 0x3F;
    if (char* tex = At_<char*>(f, kFTexture)) {
        const bool kept = At_<void*>(tex, kTTexture) && At_<void*>(tex, kTOwner) == At_<void*>(f, kFOwner);
        if (kept && (omni ? (At_<uint8_t>(f, kFSideMask) & dirty) == 0 : dirty == 0)) return 0;
        if (!kept) dirty = 0x3F;
        if (!*g.t.shadowGen) return 0;  // r_ShadowGen 0 freezes existing maps
        if (omni && g.omni) {  // a texture of another format, type or size is created anew, with all sides dirty
            const bool deferred = Deferred(f);
            const int size = (std::max)(0x20, At_<int>(f, kFSize));
            const int format = !At_<uint8_t>(f, kFFlag82) && !At_<uint8_t>(f, kFFlag83)
                                   ? (*g.t.shadowTexFormat == 4 ? 0x20 : 0x1F)
                                   : 0x13;
            if (At_<int>(tex, kTFormat) != format || At_<int>(tex, kTType) != (deferred ? 1 : 3) ||
                At_<int>(tex, kTWidth) != (deferred ? 3 * size : size) ||
                At_<int>(tex, kTHeight) != (deferred ? 2 * size : size))
                dirty = 0x3F;
        }
    }
    return omni ? uint8_t(At_<uint8_t>(f, kFSideMask) & dirty & 0x3F) : uint8_t(1);
}

// The class an object's MSVC RTTI names (x64 locators: image-relative), e.g. ".?AVCVegetation@@"; "?" if it has none.
const char* RttiName(const void* object) {
    __try {
        const auto* col = static_cast<const uint32_t*>((*static_cast<void* const* const*>(object))[-1]);
        HMODULE m = nullptr;
        if (col[0] > 1 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                              reinterpret_cast<LPCWSTR>(col), &m))
            return "?";
        return reinterpret_cast<const char*>(m) + col[3] + 16;  // TypeDescriptor: vtable, spare, name
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
}

void ViewPosition(float* out) {
    void* system = *g.t.system;
    const float* m = reinterpret_cast<GetViewCameraFn>((*reinterpret_cast<void***>(system))[kVfGetViewCamera / 8])(system);
    out[0] = m[3];  // Matrix34 translation
    out[1] = m[7];
    out[2] = m[11];
}

// verify mode: what a caster's Render call gets now
void RecordInputs(char* r, sp::CallInputs& in, void* caster, const void* params, const float* view) {
    void** vt = *reinterpret_cast<void***>(r);
    in.caster = caster;
    std::memcpy(in.params, params, sizeof in.params);
    std::memcpy(in.params + kPRenderNode, &caster, sizeof caster);  // the loop passes the spy: compare the real caster
    in.persFlags = At_<uint32_t>(r, kRPersFlags);
    in.persFlags2 = At_<uint32_t>(r, kRPersFlags2);
    in.level = *g.t.level;
    in.frameRec = reinterpret_cast<GetFrameIdFn>(vt[kVfGetFrameId / 8])(r, true);
    in.frame = reinterpret_cast<GetFrameIdFn>(vt[kVfGetFrameId / 8])(r, false);
    const float* c = reinterpret_cast<GetCameraFn>(vt[kVfGetCamera / 8])(r);
    in.camera[0] = c[3];
    in.camera[1] = c[7];
    in.camera[2] = c[11];
    std::memcpy(in.view, view, sizeof in.view);
}

// --- point-light sides -----------------------------------------------------------------------------------------------
// The rotation of side `side`'s camera as PrepareDepthMap computes it (kPdmOmni): a look-at basis from the side's
// direction and up vector, rolled about the direction. The same single-precision operations in the same order (MSVC's
// /fp:precise keeps them, the products with zero included), and the CRT's sinf/cosf through the game's own imports, so
// the result is bit-identical: verify mode compares the finished cameras. m: Matrix33 rows (columns right, forward, up).
void SideRotation(int side, float* m) {
    static const float kTable[kSides][7] = {  // direction, up, roll in degrees
        {1, 0, 0, 0, 0, 1, -90}, {-1, 0, 0, 0, 0, 1, 90}, {0, 1, 0, 0, 0, -1, 0},
        {0, -1, 0, 0, 0, 1, 0},  {0, 0, 1, 0, 1, 0, 0},   {0, 0, -1, 0, 1, 0, 0}};
    const float* t = kTable[side];
    const float dx = t[0], dy = t[1], dz = t[2];
    const float roll = t[6] * 0.017453292f;
    const float kMin = 1.17549435e-38f;  // FLT_MIN, added under each square root
    if (!(std::fabs(dx) > 0.f) && !(std::fabs(dy) > 0.f) && !(std::fabs(dz) > 0.f)) {
        static const float kIdentity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        std::memcpy(m, kIdentity, sizeof kIdentity);
        return;
    }
    const float inv = 1.0f / std::sqrt(((dz * dz + dy * dy) + dx * dx) + kMin);
    const float nx = inv * dx, ny = inv * dy, nz = inv * dz;  // forward
    float ux = t[3], uy = t[4], uz = t[5];
    if (nx == 0.f && ny == 0.f && !(std::fabs(ux) > 0.05f) && !(std::fabs(uy) > 0.05f) &&
        !(std::fabs(uz - 1.0f) > 0.05f)) {  // looking along z with z up: x becomes up
        ux = -0.0f - nz;
        uy = 0.f;
        uz = 0.f;
    }
    const float a = uy * nz - uz * ny, b = uz * nx - nz * ux, c = ny * ux - uy * nx;
    const float inv2 = 1.0f / std::sqrt(((c * c + b * b) + a * a) + kMin);
    const float rx = inv2 * a, ry = inv2 * b, rz = inv2 * c;  // right
    const float va = ry * nz - rz * ny, vb = rz * nx - nz * rx, vc = ny * rx - ry * nx;
    const float inv3 = 1.0f / std::sqrt(((vc * vc + vb * vb) + va * va) + kMin);
    const float vx = va * inv3, vy = vb * inv3, vz = vc * inv3;  // up
    if (roll == 0.f) {
        const float r[9] = {rx, nx, vx, ry, ny, vy, rz, nz, vz};
        std::memcpy(m, r, sizeof r);
        return;
    }
    const float sn = reinterpret_cast<FloatFn>(g.t.sinf)(roll);
    const float cs = reinterpret_cast<FloatFn>(g.t.cosf)(roll);
    const float nsn = -0.0f - sn;
    // times a rotation about y, zero terms included (the game doesn't fold them away either)
    m[0] = (nsn * vx + cs * rx) + nx * 0.f;
    m[1] = (vx + rx) * 0.f + nx;
    m[2] = (vx * cs + sn * rx) + nx * 0.f;
    m[3] = (ry * cs + nsn * vy) + ny * 0.f;
    m[4] = (vy + ry) * 0.f + ny;
    m[5] = (sn * ry + vy * cs) + ny * 0.f;
    m[6] = (rz * cs + nsn * vz) + nz * 0.f;
    m[7] = (vz + rz) * 0.f + nz;
    m[8] = (sn * rz + vz * cs) + nz * 0.f;
}

// Builds side `side`'s CCamera at `camera` (kCameraSize bytes) as PrepareDepthMap's side loop does.
void SideCamera(char* f, int side, char* camera) {
    reinterpret_cast<CamInitFn>(g.t.camInit)(camera);
    float rot[9];
    SideRotation(side, rot);
    const float m34[12] = {rot[0], rot[1], rot[2], At_<float>(f, kFProjTranslation) + At_<float>(f, kFLightPos),
                           rot[3], rot[4], rot[5], At_<float>(f, kFLightPos + 4) + At_<float>(f, kFProjTranslation + 4),
                           rot[6], rot[7], rot[8], At_<float>(f, kFLightPos + 8) + At_<float>(f, kFProjTranslation + 8)};
    std::memcpy(camera, m34, sizeof m34);
    reinterpret_cast<CamUpdateFn>(g.t.camUpdate)(camera);
    const int size = (std::max)(0x20, At_<int>(f, kFSize));  // what PrepareDepthMap passes (from +0xF4, set to this)
    const float fov = Deferred(f) ? float(double(*g.t.omniFov) * 0.017453292519943295) : 1.57079637f;
    reinterpret_cast<CamSetFrustumFn>(g.t.camSetFrustum)(camera, size, size, fov, At_<float>(f, kFNear),
                                                         At_<float>(f, kFFar));
}

// Distance from the view camera to a caster's box, as the caster loop computes it (same operations, same order).
float Distance(const float* cam, const float* box) {
    float d = 0.f;
    for (int k = 0; k < 3; ++k) {
        if (!(cam[k] >= box[k])) {
            const float t = cam[k] - box[k];
            d += t * t;
        } else if (cam[k] > box[k + 3]) {
            const float t = box[k + 3] - cam[k];
            d += t * t;
        }
    }
    return std::sqrt(d);
}

// Renders one map, or one side of a point light's, as PrepareDepthMap's caster loop does, then moves the nested level's
// items into s. firstSide: no other side of the map is rendered before this one.
void PreRender(char* r, char* f, sp::Stash& s, int flags, bool fromFlush, bool drift, bool firstSide) {
    const uint32_t save4 = At_<uint32_t>(r, kRPersFlags), save8 = At_<uint32_t>(r, kRPersFlags2);
    uint32_t p4 = save4, p8 = save8;
    if (fromFlush) {  // what the list processing sets before the shadow pass (kHdr): HDR scene, nFlags bit 7, AO
        const bool hdr = (flags & 1) && *g.t.hdrRendering && !*g.t.measureOverdraw &&
                         (*g.t.hdrAllowNonFP || At_<int>(r, kRHdrFormat) <= 1) && !(p4 & 0x1000000);
        p8 = (p8 & ~0x200u) | (hdr ? 0x200u : 0u);
        if (flags & 0x80) p8 |= 0x10000;
        p4 = (p4 & ~0x20000000u) | ((flags & 0x100) ? 0x20000000u : 0u);
    }
    if (drift) p4 |= 0x600000;  // what each PrepareDepthMap that rendered leaves behind
    // PrepareDepthMap, before its caster loop (kPdmFormat, kPdmFlags, kPdmClear; SetViewport adds 0x200000)
    p4 |= 0x400000;
    p4 = (p4 & ~0x4000000u) | 0x100000u;
    const bool f82 = At_<uint8_t>(f, kFFlag82) != 0, f83 = At_<uint8_t>(f, kFFlag83) != 0;
    const uint32_t fflags = At_<uint32_t>(f, kFFlags);
    if (f82) p8 |= 0x100;
    if (!(fflags & 2)) p8 |= 0x800000;
    p8 |= 4;  // all of the formats it picks (0x13, 0x1F, 0x20) are in its first set
    if (f82 || f83) p8 |= 0x40000;
    p8 |= 0x200000;
    if (!f82 && !f83) p8 |= 0x4000000;
    p8 &= ~0x202000u;  // right before its caster loop it applies viewport and state (FUN_38024d50): dirty bits cleared
    alignas(16) char camera[kCameraSize + 0x58];  // (room to spare)
    if (s.side >= 0) {
        // each side sets the render camera (0x600000) and 0x40000. The sides after the first see what the previous
        // side's nested flush left: its list processing ran with nFlags 0, which clears bit 29 (m_PersFlags2 it restores)
        p4 |= 0x640000;
        if (!firstSide) p4 &= ~0x20000000u;
        SideCamera(f, s.side, camera);
        if (g.mode == Mode::Verify) s.camera.assign(camera, camera + kCameraSize);
    }
    At_<uint32_t>(r, kRPersFlags) = p4;
    At_<uint32_t>(r, kRPersFlags2) = p8;

    float cam[3];
    ViewPosition(cam);
    const int polygonsBefore = PolygonCount(*g.t.level + 1);
    reinterpret_cast<RendererFn>((*reinterpret_cast<void***>(r))[kVfStartEf / 8])(r);
    const int level = *g.t.level;
    const uint32_t* start = g.t.starts + (level - 1) * sp::kLists;
    void* const owner = At_<void*>(f, kFOwner);
    alignas(16) uint8_t params[kParamsSize];
    for (int i = 0; i < At_<PodArrayPtr*>(f, kFCasters)->count; ++i) {
        void* c = At_<PodArrayPtr*>(f, kFCasters)->elems[i];
        void** vt = *reinterpret_cast<void***>(c);
        alignas(16) float box[8];
        reinterpret_cast<GetBBoxFn>(vt[2])(c, box);
        if (s.side >= 0 && !reinterpret_cast<CullBoxFn>(g.t.cullBox)(camera, box)) continue;
        if (int32_t(fflags) < 0 && reinterpret_cast<HasOcclusionmapFn>(vt[0])(c, 0, owner)) continue;
        std::memset(params, 0, sizeof params);
        char* p = reinterpret_cast<char*>(params);
        At_<int>(p, kPRenderList) = 2;  // EFSLIST_GENERAL
        At_<float>(p, kPAlpha) = 1.f;
        At_<float>(p, kPRenderQuality) = 1.f;
        At_<uint8_t>(p, kPAfterWater) = 1;
        At_<int>(p, kPTechnique) = 4;  // TTYPE_SHADOWGEN
        At_<uint32_t>(p, kPObjFlags) = 0xF | (At_<uint32_t>(static_cast<char*>(c), kCasterRndFlags) & 0x200);
        At_<float>(p, kPDistance) = Distance(cam, box);
        At_<void*>(p, kPRenderNode) = c;
        if (g.mode == Mode::Verify) RecordInputs(r, s.calls.emplace_back(), c, params, cam);
        reinterpret_cast<RenderFn>(vt[1])(c, params);
        ++s.casters;
        if (g.mode == Mode::Compare) {  // for the mismatch log: which caster added which items
            s.casterPtrs.push_back(c);
            for (int k = 0; k < sp::kLists; ++k) s.ends.push_back(Lists()[k].count - start[k]);
        }
    }
    sp::TakeLevel(Lists(), start, s);
    *g.t.level = level - 1;  // closes the level; its render objects are not recycled (see the top of the file)
    At_<uint32_t>(r, kRPersFlags) = save4;
    At_<uint32_t>(r, kRPersFlags2) = save8;
    if (PolygonCount(level) != polygonsBefore) ++g.st.polygons;
}

void EndOfFlush() {
    for (int i = 0; i < g.used; ++i)
        if (g.stashes[i].state == kPending) ++g.st.unused;
    g.used = 0;
    g.replay = Replay{};
}

sp::Stash& NewStash(char* f, int side, PodArrayPtr* casters) {
    sp::Stash& s = g.stashes[g.used++];
    s.Reset(f, side);
    s.casterList = casters;
    s.casterCount = casters->count;
    s.casterHash = CasterHash(casters);
    return s;
}

void PrePass(char* r, int flags, bool fromFlush) {
    EndOfFlush();
    if (*g.t.useShadowsPool || !*g.t.forwardPass) return;  // shadow maps are prepared elsewhere (or not at all)
    if (fromFlush && ((flags & 8) || At_<uint8_t>(r, kRDeviceLost) || *g.t.noDrawShaders)) return;  // no shadow pass
    if (reinterpret_cast<IsMultiGpuFn>((*reinterpret_cast<void***>(r))[kVfIsMultiGpu / 8])(r)) {
        ++g.st.multiGpu;  // maps alternate between GPUs: left to the original code
        return;
    }
    const uint64_t t0 = __rdtsc();
    ++g.st.prepasses;
    const int level = *g.t.level;
    char* lights = r + kRLights + level * 16;
    const uint32_t nLights = At_<uint32_t>(lights, 8);
    int lightsCasting = 0, frustumsSeen = 0;
    bool drift = false;
    const int perCaster = g.mode == Mode::Compare ? 8 : 4;  // compare renders the casters twice
    for (uint32_t i = 0; i < nLights; ++i) {
        char* light = At_<char**>(lights, 0)[i];
        const uint32_t lflags = At_<uint32_t>(light, kLFlags);
        if (!(lflags & 0x10)) continue;
        ++lightsCasting;
        if (*g.t.shadowGenGS && (lflags & 2)) {
            ++g.st.gsLights;
            drift = true;
            continue;
        }
        char** frustums = At_<char**>(light, kLFrustums);
        if (!frustums) continue;
        for (int j = 0; j < 8 && frustums[j]; ++j) {
            char* f = frustums[j];
            ++g.st.frustums;
            ++frustumsSeen;
            const uint8_t mask = SideMask(r, f);
            if (!mask) {
                ++g.st.upToDate;
                continue;
            }
            const bool omni = At_<uint8_t>(f, kFOmni) != 0;
            auto* casters = At_<PodArrayPtr*>(f, kFCasters);
            const int sides = omni ? SideCount(mask) : 1;
            if (omni && !g.omni) {
                ++g.st.omniLeft;
            } else if (At_<int>(r, kRObjects) + perCaster * casters->count * sides + 256 >= kRMaxObjects) {
                ++g.st.noRoom;
            } else if (g.used + sides > kMaxStashes) {
                ++g.st.overflow;
            } else {
                bool first = true;
                for (int side = 0; side < (omni ? kSides : 1); ++side) {
                    if (omni && !(mask & (1 << side))) continue;
                    sp::Stash& s = NewStash(f, omni ? side : -1, casters);
                    PreRender(r, f, s, flags, fromFlush, drift, first);
                    first = false;
                    ++g.st.sides;
                    g.st.casters += uint64_t(s.casters);
                    g.st.items += s.items.size();
                }
                ++g.st.rendered;
                if (omni) ++g.st.omniRendered;
            }
            drift = true;  // left by this map's PrepareDepthMap, for the maps after it
        }
    }
    if (At_<int>(r, kRObjects) > g.st.maxObjects) g.st.maxObjects = At_<int>(r, kRObjects);
    g.st.prepassCycles += __rdtsc() - t0;
    if (!g.loggedFirst && g.used) {
        g.loggedFirst = true;
        Log("shadow prepass: first pre-pass: %u lights (%d casting shadows), %d maps, %d sides rendered early", nLights,
            lightsCasting, frustumsSeen, g.used);
    }
}

// the placeholder caster that replays a stash, once per side
bool PhHasOcclusionmap(void*, int, void*) { return false; }
void* PhGetBBox(void*, float* box) {  // everything, so that no side's camera culls it
    for (int k = 0; k < 3; ++k) {
        box[k] = -1e9f;
        box[k + 3] = 1e9f;
    }
    return box;
}
void* PhGetCharacter(void*, unsigned, void*, bool) { return nullptr; }
bool PhRender(void*, const void*) {
    sp::Stash* s = g.replay.Current();
    ++g.replay.renders;
    if (!s || s->state != kPending) return false;
    const uint64_t t0 = __rdtsc();
    if (!sp::PutBack(Lists(), *s, reinterpret_cast<sp::ReallocFn>(g.t.realloc))) return false;  // (tried again at the end)
    s->state = kReplayed;
    g.st.replayCycles += __rdtsc() - t0;
    return true;
}
struct Placeholder {
    void* const* vtbl;
    uint32_t pad;
    uint32_t rndFlags;  // +0xC, tested by the caster loop
    uint8_t rest[64];
};
void* const kPlaceholderVtbl[] = {reinterpret_cast<void*>(&PhHasOcclusionmap), reinterpret_cast<void*>(&PhRender),
                                  reinterpret_cast<void*>(&PhGetBBox), reinterpret_cast<void*>(&PhGetCharacter)};
Placeholder g_placeholder{kPlaceholderVtbl, 0, 0, {}};
void* g_placeholderElems[1] = {&g_placeholder};

// Elements pooled per call (CREParticle) are equivalent if they have the same type and the same source and settings.
bool SameElement(const char* a, const char* b) {
    return *reinterpret_cast<void* const*>(a) == *reinterpret_cast<void* const*>(b) &&
           std::memcmp(a + 0x30, b + 0x30, 0x20) == 0 &&
           ((*reinterpret_cast<const uint32_t*>(a + 0xC) ^ *reinterpret_cast<const uint32_t*>(b + 0xC)) & 0x800) == 0;
}

// --- verify: spies -----------------------------------------------------------------------------------------------------
// appends to a log line, truncating instead of failing when it is full
void Append(char* buf, size_t size, size_t& n, const char* fmt, ...) {
    if (n + 1 >= size) return;
    va_list args;
    va_start(args, fmt);
    const int w = vsnprintf(buf + n, size - n, fmt, args);
    va_end(args);
    if (w > 0) n = (std::min)(size - 1, n + size_t(w));
}

// A point-light side's camera, the pre-pass's against the one PrepareDepthMap's loop culls the casters with. The spies'
// GetBBox is called by that loop, whose frame (rsp) starts right above the return address: it passes the box at
// rsp+0x68 and keeps the camera at rsp+0x4A0 (kPdmLoop). Compared: the matrix and what the cull test reads.
void CheckSideCamera(char* frame, const float* box) {
    Replay& rp = g.replay;
    rp.cameraChecked = true;
    const sp::Stash* s = rp.Current();
    if (!s || s->side < 0) return;
    if (reinterpret_cast<const char*>(box) != frame + kLoopBoxAt || s->camera.size() != kCameraSize) {
        ++g.st.vSideCamerasMissed;
        return;
    }
    ++g.st.vSideCameras;
    const uint8_t* a = s->camera.data();
    const uint8_t* b = reinterpret_cast<const uint8_t*>(frame + kLoopCameraAt);
    size_t at = SIZE_MAX;
    for (size_t o = 0; o < kCameraPlanesEnd && at == SIZE_MAX; o += 4) {
        if (o == 0x30) o = kCameraPlanes;
        if (std::memcmp(a + o, b + o, 4) != 0) at = o;
    }
    if (at == SIZE_MAX) return;
    ++g.st.vSideCamerasDiffer;
    g.spyDiffers = true;
    if (++g.st.vLogged > 40) return;
    uint32_t va, vb;
    std::memcpy(&va, a + at, 4);
    std::memcpy(&vb, b + at, 4);
    float fa, fb;
    std::memcpy(&fa, &va, 4);
    std::memcpy(&fb, &vb, 4);
    Log("shadow prepass: VERIFY difference #%llu, frustum %p side %d: camera +0x%zX: pre-pass %.9g (0x%08X), loop %.9g "
        "(0x%08X)",
        g.st.vLogged, s->frustum, s->side, at, double(fa), va, double(fb), vb);
}

bool SpyHasOcclusionmap(Spy* s, int lod, void* owner) {
    return reinterpret_cast<HasOcclusionmapFn>((*reinterpret_cast<void***>(s->real))[0])(s->real, lod, owner);
}
void* SpyGetBBox(Spy* s, float* box) {
    if (!g.replay.cameraChecked) CheckSideCamera(static_cast<char*>(_AddressOfReturnAddress()) + 8, box);
    return reinterpret_cast<GetBBoxFn>((*reinterpret_cast<void***>(s->real))[2])(s->real, box);
}
void* SpyGetCharacter(Spy* s, unsigned slot, void* matrix, bool onlyVisible) {
    return reinterpret_cast<GetCharacterFn>((*reinterpret_cast<void***>(s->real))[3])(s->real, slot, matrix, onlyVisible);
}

void CompareInputs(const sp::Stash& s, size_t call, const sp::CallInputs& a, const sp::CallInputs& b) {
    Stats& st = g.st;
    int first = -1;  // first differing SRendParams byte
    for (int i = 0; i < kParamsSize && first < 0; ++i)
        if (a.params[i] != b.params[i]) first = i;
    const bool order = a.caster != b.caster;
    const bool f4 = a.persFlags != b.persFlags, f8 = a.persFlags2 != b.persFlags2;
    const bool level = a.level != b.level, rec = a.frameRec != b.frameRec, frame = a.frame != b.frame;
    const bool camera = std::memcmp(a.camera, b.camera, sizeof a.camera) != 0;
    const bool view = std::memcmp(a.view, b.view, sizeof a.view) != 0;
    if (order) {
        ++st.vOrder;
    } else {
        st.vParams += first >= 0;
        st.vFlags += f4;
        st.vFlags2 += f8;
        st.vFlagsMask |= a.persFlags ^ b.persFlags;
        st.vFlags2Mask |= a.persFlags2 ^ b.persFlags2;
        st.vLevel += level;
        st.vFrameRec += rec;
        st.vFrame += frame;
        st.vCamera += camera;
        st.vView += view;
    }
    if (!(order || first >= 0 || f4 || f8 || level || rec || frame || camera || view)) return;
    g.spyDiffers = true;
    if (++st.vLogged > 40) return;
    char what[768] = "";
    size_t n = 0;
    if (order)
        Append(what, sizeof what, n, " caster: pre-pass %s %p, loop %s %p;", RttiName(a.caster), a.caster,
               RttiName(b.caster), b.caster);
    if (!order && first >= 0) {
        uint32_t va = 0, vb = 0;
        std::memcpy(&va, a.params + (first & ~3), 4);
        std::memcpy(&vb, b.params + (first & ~3), 4);
        Append(what, sizeof what, n, " SRendParams +0x%X: 0x%08X / 0x%08X;", first & ~3, va, vb);
    }
    if (f4) Append(what, sizeof what, n, " m_PersFlags 0x%08X / 0x%08X;", a.persFlags, b.persFlags);
    if (f8) Append(what, sizeof what, n, " m_PersFlags2 0x%08X / 0x%08X;", a.persFlags2, b.persFlags2);
    if (level) Append(what, sizeof what, n, " level %d / %d;", a.level, b.level);
    if (rec) Append(what, sizeof what, n, " frame id with recursion %d / %d;", a.frameRec, b.frameRec);
    if (frame) Append(what, sizeof what, n, " frame id %d / %d;", a.frame, b.frame);
    if (camera)
        Append(what, sizeof what, n, " camera (%.3f %.3f %.3f) / (%.3f %.3f %.3f);", a.camera[0], a.camera[1], a.camera[2],
               b.camera[0], b.camera[1], b.camera[2]);
    if (view)
        Append(what, sizeof what, n, " view (%.3f %.3f %.3f) / (%.3f %.3f %.3f);", a.view[0], a.view[1], a.view[2],
               b.view[0], b.view[1], b.view[2]);
    Log("shadow prepass: VERIFY difference #%llu, frustum %p side %d call %zu of %zu (%s), pre-pass / loop:%s", st.vLogged,
        s.frustum, s.side, call, s.calls.size(), RttiName(b.caster), what);
}

bool SpyRender(Spy* spy, const void* params) {
    sp::Stash* s = g.replay.Current();
    if (!s) return false;
    float view[3];
    ViewPosition(view);
    sp::CallInputs now;
    RecordInputs(g.renderer, now, spy->real, params, view);
    const size_t call = size_t(g.replay.renders++);
    if (call < s->calls.size()) CompareInputs(*s, call, s->calls[call], now);
    ++g.st.vCalls;
    if (s->state == kPending) {  // the whole stash at the side's first call
        const uint64_t t0 = __rdtsc();
        if (!sp::PutBack(Lists(), *s, reinterpret_cast<sp::ReallocFn>(g.t.realloc))) return false;
        s->state = kReplayed;
        g.st.replayCycles += __rdtsc() - t0;
    }
    return true;
}
void* const kSpyVtbl[] = {reinterpret_cast<void*>(&SpyHasOcclusionmap), reinterpret_cast<void*>(&SpyRender),
                          reinterpret_cast<void*>(&SpyGetBBox), reinterpret_cast<void*>(&SpyGetCharacter)};

// --- compare -------------------------------------------------------------------------------------------------------------
void CompareOutputs(sp::Stash& s) {
    const int level = *g.t.level;
    sp::Difference d;
    const bool same = sp::CompareLevel(Lists(), g.t.starts + (level - 1) * sp::kLists, s, &SameElement, &d,
                                       &g.st.elemsByContents, &g.st.nearMatrices);
    s.state = kCompared;
    ++g.st.compared;
    if (same) return;
    if (++g.st.mismatched <= 40) {
        const void* caster = d.index >= 0 ? s.CasterOf(d.list, d.index) : nullptr;
        Log("shadow prepass: COMPARE mismatch #%llu, frustum %p side %d (%d casters, %zu items): list %d/%d item %d (from "
            "%s %p), %s%s0x%X: original 0x%llX, pre-pass 0x%llX",
            g.st.mismatched, s.frustum, s.side, s.casters, s.items.size(), d.list / 15, d.list % 15, d.index,
            caster ? RttiName(caster) : "-", caster, sp::FieldName(d.field), d.field == sp::Field::Object ? " +" : " ",
            d.field == sp::Field::Object ? d.objOffset : 0u, d.a, d.b);
    }
}

// --- replay ----------------------------------------------------------------------------------------------------------------
// At the nested EF_EndEf3D of the side PrepareDepthMap's loop has just rendered, before that side's flush.
void EndOfSide() {
    Replay& rp = g.replay;
    sp::Stash* s = rp.Current();
    if (!s) return;
    if (g.mode == Mode::Compare) {
        CompareOutputs(*s);
    } else {
        if (s->state == kPending) {  // no Render call put it back (the loop culled everything): it goes back here
            if (sp::PutBack(Lists(), *s, reinterpret_cast<sp::ReallocFn>(g.t.realloc))) {
                s->state = kReplayed;
                ++g.st.backAtEnd;
            } else {
                s->state = kStale;
                ++g.st.putBackFailed;
            }
        }
        if (s->state == kReplayed) {
            ++g.st.replayed;
            // the loop counts the casters it rendered in the shadow texture: the placeholder or the spies, not the stash's
            if (char* tex = At_<char*>(s->frustum, kFTexture)) At_<int>(tex, kTCasters) += s->casters - rp.renders;
        }
        if (g.mode == Mode::Verify) {
            ++g.st.vSides;
            if (size_t(rp.renders) != s->calls.size()) {
                ++g.st.vCount;
                g.spyDiffers = true;
                if (++g.st.vLogged <= 40)
                    Log("shadow prepass: VERIFY difference #%llu, frustum %p side %d: the loop rendered %d casters, the "
                        "pre-pass %zu",
                        g.st.vLogged, s->frustum, s->side, rp.renders, s->calls.size());
            }
            if (!g.spyDiffers) ++g.st.vSidesSame;
            g.spyDiffers = false;
        }
    }
    rp.renders = 0;
    rp.cameraChecked = false;
    ++rp.cursor;
}

// --- hooks -------------------------------------------------------------------------------------------------------------
void EndEf3DHook(char* r, int flags) {
    if (!g.mainThread) g.mainThread = GetCurrentThreadId();
    if (!OnPartThread()) return g.endEf3D(r, flags);
    TraversalRecord_OnEndEf3D();  // (a traversal answered, FlushThreadRecord=on|beside: made now, its level then real)
    const int level = *g.t.level;
    if (level == 2 && ReflectionPrepass_WillTake(level)) ParticlePrepass_OnNestedLevel(r, level);  // (its particles)
    if (level == 2 && ReflectionPrepass_OnNestedEndEf3D(r, flags)) return;  // a reflection rendered early: captured
    if (level == 2 && g.replay.active) EndOfSide();
    const bool outer = level == 1 && GetCurrentThreadId() == g.mainThread;
    uint64_t t0 = 0, t1 = 0;
    if (outer) {
        FlushThread_OnEntry();           // (the flush's thread, known to the hand-off below)
        TraversalRecord_OnFlushEntry(r);  // (verify: the traversal's producer calls replayed and compared)
        Allocator_BeginPins();           // (FlushThread=overlap: what the parts will write after the flush, kept)
        FramePhases_FlushEntry();        // (a diagnostic: optiperf's work before the flush, then the flush, timed)
        ProducerTiming_OnFlushEntry();   // (a diagnostic: the producer calls made at the flush's entry, apart)
        t0 = __rdtsc();
        if (g.st.lastEntry) {
            g.st.frameCycles += t0 - g.st.lastEntry;
            ++g.st.frames;
        }
        g.st.lastEntry = t0;
        ++g.st.flushes;
        ParticlePrepass_OnFlushEntry(r);
        ReflectionPrepass_OnFlushEntry(r, flags);  // (the reflections come before the shadow maps in the flush too)
        if (g.at == At::Flush) PrePass(r, flags, true);
        ReflectionPrepass_BeforeFlush(r);
        ParticlePrepass_BeforeFlush(r);  // (the particles' vertices, for the main pass)
        SkinPrepass_BeforeFlush(r);      // (the skinned objects' poses: all of the frame's render objects by now)
        MaterialSnapshot_BeforeFlush(r); // (the materials of the objects with layers, from the same items)
        EnvSnapshot_BeforeFlush(r);      // (what the flush reads of the 3D engine's environment from here on)
        CvarSnapshot_BeforeFlush();      // (the other modules' cvars the renderer reads)
        SysSnapshot_BeforeFlush();       // (the system's and the timer's getters, which the captures above call)
        LightSnapshot_BeforeFlush(r);    // (the 3D engine's lights, copied once nothing adds any before the flush)
        EngineData_BeforeFlush(r);       // (the sky, the ocean, the water volumes, the custom data, the far trees' sets)
        RenderQueue_BeforeFlush(r);      // (the camera ProjectToScreen reads beside the flush, with FlushThread=overlap)
        Census_BeforeFlush(r);           // (a diagnostic, last: reads what the parts above have put in place)
        FramePhases_FlushStart();
        ProducerTiming_OnFlushStart();
        t1 = __rdtsc();
        g.st.entryCycles += t1 - t0;
        // on the flush's thread: the main thread, or a worker while this one waits, or beside it until its first
        // renderer call (FlushThread); the call tracer, if on, counts what the flush itself calls apart. The parts'
        // work after it comes when it has ended (AfterOuterFlush)
        g_outer = OuterFlush{r, flags, t1};
        FlushThread_Run(&RunOuterFlush, &g_outer, &AfterOuterFlush, &g_outer);
    } else {
        g.endEf3D(r, flags);
    }
}

void PrepareAllHook(char* r) {
    if (g.at == At::Shadows && g.mainThread && OnFlush()) PrePass(r, 0, false);
    // (the main pass's shadow maps, after the reflections: the flush's scene passes, which the recursive frame id
    // counts, are done; FlushThread=overlap's GetFrameID(true) beside the flush waits for this)
    if (*g.t.level == 1 && OnFlushThread()) RenderQueue_OnScenePassesDone();
    g.prepareAll(r);
}

void PrepareDepthMapHook(char* r, char* f, char* light) {
    if (!f || !g.mainThread || !OnFlush() || !g.used || g.replay.active) return g.prepareDepthMap(r, f, light);
    ++g.st.pdmCalls;
    // the frustum's stashes: one run of them, in side order
    Replay rp;
    int i = 0;
    while (i < g.used && !(g.stashes[i].frustum == f && g.stashes[i].state == kPending)) ++i;
    for (; i < g.used && rp.count < kSides; ++i) {
        sp::Stash& s = g.stashes[i];
        if (s.frustum != f || s.state != kPending || (rp.count && s.side <= rp.sides[rp.count - 1]->side)) break;
        rp.sides[rp.count++] = &s;
    }
    if (!rp.count) {
        ++g.st.withoutStash;
        return g.prepareDepthMap(r, f, light);
    }
    auto* casters = At_<PodArrayPtr*>(f, kFCasters);
    const sp::Stash& first = *rp.sides[0];
    bool fallBack = false;
    if (casters != first.casterList || !casters || casters->count != first.casterCount ||
        CasterHash(casters) != first.casterHash) {
        ++g.st.stale;
        fallBack = true;
    } else {  // the sides it renders now must be those the pre-pass rendered
        uint8_t stashed = 0;
        for (int k = 0; k < rp.count; ++k) stashed = uint8_t(stashed | (1 << (rp.sides[k]->side < 0 ? 0 : rp.sides[k]->side)));
        const uint8_t now = SideMask(r, f);
        if (now != stashed) {
            if (++g.st.otherSides <= 10)
                Log("shadow prepass: frustum %p: PrepareDepthMap renders sides 0x%02X, the pre-pass rendered 0x%02X", f,
                    now, stashed);
            fallBack = true;
        }
    }
    if (fallBack) {  // the casters render as usual
        for (int k = 0; k < rp.count; ++k) rp.sides[k]->state = kStale;
        return g.prepareDepthMap(r, f, light);
    }
    rp.active = true;
    g.replay = rp;
    PodArrayPtr placeholder{g_placeholderElems, 1, 1};
    PodArrayPtr spies{};
    if (g.mode == Mode::Verify) {  // spies stand in for the casters: the loop's own calls are compared with the pre-pass's
        const int n = casters->count;
        g.spies.resize(size_t(n));
        g.spyElems.resize(size_t(n));
        for (int k = 0; k < n; ++k) {
            void* real = casters->elems[k];
            g.spies[size_t(k)] = Spy{kSpyVtbl, 0, At_<uint32_t>(static_cast<char*>(real), kCasterRndFlags), real};
            g.spyElems[size_t(k)] = &g.spies[size_t(k)];
        }
        spies = PodArrayPtr{g.spyElems.data(), n, n};
        At_<PodArrayPtr*>(f, kFCasters) = &spies;
        g.renderer = r;
        g.spyDiffers = false;
    } else if (g.mode == Mode::On) {
        At_<PodArrayPtr*>(f, kFCasters) = &placeholder;
    }  // compare: the original casters render, and each side's items are compared at its nested EF_EndEf3D
    g.prepareDepthMap(r, f, light);
    At_<PodArrayPtr*>(f, kFCasters) = casters;
    for (int k = g.replay.cursor; k < g.replay.count; ++k) {  // sides its loop didn't come to (it returned early)
        ++g.st.notReached;
        g.replay.sides[k]->state = kStale;
    }
    g.replay = Replay{};
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

// whether the 4-byte constant a RIP-relative operand at `field` refers to has these bits
bool ConstIs(char* base, char* field, uint32_t bits) {
    const char* p = Rip(field, 0);
    return IsInImage(base, p, 4) && ReadU32(p) == bits;
}

// Point-light maps: PrepareDepthMap's side loop and the code that builds the side cameras. Null if found and as
// expected, else why not (those maps then keep the original path).
const char* LocateOmni(char* base, char* pdm, char* format, char* flags, char* clear, char* loop, ShadowPrepassTarget* t) {
    const char* why = nullptr;
    char* deferred = UniqueIn(pdm, kPdmSize, kPdmDeferred, &why, "the deferred-mode test not found");
    char* head = deferred ? UniqueIn(pdm, kPdmSize, kPdmLoopHead, &why, "the side loop not found") : nullptr;
    char* cam = head ? UniqueIn(pdm, kPdmSize, kPdmCamInit, &why, "the side camera not found") : nullptr;
    char* omni = cam ? UniqueIn(pdm, kPdmSize, kPdmOmni, &why, "the side camera's setup not found") : nullptr;
    if (!omni) return why;
    if (!(deferred < format && flags < head && head < cam && cam < omni && omni < clear))
        return "the side loop's blocks are not in the expected order";
    // the constants SideRotation's transcription relies on
    bool same = ConstIs(base, head + kLoopAbsMask, 0x7FFFFFFF) && ConstIs(base, head + kLoopThreshold, 0x3D4CCCCD) &&
                ConstIs(base, head + kLoopNegZero, 0x80000000) && ConstIs(base, omni + kOmniOne, 0x3F800000) &&
                ConstIs(base, omni + kOmniMinusOne, 0xBF800000) && ConstIs(base, omni + kOmniMinus90, 0xC2B40000) &&
                ConstIs(base, omni + kOmniPlus90, 0x42B40000) && ConstIs(base, omni + kOmniDegToRad, 0x3C8EFA35) &&
                ConstIs(base, omni + kOmniNegZero, 0x80000000) && ConstIs(base, omni + kOmniHalfPi, 0x3FC90FDB);
    for (int o : kOmniFltMin) same = same && ConstIs(base, omni + o, 0x00800000);
    const char* d = Rip(omni + kOmniDegToRadD, 0);  // pi/180 as a double
    same = same && IsInImage(base, d, 8) && ReadU32(d) == 0xA2529D39u && ReadU32(d + 4) == 0x3F91DF46u;
    if (!same) return "the side rotation's constants are not as expected";
    char* const calls[] = {RelTarget(cam + kCamInitCall),        RelTarget(omni + kOmniUpdate),
                           RelTarget(omni + kOmniSetFrustum[0]), RelTarget(loop + kPdmLoopCull),
                           RelTarget(omni + kOmniSinf),          RelTarget(omni + kOmniCosf)};
    for (char* c : calls)
        if (!IsInCode(base, c, 16)) return "a call of the side cameras' code goes outside the module's code";
    if (RelTarget(omni + kOmniSetFrustum[1]) != calls[2]) return "the side cameras' two SetFrustum calls differ";
    for (char* c : {calls[4], calls[5]})  // sinf and cosf: import thunks, jmp [rip+X]
        if (uint8_t(c[0]) != 0xFF || uint8_t(c[1]) != 0x25) return "sinf or cosf is not an import thunk";
    char* fov = Rip(omni + kOmniFov, 0);
    char* cvar = Rip(deferred + kPdmDeferredCvar, 0);
    if (!IsInImage(base, fov, 4) || !IsInImage(base, cvar, 4)) return "the side cameras' globals are outside the module";
    t->camInit = calls[0];
    t->camUpdate = calls[1];
    t->camSetFrustum = calls[2];
    t->cullBox = calls[3];
    t->sinf = calls[4];
    t->cosf = calls[5];
    t->omniFov = reinterpret_cast<float*>(fov);
    t->shadowsDeferred = reinterpret_cast<int*>(cvar);
    return nullptr;
}
#endif  // _WIN64

}  // namespace

bool ShadowPrepass_Locate(char* base, ShadowPrepassTarget* out, const char** why) {
#ifndef _WIN64
    (void)base;
    (void)out;
    *why = "64-bit renderer only";
    return false;
#else
    ShadowPrepassTarget t;
    char* end = Unique(base, kEndEf3D, why, "EF_EndEf3D not found (or found more than once)");
    char* all = end ? Unique(base, kPrepareAll, why, "the shadow pass (PrepareAllDepthMaps) not found") : nullptr;
    char* gate = all ? Unique(base, kGate, why, "the call of the shadow pass not found") : nullptr;
    char* pdm = gate ? Unique(base, kPdmPrologue, why, "PrepareDepthMap not found") : nullptr;
    char* hdr = pdm ? Unique(base, kHdr, why, "the list processing's HDR setup not found") : nullptr;
    char* start = hdr ? Unique(base, kStartEfHead, why, "EF_StartEf not found") : nullptr;
    char* add = start ? Unique(base, kAddItem, why, "SRendItem::mfAdd not found") : nullptr;
    if (!add) return false;
    if (!IsInCode(base, pdm, kPdmSize + 0x400) || !IsInCode(base, start, 0x600)) {
        *why = "PrepareDepthMap or EF_StartEf too close to the end of the code";
        return false;
    }
    char* format = UniqueIn(pdm, kPdmSize, kPdmFormat, why, "PrepareDepthMap: format selection not found");
    char* flags = format ? UniqueIn(pdm, kPdmSize, kPdmFlags, why, "PrepareDepthMap: flag setup not found") : nullptr;
    char* clear = flags ? UniqueIn(pdm, kPdmSize, kPdmClear, why, "PrepareDepthMap: map clear not found") : nullptr;
    char* loop = clear ? UniqueIn(pdm, kPdmSize, kPdmLoop, why, "PrepareDepthMap: caster loop not found") : nullptr;
    char* tail = loop ? UniqueIn(start, 0x500, kStartEfTail, why, "EF_StartEf: level bookkeeping not found") : nullptr;
    char* poly = tail ? UniqueIn(start, 0x500, kStartEfPoly, why, "EF_StartEf: polygon-list reset not found") : nullptr;
    if (!poly) return false;
    if (!(format < flags && flags < clear && clear < loop)) {
        *why = "PrepareDepthMap: its blocks are not in the expected order";
        return false;
    }
    if (RelTarget(gate + kGateCall) != all) {
        *why = "the shadow pass call goes elsewhere";
        return false;
    }
    if (!(hdr < gate && gate - hdr < 0x1000)) {  // both in the list processing, the HDR setup first
        *why = "the HDR setup is not where the shadow pass call is";
        return false;
    }
    // the recursion level, wherever it is used
    t.level = reinterpret_cast<int*>(Rip(end + kEndEf3DLevel[0], 1));
    bool same = true;
    for (int o : kEndEf3DLevel) same &= Rip(end + o, 1) == reinterpret_cast<char*>(t.level);
    for (int o : kPrepareAllLevel) same &= Rip(all + o, 0) == reinterpret_cast<char*>(t.level);
    same &= Rip(start + kStartEfLevel, 0) == reinterpret_cast<char*>(t.level);
    for (int o : kStartEfTailLevel) same &= Rip(tail + o, 0) == reinterpret_cast<char*>(t.level);
    same &= Rip(tail + kStartEfTailInc, 1) == reinterpret_cast<char*>(t.level);
    same &= Rip(hdr + kHdrLevel, 1) == reinterpret_cast<char*>(t.level);
    if (!same) {
        *why = "the recursion level is not the same variable everywhere";
        return false;
    }
    // the item lists: mfAdd's base, EF_StartEf's count walk (+8, 30 lists of 16 bytes), the level starts
    t.lists = Rip(add + kAddItemLists, 0);
    t.starts = reinterpret_cast<uint32_t*>(Rip(start + kStartEfStarts, 0));
    if (Rip(start + kStartEfCounts, 0) != t.lists + 8 || Rip(start + kStartEfCountsEnd, 0) != t.lists + 8 + 16 * sp::kLists ||
        Rip(tail + kStartEfTailStarts, 0) != reinterpret_cast<char*>(t.starts)) {
        *why = "the item lists are not where EF_StartEf and SRendItem::mfAdd agree";
        return false;
    }
    t.polyLists = Rip(poly + kStartEfPolyFirst, 0) - 8;
    if (Rip(poly + kStartEfPolyEnd, 0) != t.polyLists + 8 + 4 * 16) {
        *why = "EF_StartEf: unexpected polygon lists";
        return false;
    }
    t.realloc = RelTarget(add + kAddItemRealloc);
    static const Pattern realloc = ParsePattern(kRealloc);
    if (!IsInCode(base, t.realloc, realloc.size()) || !MatchesAt(t.realloc, realloc)) {
        *why = "CryModuleRealloc does not look as expected";
        return false;
    }
    t.endEf3D = end;
    t.prepareAll = all;
    t.prepareDepthMap = pdm;
    t.system = reinterpret_cast<void**>(Rip(loop + kPdmLoopSystem, 0));
    t.useShadowsPool = reinterpret_cast<int*>(Rip(all + kPrepareAllPool, 1));
    t.shadowGenGS = reinterpret_cast<int*>(Rip(all + kPrepareAllGS, 1));
    t.forwardPass = reinterpret_cast<int*>(Rip(gate + kGateForward, 1));
    t.shadowGen = reinterpret_cast<int*>(Rip(pdm + kPdmShadowGen, 0));
    t.noDrawShaders = reinterpret_cast<int*>(Rip(end + kEndEf3DNoDraw, 1));
    t.hdrRendering = reinterpret_cast<int*>(Rip(hdr + kHdrRendering, 0));
    t.measureOverdraw = reinterpret_cast<int*>(Rip(hdr + kHdrOverdraw, 0));
    t.hdrAllowNonFP = reinterpret_cast<int*>(Rip(hdr + kHdrAllowNonFP, 0));
    t.shadowTexFormat = reinterpret_cast<int*>(Rip(format + kPdmFormatCvar, 1));
    t.omniWhy = LocateOmni(base, pdm, format, flags, clear, loop, &t);
    *out = t;
    return true;
#endif
}

void ShadowPrepass_OnModuleLoaded(const wchar_t* baseName, char* base, size_t size) {
    if (_wcsicmp(baseName, L"CryRenderD3D10.dll") != 0 || g.installed) return;
    g.mode = ReadMode();
    if (g.mode == Mode::Off) return;
    g.at = ReadAt();
#ifndef _WIN64
    (void)base;
    (void)size;
    Log("%ls: shadow pre-pass not applied: 64-bit game only", baseName);
#else
    const char* why = nullptr;
    if (!ShadowPrepass_Locate(base, &g.t, &why)) {
        Log("%ls: shadow pre-pass not applied: %s", baseName, why);
        return;
    }
    g.omni = !g.t.omniWhy;
    g.stashes.resize(kMaxStashes);  // their item vectors grow on first use and keep their capacity
    HookSpec hooks[] = {
        {"EF_EndEf3D", g.t.endEf3D, reinterpret_cast<void*>(&EndEf3DHook), reinterpret_cast<void**>(&g.endEf3D)},
        {"PrepareDepthMap", g.t.prepareDepthMap, reinterpret_cast<void*>(&PrepareDepthMapHook),
         reinterpret_cast<void**>(&g.prepareDepthMap)},
        {"PrepareAllDepthMaps", g.t.prepareAll, reinterpret_cast<void*>(&PrepareAllHook),
         reinterpret_cast<void**>(&g.prepareAll)},
    };
    if (!InstallHooks(hooks, 3)) {  // (PrepareAllDepthMaps: the pre-pass with At=shadows, and the render queue's mark)
        Log("%ls: shadow pre-pass not applied: could not write the hooks", baseName);
        return;
    }
    g.installed = true;
    Log("%ls: shadow pre-pass installed (mode=%s, at=%s): EF_EndEf3D +0x%X, PrepareAllDepthMaps +0x%X, PrepareDepthMap "
        "+0x%X, lists +0x%X, level +0x%X; point lights: %s",
        baseName, ModeName(g.mode), g.at == At::Flush ? "flush" : "shadows", unsigned(g.t.endEf3D - base),
        unsigned(g.t.prepareAll - base), unsigned(g.t.prepareDepthMap - base), unsigned(g.t.lists - base),
        unsigned(reinterpret_cast<char*>(g.t.level) - base), g.omni ? "yes" : g.t.omniWhy);
    FlushThread_Install(baseName, base, g.t.endEf3D);
    TraversalRecord_Install(baseName, base, g.t);  // (overlap: after the gate and the render queue's redirects)
    ReflectionPrepass_Install(baseName, base, g.t);
    EnvSnapshot_Install(baseName, g.t.system);
    ParticlePrepass_Install(baseName, base, g.t);
    SkinPrepass_Install(baseName, base, g.t);
    MaterialSnapshot_Install(baseName, base, g.t);
    CvarSnapshot_Install(baseName, base, size);
    SysSnapshot_Install(baseName, g.t.system);
    LightSnapshot_Install(baseName, base, size, g.t);
    EngineData_Install(baseName, base, size, g.t);
    Census_Install(baseName, base, size, g.t);
    FramePhases_Install(baseName, g.t.system);
    ProducerTiming_Install(baseName, base);
#endif
}

void ShadowPrepass_ForEachStash(void (*fn)(const sp::Stash&, void*), void* ctx) {
#ifdef _WIN64
    for (int i = 0; i < g.used; ++i)
        if (g.stashes[size_t(i)].state == kPending) fn(g.stashes[size_t(i)], ctx);
#else
    (void)fn;
    (void)ctx;
#endif
}

void ShadowPrepass_MapStashes(void (*fn)(char**, void**, void*), void* ctx) {
#ifdef _WIN64
    for (int i = 0; i < g.used; ++i) {
        sp::Stash& s = g.stashes[size_t(i)];
        if (s.state == kPending) fn(&s.frustum, &s.casterList, ctx);
    }
#else
    (void)fn;
    (void)ctx;
#endif
}

const char* ShadowPrepass_AfterStep() {
#ifdef _WIN64
    return g_afterStep;
#else
    return nullptr;
#endif
}

void ShadowPrepass_Report(const char* when) {
    Report(when);
    FlushThread_Report(when);
    ReflectionPrepass_Report(when);
    EnvSnapshot_Report(when);
    ParticlePrepass_Report(when);
    SkinPrepass_Report(when);
    MaterialSnapshot_Report(when);
    CvarSnapshot_Report(when);
    SysSnapshot_Report(when);
    LightSnapshot_Report(when);
    EngineData_Report(when);
    Census_Report(when);
    FramePhases_Report(when);
    ProducerTiming_Report(when);
    TraversalRecord_Report(when);
}

}  // namespace optiperf
