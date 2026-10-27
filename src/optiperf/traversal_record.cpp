// The traversal's producer calls recorded: see traversal_record.h.
//
// The producers' slots go through the gate's redirects to hooks that forward this and nine integer words (the
// methods take integers, pointers and references only, none more than nine; the words past a method's own arguments
// are its caller's frame, forwarded and ignored). On the main thread, from the main traversal's EF_StartEf (the level
// 0 to 1) to the outer flush's entry, each outermost call is recorded: its words, the state of the render object it is
// given (an argument, or the method's this) at the call and after it, a copy of what it points to that the replay
// can't find again (EF_AddEf's shader item, Render's SRendParams with what its words point to on the stack: a pointer
// into the stack is copied with the 128 bytes there; a fog contribution's colour; AddWaves' two-pointer result array;
// a polygon's vertices, tangents and indices; a light), its result, and the items it added to the render lists (each
// list's new suffix); for a polygon, the one it added to its level's storage and what it added to the renderer's two
// pools; for a light, its index and its coronas' objects after the call.
//
// A render object's state is the object and what the renderer has handed out for it, which the 3D engine writes
// through: its data (GetObjData's result, 32 bytes) and its two wave forms (AddWaves writes their pointers into its
// argument, 20 bytes each).
//
// verify then replays the record at the flush's entry, at the traversal's own level after its items (an item's batch
// flags follow the level: the main pass's are a recursive level's but for its z-pass and other bits): each call with
// its words (the copies' pointers in place of the originals), the recorded objects mapped to the replay's EF_GetObject
// results (a permanent object by its id is the same object: its contents put back after the replay). Each object
// starts from its copy as EF_GetObject handed it out (but its pool and frame indices), and gets before each call what
// the 3D engine changed in it, and in what was handed out for it, since it last saw them: the renderer's own writes
// in the replay's object stay (IRenderMesh::Render writes two words at every call), as they must where the 3D engine
// filled a copy of its own. What a replayed call hands out new starts as the recorded call's did (GetObjData doesn't
// set all of an object's data: the rest is what its slot in the array held). The replay compares per call: the items added (the objects mapped), the object's state
// after the call (the object from +0x10, past the renderer's indices, but its wave id), a fog contribution's index
// from where each run's contributions began, a polygon (its fields, offsets and data), a light (its index, its
// coronas' objects). Then each list is cut back to where the replay began, and the frame's pools put back as they
// were (the render objects' count, their data's, the waves', the fog contributions', the polygons' and their pools',
// the level's lights'): the
// replay checks first that each has the room for what it will add, as a pool that grows moves under the pointers the
// frame has handed out into it.
//
// The render meshes' updates (SetChunk, SetMaterial, UpdateSysIndices, ...) are observed, not recorded yet: how many
// of the traversal's are on a mesh the frame before drew, what the traversal reads back after them, and when.
//
// And what step 5 will answer beside the flush (the producers' results it can't get from the calls: indices, merged
// objects, a temporary object's reset fields) is predicted at each call and checked against what the call answered.
#include "traversal_record.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "calltrace.h"
#include "config.h"
#include "diag.h"
#include "engine_data.h"
#include "flush_thread.h"
#include "log.h"
#include "particle_batch.h"
#include "render_gate.h"
#include "render_queue.h"
#include "scan.h"
#include "shadow_prepass_lists.h"

namespace optiperf {
namespace {

#ifdef _WIN64
using Fn10 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                          uint64_t);
constexpr int kWords = 9;  // after this

enum CallKind : uint8_t {
    kStartEf, kGetObject, kAddEf, kRender, kAddRenderElements, kAddRE, kFogContrib, kAddWaves, kGetObjData, kParticles,
    kPolygon, kPolygonOut, kAddLight, kCallKinds
};
const char* const kCallNames[] = {
    "EF_StartEf",         "EF_GetObject",       "EF_AddEf",
    "IRenderMesh::Render", "IRenderMesh::AddRenderElements", "IRenderMesh::AddRE",
    "PushFogVolumeContribution", "CRenderObject::AddWaves", "CRenderObject::GetObjData", "EF_AddParticlesToScene",
    "EF_AddPolygonToScene", "EF_AddPolygonToScene (its storage handed out)", "EF_ADDDlight"};
static_assert(sizeof kCallNames / sizeof kCallNames[0] == kCallKinds, "a name per kind");
constexpr int kNoObject = -1, kSelf = -2;  // the object: none; the method's this
struct Slot {
    const char* cls;
    unsigned slot;
    int objectArg;  // the render object's word (0: a1, ...), kSelf or kNoObject
};
const Slot kSlots[] = {
    {".?AVCD3D9Renderer@@", 112, kNoObject}, {".?AVCD3D9Renderer@@", 113, kNoObject}, {".?AVCD3D9Renderer@@", 114, 2},
    {".?AUCRenderMesh@@", 47, 1},            {".?AUCRenderMesh@@", 24, 0},            {".?AUCRenderMesh@@", 40, 0},
    {".?AVCD3D9Renderer@@", 194, kNoObject}, {".?AVCRenderObject@@", 0, kSelf},       {".?AVCRenderObject@@", 1, kSelf},
    {".?AVCD3D9Renderer@@", 87, 1},          {".?AVCD3D9Renderer@@", 92, 4},          {".?AVCD3D9Renderer@@", 91, 1},
    {".?AVCD3D9Renderer@@", 117, kNoObject}};
static_assert(sizeof kSlots / sizeof kSlots[0] == kCallKinds, "a slot per kind");

constexpr size_t kObjectBytes = 0x130;
constexpr size_t kObjectIds = 0x8, kObjectIdsEnd = 0xC;  // m_Id, m_VisId: the pool's and the frame's index
constexpr size_t kObjectCompareFrom = 0x10;              // past m_nObjDataId, m_FogVolumeContribIdx
constexpr size_t kObjDataId = 0xC, kWaveId = 0xD2;       // (int16: none below 0; uint16: none at 0)
constexpr size_t kObjectOwnEnd = 0xE;                    // (on: the ids and the data's id stay the real object's)
constexpr size_t kShaderItemBytes = 24;                  // SShaderItem: shader, resources, technique, flags
constexpr size_t kParamsBytes = 200, kParamsWords = kParamsBytes / 8;  // SRendParams
constexpr size_t kStackCopyBytes = 128;
constexpr size_t kColorBytes = 16, kWavesArg = 16;  // ColorF; AddWaves' SWaveForm2*[2]
constexpr size_t kRObjectCount = 0x141A0;  // the frame's render objects (EF_GetObject's), at most kMaxObjects
constexpr int kMaxObjects = 0x3FFF, kObjectsMargin = 64;  // (the replay's objects are dropped before the captures')
constexpr unsigned kNone = ~0u;
constexpr int kLoggedDiffer = 12;
// a render object's state: the object, its data, its two wave forms, which of those it has
constexpr size_t kDataBytes = 32, kWaveBytes = 20, kAttachments = 3;
constexpr size_t kAttachOffset[kAttachments] = {kObjectBytes, kObjectBytes + kDataBytes,
                                                kObjectBytes + kDataBytes + kWaveBytes};
constexpr size_t kAttachBytes[kAttachments] = {kDataBytes, kWaveBytes, kWaveBytes};
constexpr size_t kStateMask = kObjectBytes + kDataBytes + 2 * kWaveBytes;
constexpr size_t kStateBytes = (kStateMask + 1 + 7) & ~size_t(7);
// The render objects' data (SRenderObjData, 32 bytes each): one array a frame (EF_StartEf's), its count and capacity
// before its elements. CRenderObject::GetObjData (slot 1) gives an object one when it has none by appending to it,
// read from its code: `mov rax, [rip+array]` at +28, `lea rcx, [rip+array]` at +55.
constexpr const char kGetObjDataCode[] =
    "40 53 48 83 EC 20 66 83 79 0C 00 48 8B D9 7D 74 84 D2 75 08 33 C0 48 83 C4 20 5B C3 48 8B 05 ?? ?? ?? ?? 48 89 74 "
    "24 30 48 89 7C 24 38 8B 48 FC 0F BF D1 66 89 4B 0C 48 8D 0D ?? ?? ?? ?? 83 C2";
constexpr size_t kGetObjDataBytes = 64, kObjDataLoadAt = 28, kObjDataLeaAt = 55;
// The wave forms (SWaveForm2, 20 bytes each): one TArray a frame (elements, count, capacity), two for an object that
// has none yet (CRenderObject::AddWaves, slot 0), its count read at +35: `mov r8d, [rip+count]`.
constexpr const char kAddWavesCode[] =
    "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 20 66 83 B9 D2 00 00 00 00 48 8B F2 48 8B F9 0F 85 ?? ?? ?? ?? 44 8B 05 "
    "?? ?? ?? ??";
constexpr size_t kAddWavesBytes = 42, kWavesCountAt = 35;
// The fog contributions (ColorF, 16 bytes each): a std::vector in the renderer, its displacement read from
// PushFogVolumeContribution (slot 194, `lea rbx, [rcx+d]` at +6), which answers the new one's index.
constexpr const char kFogContribCode[] =
    "40 53 48 83 EC 20 48 8D 99 ?? ?? ?? ?? 48 8B 43 08 48 2B 03 48 C1 F8 04 48 3D FF FF 00 00";
constexpr size_t kFogContribBytes = 30, kFogVectorAt = 9;
// EF_AddParticlesToScene(SShaderItem&, CRenderObject*, IParticleVertexCreator*, const SParticleRenderInfo&, int nAW,
// bool& canUseGS) takes its element from a pool of CREParticle kept across frames (a TArray of pointers: the
// elements, how many are used this frame, how many there are), which a function it calls directly initializes (its
// count read at +26: `mov r9d, [rip+count]`). It merges the object into an earlier one with the same batching state
// (particle_batch.cpp: the renderer's batch list, the given object put on the temporary objects' free list). The
// replay starts from the batch list and the free list as they were at the traversal's start, and puts them back.
constexpr const char kParticlePoolCode[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 48 83 EC 20 44 8B 0D ?? ?? ?? ?? 48 63 1D ?? "
    "?? ?? ?? 41 0F B6 E8 49 8B F1 41";
constexpr size_t kParticlePoolBytes = 48, kParticlePoolCountAt = 26, kParticlesScan = 0x240;
constexpr size_t kRenInfoBytes = 128;
// EF_AddPolygonToScene, in two forms (slot 92: SShaderItem&, int numPts, const SVF_P3F_C4B_T2F* verts, const
// SPipTangents* tangs, CRenderObject*, uint16* inds, int ninds, int nAW, bool merge; slot 91: SShaderItem&,
// CRenderObject*, int numPts, int ninds, then references to the three pointers it hands out, nAW, merge). Each call
// takes the next polygon of its level's storage (t.polyLists: kept across frames, a new one made where the slot is
// empty), and appends to the renderer's two pools (a TArray each): the vertices (the vertex format's size each, the
// size read from a global) and tangents (16 bytes each) to one, the indices to the other (16 bits each; slot 92 makes
// a fan's where it is given none). The polygon keeps its pools' offsets. With merge, the object is first looked for
// in a list of the frame's polygon objects (another TArray): one with the same lighting state found there is used in
// its place, the given one put on the temporary objects' free list; one not found is appended. The replay starts
// that list from its state at the traversal's start, as the particles' batch list.
// Slot 92's start (merge at [rsp+0x98], the list's merge called at +57) ...
constexpr const char kPolygonCode[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 4C 89 4C 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 20 80 BC 24 98 "
    "00 00 00 00 45 8B E8 4C 8B E2 48 8B D9 74 0F 48 8B 54 24 78 E8 ?? ?? ?? ?? 4C 8B F8 EB 05 4C 8B 7C 24 78";
constexpr size_t kPolygonMergeCallAt = 57, kPolygonScan = 0x100;
// ... and slot 91's (the same call at +53)
constexpr const char kPolygonOutCode[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 80 BC 24 98 00 00 00 00 45 "
    "8B E9 49 8B F0 4C 8B F2 48 8B F9 74 0B 49 8B D0 E8 ?? ?? ?? ?? 48 8B F0";
constexpr size_t kPolygonOutBytes = 61, kPolygonOutMergeCallAt = 53;
// the merge: the list's count at +28, its elements at +42
constexpr const char kPolyMergeCode[] =
    "48 89 5C 24 20 57 48 83 EC 20 48 85 D2 48 8B DA 48 8B F9 0F 84 ?? ?? ?? ?? 44 8B 81 ?? ?? ?? ?? 33 C0 45 85 C0 "
    "74 1A 48 8B 91 ?? ?? ?? ??";
constexpr size_t kPolyMergeBytes = 46, kPolyMergeCountAt = 28, kPolyMergeElemsAt = 42;
// in slot 92, within its first 0x300 bytes: the polygon's fields set (the shader item's three words at +0x30, +0x38
// and +0x40, numPts at +0x50, the object at +0x48, nAW at +0x54), then the vertices' room in their pool: the vertex
// size (`mov ecx, [rip+size]` at +46), the pool's count at +55
constexpr const char kPolyVertsCode[] =
    "49 8B 04 24 48 89 46 30 49 8B 44 24 08 48 89 46 38 49 8B 44 24 10 66 44 89 6E 50 48 89 46 40 0F B6 84 24 90 00 "
    "00 00 4C 89 7E 48 88 46 54 8B 0D ?? ?? ?? ?? 44 8B B3 ?? ?? ?? ?? 4C 63 A3 ?? ?? ?? ?? 41 0F AF CD 49 63 EE 03 "
    "CD 3B 8B ?? ?? ?? ?? 89 8B ?? ?? ?? ?? 76 2E";
constexpr size_t kPolyVertsBytes = 89, kPolyVertsScan = 0x300, kPolyVertSizeAt = 46, kPolyVertCountAt = 55;
constexpr size_t kPolyVertCountAt2 = 83, kPolyVertCapacityAt[] = {62, 77};  // (the same count stored, the capacity)
// ... and within its first 0x600 bytes, the indices' offset (+0x60) from their pool's count (at +10)
constexpr const char kPolyIndicesCode[] =
    "4C 8B B4 24 80 00 00 00 8B 83 ?? ?? ?? ?? 4D 85 F6 89 46 60 0F 84 ?? ?? ?? ?? 48 63 AC 24 88 00 00 00 85 ED 0F "
    "84 ?? ?? ?? ?? 4C 63 AB ?? ?? ?? ?? 4C 63 A3 ?? ?? ?? ?? 41 8D 4C 2D 00";
constexpr size_t kPolyIndicesBytes = 61, kPolyIndicesScan = 0x600, kPolyIndexCountAt = 10;
constexpr size_t kPolyIndexCountAt2 = 45, kPolyIndexCapacityAt = 52;
// CREClientPoly: the shader item's words, the object, the vertex and index counts, nAW, the pools' offsets (the
// tangents' -1 for none)
constexpr size_t kPolyItemAt = 0x30, kPolyObjectAt = 0x48, kPolyVertsAt = 0x50, kPolyIndicesAt = 0x52;
constexpr size_t kPolyAWAt = 0x54, kPolyVertOffsAt = 0x58, kPolyTangOffsAt = 0x5C, kPolyIndexOffsAt = 0x60;
constexpr size_t kTangentBytes = 16;
constexpr int kMaxPolyPoints = 0xFFFF;  // (the polygon keeps 16 bits of them)
constexpr int32_t kMaxVertexBytes = 256;
constexpr uint32_t kPolyLevels = 4;  // (the storage's levels, as EF_StartEf resets them)
// EF_ADDDlight(CDLight*) (slot 117) gives the light its index in its level's list (m_Id, its first field: -1 for a
// fake light or a full list, 32), appends it to the list (a TArray in the renderer, 16 bytes a level), and for a light
// with a shader gives its coronas' permanent render objects (m_pObject[level][k] in the light, 4 a level: taken again
// by their id each frame, made where there is none) its matrix, colour, m_pLight and bit (1 << m_Id), and adds their
// items (EF_AddEf). The replay passes a copy of the recorded light with the recorded call's objects in it, on its own
// stack (the light part leaves lights there to the reflection part); it starts the level's list from its count at the
// traversal's start (the indices, and the objects' bits, follow), and puts the list's entries and the objects'
// contents back after it. Read from its code past its start (the light part's hook patches that): the index set to -1
// at [rdi], then the level's count (the list's +8) against 32 ...
constexpr const char kAddLightListCode[] =
    "84 C0 74 08 C7 07 FF FF FF FF EB ?? 48 63 05 ?? ?? ?? ?? 48 03 C0 8B 8C C6 ?? ?? ?? ?? 83 F9 20";
constexpr size_t kAddLightLevelAt = 12, kAddLightCountAt = 25, kAddLightScan = 0x100;
// ... the level's objects in the light (lea rbx, [rcx+rdi+X], rcx the level x 0x20) ...
constexpr const char kAddLightObjectsCode[] = "48 8D 9C 39 ?? ?? ?? ?? 4D 8B EC 48 8B 0B 48 85 C9 75";
constexpr size_t kAddLightObjectsAt = 4, kAddLightBodyScan = 0x400;
// ... and on each object, the light's matrix (48 bytes to its +0x14), its m_pLight and the light's bit
constexpr const char kAddLightCoronaCode[] =
    "48 8D 97 ?? ?? ?? ?? 41 B8 30 00 00 00 48 83 C1 14 E8 ?? ?? ?? ?? 4C 8B 1B BA 01 00 00 00 49 89 BB ?? ?? ?? ?? "
    "8B 0F 48 8B 03 D3 E2 89 90 ?? ?? ?? ??";
constexpr size_t kAddLightPointerAt = 33;
constexpr size_t kLightBytes = 0x258;  // CDLight (light_record.h: the renderer's constructor clears 600 bytes)
constexpr uint32_t kLightLevels = 4, kLightObjects = 4;
constexpr size_t kLightLevelBytes = kLightObjects * 8;
constexpr size_t kReplayLights = 40;  // the replay's copies of the lights, on its stack
// on: the stand-ins' room (a frame has at most 0x4000 render objects), the renderer's objects by their id (a TArray,
// as EF_GetObject(false, id) reads it), why a traversal joined
constexpr uint32_t kStandIns = 0x4000, kStandInData = 0x4000, kStandInWaves = 0x8000;
constexpr size_t kStandInStorage = size_t(8) << 20;
constexpr size_t kStandInSetBytes = size_t(kStandIns) * kObjectBytes + size_t(kStandInData) * kDataBytes +
                                    size_t(kStandInWaves) * kWaveBytes + kStandInStorage;
constexpr size_t kRAllObjects = 0x141C8;
constexpr int kFrameObjects = 0x4000;
// What the real calls start the storage they hand out as, for the stand-ins' (so that the 3D engine sees the same, and
// the real storage can start as the stand-in's did): GetObjData resets a new entry's pointer at +0x18 to a default of
// the renderer's (lea rax, [rip+default] at +15, its displacement at +26) and leaves the rest; AddWaves starts each
// wave form as {1, 0, 0, 0, 0}
constexpr const char kDataDefaultCode[] =
    "83 39 00 74 16 48 85 C9 74 05 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 44 37 ??";
constexpr size_t kDataDefaultLeaAt = 15, kDataDefaultFieldAt = 26, kDataDefaultScan = 0x100;
constexpr uint32_t kWaveStartType = 1;
// EF_Query (renderer slot 121): the traversal reads the light it added back by its index (type 27: the current
// level's list's light, or 0 past its count)
constexpr unsigned kQuerySlot = 121;
// beside: the renderer's slots made beside the flush while the record answers the traversal: EF_IsFakeDLight (a test
// of the light's flags) and EF_UpdateDLight (it writes the 3D engine's light, which the flush reads through the light
// part's copies, and a light style's cache, which the flares' prepare in the flush writes too: a flare's colour
// computed for the other's time, at worst)
constexpr unsigned kBesideSlots[] = {116, 118};
constexpr int kBesides = int(sizeof kBesideSlots / sizeof kBesideSlots[0]);
// beside, after a join: one traversal in kObserveEvery keeps the gate on for the rest of it, to see which of its
// renderer calls would have joined too (the gate's slots, how many calls, where in the traversal each came first)
constexpr unsigned kObserveEvery = 8;
constexpr int kObserved = 64;
constexpr uint32_t kQueryLight = 27, kQueryLevel = 19, kQueryTypes = 64;  // (19: the recursion level)
enum JoinWhy : uint8_t { kJoinLevel, kJoinNewPermanent, kJoinNoRoom, kJoinUnknownObject, kJoinRendererData,
                         kJoinPermanentId, kJoinOther, kJoinFlush, kJoinFlushEnded, kJoinMeshSetting, kJoinWhys };
const char* const kJoinNames[kJoinWhys] = {"a level inside the traversal", "a new permanent object",
                                           "the stand-ins' room", "an object not handed out by it",
                                           "an object's data or wave forms the renderer's", "a permanent id out of range",
                                           "other", "the flush joined (beside it)", "the flush beside it ended",
                                           "a mesh setting that changes something"};
constexpr int kJoinSites = 48;  // beside: the gate's thunks the traversals joined at most, by their count
// The render meshes' updates (batch C), observed so far, not recorded (made twice, some would reallocate what render
// elements point into): how many of the traversal's outermost ones are on a mesh the flush before it drew (one
// rendered on the main thread in the frame before: in its traversal or at its flush's entry), and how many meshes.
constexpr unsigned kMeshUpdateSlots[] = {25, 26, 27, 30, 31, 34, 36, 38, 39, 42, 43};
constexpr int kMeshUpdates = int(sizeof kMeshUpdateSlots / sizeof kMeshUpdateSlots[0]);
const char* const kMeshUpdateNames[] = {
    "SetRECustomData", "SetChunk",           "SetChunk (2)",          "UpdateSysIndices",
    "UpdateSysVertices", "SetMaterial",      "SetBBox",               "UpdateBBoxFromMesh",
    "InvalidateVideoBuffer", "SetVertexContainer", "SetUpdateFrame"};
static_assert(sizeof kMeshUpdateNames / sizeof kMeshUpdateNames[0] == kMeshUpdates, "a name per mesh update");
// the settings a frame makes on its meshes (SetRECustomData, SetMaterial); the others rebuild a mesh
constexpr bool kMeshSetting[kMeshUpdates] = {true, false, false, false, false, true, false, false, false, false, false};
// beside: a setting that would change nothing isn't made, and the traversal goes on beside the flush (which may be
// drawing the mesh's elements); one that would change something joins first. Both return the chunks' array (+0x108;
// declared void). SetMaterial(material, texture id) references the material (AddRef, then the old one's Release) and
// keeps it (+0x30); with a texture id, it sets each chunk's element's +0x18 to it. SetRECustomData(data) sets each
// chunk's element's custom data (+0x10), which the engine data part may have pointed at its copy for the flush (put
// back after it). The chunks: 0x48 bytes each, their element at +0x20; the array's elements at +0, its count at +8.
constexpr int kMeshSetCustomData = 0, kMeshSetMaterial = 5;  // (their places in kMeshUpdateSlots)
constexpr int kMeshSkips = 2;                                // (the two, by kMeshSkipOf)
constexpr int kMeshSkipOf[kMeshSkips] = {kMeshSetCustomData, kMeshSetMaterial};
constexpr size_t kMeshMaterial = 0x30, kMeshChunks = 0x108, kChunkBytes = 0x48, kChunkElement = 0x20;
constexpr size_t kElementCustomData = 0x10, kElementTexBind = 0x18;
constexpr const char* const kMeshSkipCode[kMeshSkips] = {
    "48 8B 81 08 01 00 00 45 33 C0 4C 8B D2 44 39 40 08 7E 33 4D 8B C8 66 66 66 90 66 66 90 66 66 90 48 8B 00 4A 8B "
    "54 08 20 48 85 D2 74 04 4C 89 52 10 48 8B 81 08 01 00 00 41 83 C0 01 49 83 C1 48 44 3B 40 08 7C DA C2 00 00",
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 85 D2 41 8B F8 48 8B F2 48 8B D9 74 09 48 8B 02 48 8B CA FF 50 "
    "08 48 8B 4B 30 48 85 C9 74 06 48 8B 01 FF 50 10 48 89 73 30 48 8B 83 08 01 00 00 48 85 C0 74 32 85 FF 74 2E 33 "
    "C9 39 48 08 7E 27 48 8B D1 48 8B 00 4C 8B 44 02 20 4D 85 C0 74 04 41 89 78 18 48 8B 83 08 01 00 00 83 C1 01 48 "
    "83 C2 48 3B 48 08 7C DC 48 8B 5C 24 30 48 8B 74 24 38 48 83 C4 20 5F C3"};
// the render meshes' other slots, observed too: how often the traversal calls each on its own, and on a mesh it
// updated earlier (what it reads back); each goes on to what was behind it (the render queue's hooks: its getters)
constexpr size_t kMeshSlots = 96;

// What step 5 will answer beside the flush, predicted here and checked against what the calls answer:
//   - PushFogVolumeContribution's index: the contributions since the traversal's start, from where they began (full
//     at 0xFFFF);
//   - EF_ADDDlight's index (m_Id): -1 for a fake light (two bits of its flags, as EF_IsFakeDLight, slot 116, tests
//     them), else the level's lights since the traversal's start, -1 from 32;
//   - EF_AddPolygonToScene's object (with merge): the merge over the frame's polygon objects as the prediction keeps
//     them (the lighting fields its merge compares: two words, six floats compared as ucomiss does, unordered equal,
//     and a byte);
//   - EF_AddParticlesToScene's bool&: cleared unless a flag of the renderer's (a global) is set and the shader item's
//     technique (as SShaderItem's technique lookup finds it) has its flag bit;
//   - EF_GetObject's temporary object: the fields its reset writes, as the frame before's first one had them.
// EF_IsFakeDLight's test, past its start: the flags' displacement at +2, the two bits at +11 and +25
constexpr const char kFakeLightCode[] =
    "8B 8A ?? ?? ?? ?? 32 C0 0F BA E1 ?? 72 ?? 0F B6 C0 BA 01 00 00 00 0F BA E1 ?? 0F 42 C2";
constexpr size_t kFakeLightFlagsAt = 2, kFakeLightBitAt[] = {11, 25}, kFakeLightScan = 0x60;
constexpr unsigned kFakeLightSlot = 116;
// EF_AddParticlesToScene, past its start: the technique lookup's call (+0), the renderer's flag (cmp [rip+d], 0 at +5:
// 7 bytes), then the technique's flag bit (bt [rax+d8], bit: the displacement at +39, the bit at +40)
constexpr const char kParticleGSCode[] =
    "E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 48 8B AC 24 ?? ?? ?? ?? 75 04 C6 45 00 00 48 85 C0 BE 01 00 00 00 74 ?? 0F BA "
    "60 ?? ?? 73 ?? 80 7D 00 00";
constexpr size_t kParticleGSFlagAt = 5, kParticleTechFlagsAt = 39, kParticleTechBitAt = 40, kParticleGSScan = 0x100;
// SShaderItem's technique lookup: its index (+0x10 of the item, 0 below 0), the shader's count of techniques and
// their array
constexpr const char kTechniqueCode[] =
    "8B 41 ?? 33 D2 85 C0 0F 48 C2 48 8B 11 48 85 D2 74 10 3B 42 ?? 7D 0B 8B C8 48 8B 42 ?? 48 8B 04 C8 C3";
constexpr size_t kTechIndexAt = 2, kTechCountAt = 20, kTechArrayAt = 28;
// the polygon merge's lighting fields (+0x102590's code): two words, six floats, then a byte
constexpr size_t kMergeWords[] = {0x8C, 0xC0}, kMergeFloats[] = {0xC4, 0x44, 0x48, 0x4C, 0x50, 0xF8};
constexpr size_t kMergeByte = 0xCC;
// what a temporary object's reset (CRenderObject's, which EF_GetObject calls) writes: (offset, bytes)
constexpr size_t kResetFields[][2] = {{0x0C, 8},  {0x44, 16}, {0x84, 4},  {0x89, 2},   {0x8C, 4},   {0xA8, 24},
                                      {0xC0, 20}, {0xD8, 32}, {0xF8, 4},  {0x108, 16}, {0x120, 16}};
// a particle element pooled per call is the recorded call's if it is the same type with the same source and settings
// (as the shadow pre-pass's compare mode takes them)
bool SameParticle(const char* a, const char* b) {
    return *reinterpret_cast<void* const*>(a) == *reinterpret_cast<void* const*>(b) &&
           std::memcmp(a + 0x30, b + 0x30, 0x20) == 0 &&
           ((*reinterpret_cast<const uint32_t*>(a + 0xC) ^ *reinterpret_cast<const uint32_t*>(b + 0xC)) & 0x800) == 0;
}

struct Call {
    CallKind kind;
    uint64_t self;
    uint64_t a[kWords];
    uint32_t object = kNone;                 // its object's index in the record's objects (kNone: none, or unknown)
    uint32_t before = kNone, after = kNone;  // the object's state at the call, and after it (offsets in the bytes)
    uint32_t copy = kNone;                   // what an argument points to (see above)
    uint32_t copy2 = kNone;                  // EF_AddParticlesToScene's render info
    uint32_t items = 0, itemCount = 0;       // what it added to the lists (in items)
    uint64_t result = 0;
    uint8_t flagBefore = 0, flagAfter = 0;   // EF_AddParticlesToScene's bool& (canUseGS)
    bool unknownObject = false;              // an object no recorded EF_GetObject returned: not replayed
};
struct Item {
    uint8_t list;
    sp::RendItem item;
    uint32_t objCopy;  // an object the call took itself (not one the record's EF_GetObject returned): its contents
                       // right after the call (offset in the bytes; kNone otherwise)
};
// Render's params: the 200 bytes, then the count of words that pointed into the stack, then each one's word index
// and the copy of the 128 bytes there
struct StackWord {
    uint32_t word, at;
};
// what the renderer handed out for an object: its data, its two wave forms
struct Attach {
    uint8_t* at[kAttachments] = {};
};
// a frame's TArray (elements, count, capacity) and the renderer's fog contributions (a std::vector)
struct TArray {
    uint8_t* elems;
    uint32_t count, capacity;
};
struct Vector {
    uint8_t *begin, *end, *capacityEnd;
};
// a polygon call's record (in the bytes): the copies of the arrays it was given (slot 92), its level, the polygon it
// added (its index in the level's storage), the two pools' counts and the polygon objects' list's before it, and what
// it added to each
struct PolyRec {
    uint32_t verts = kNone, tangs = kNone, inds = kNone;
    uint32_t level = 0, poly = kNone;
    uint32_t vert0 = 0, index0 = 0, objects0 = 0;
    uint32_t vertAdded = 0, indexAdded = 0, objectsAdded = 0;
};
// a light call's record (in the bytes): its level, the list's count before it and what it added, the light's index
// after it, its level's corona objects after it and their contents then (kObjectBytes each, in the bytes)
struct LightRec {
    uint32_t level = 0, count0 = 0, added = 0;
    int32_t id = 0;
    uint64_t objects[kLightObjects] = {};
    uint32_t objectsAt = kNone;
    uint8_t touched = 0;  // the objects the call gave the light (their m_pLight the light): saved, compared, put back
};

struct Stats {
    uint64_t frames = 0, verified = 0, broken = 0, noRoom = 0, notStarted = 0, noDataRoom = 0, noWavesRoom = 0;
    uint64_t noFogRoom = 0, poolsMoved = 0;
    uint64_t calls[kCallKinds] = {}, replayed[kCallKinds] = {}, unknownObjects[kCallKinds] = {};
    uint64_t items = 0, itemsDiffer = 0, callsDiffer = 0, objectsCompared = 0, objectsDiffer = 0, ownObjects = 0;
    uint64_t attachCompared = 0, resultsCompared = 0, particlesByContents = 0;
    uint64_t polysCompared = 0, polyBytes = 0, polyMerged = 0;
    uint64_t lightsCompared = 0, coronasCompared = 0, noLightRoom = 0;
    uint64_t meshCalls[kMeshUpdates] = {}, meshOnDrawn[kMeshUpdates] = {}, meshNested[kMeshUpdates] = {};
    uint64_t meshesUpdated = 0, meshesUpdatedDrawn = 0, meshFrames = 0;
    uint64_t meshReads[kMeshSlots] = {}, meshReadsAfter[kMeshSlots] = {};
    // when in the traversal the first update of each group on a mesh the frame before drew came (the sum of the
    // fractions of its time, the frames that had one), and the traversals' time
    double firstSetting = 0, firstRebuild = 0;
    uint64_t framesSetting = 0, framesRebuild = 0, timedFrames = 0, traversalCycles = 0;
    // step 5's answers predicted: compared, differing
    uint64_t predFog = 0, predFogDiffer = 0, predLight = 0, predLightDiffer = 0, predPoly = 0, predPolyDiffer = 0;
    uint64_t predPolyMerged = 0, predParticles = 0, predParticlesDiffer = 0, predObjects = 0, predObjectsDiffer = 0;
    uint64_t predTemplates = 0, predTemplatesDiffer = 0;
    size_t predObjectFirstAt = 0;
    // on: the frames answered, learning, joined (and why), the replays' checks of the answers given
    uint64_t onFrames = 0, onLearning = 0, onJoins = 0, onJoinWhy[16] = {}, onReplayed = 0, onChecked = 0;
    // beside: where the traversals joined (the gate's thunk, or the record's reason), how many, and how much of the
    // traversal ran before (the sum of the fractions); the traversals that reached the flush's entry without one; those
    // that started with no flush beside them (the update joined it), made and not recorded
    unsigned joinSite[kJoinSites] = {};
    uint64_t joinSiteCount[kJoinSites] = {};
    double joinSiteShare[kJoinSites] = {};
    int joinSites = 0;
    uint64_t besideWhole = 0, besideFrames = 0, besideNone = 0, besideEnded = 0;
    // the mesh settings beside the flush: not made (they changed nothing), made after a join (they changed something)
    uint64_t meshSkipped[kMeshSkips] = {}, meshChanged[kMeshSkips] = {};
    // the answered traversals' time: answered (to the join), in the join (the flush's end, with its steps after and
    // the queue's calls; then the record made), made after it (to the flush's entry)
    uint64_t answeredCycles = 0, joinFlushCycles = 0, joinRecordCycles = 0, madeCycles = 0, timedJoins = 0;
    // the traversals made with no flush beside them (beside: the update joined it, or it had ended): their time
    uint64_t madeTraversalCycles = 0, madeTraversals = 0;
    // the observed traversals' calls that would have joined: the gate's slot, its calls, the traversals it came in and
    // where it came first in each (the sum of the fractions)
    unsigned observed[kObserved] = {};
    void* observedCaller[kObserved] = {};  // (where the first was called from)
    uint64_t observedCalls[kObserved] = {}, observedIn[kObserved] = {};
    double observedFirst[kObserved] = {};
    int observeds = 0;
    uint64_t observedTraversals = 0;
    double besideShare = 0;
    uint64_t onDiffer = 0, onStandIns = 0, onFinalMerges = 0, onSkipped = 0;
    // EF_Query in the traversal: by its type; its light by index answered (on) or checked (verify)
    uint64_t queries[64] = {}, queryLightAnswered = 0, queryLightChecked = 0, queryLightDiffer = 0;
    // the items the calls added, by kind, and the calls that added none (verify: the traversal's own calls; on: the
    // record's, made): the two modes' runs compared
    uint64_t itemsByKind[kCallKinds] = {}, emptyByKind[kCallKinds] = {};
    uint64_t stackWords = 0, replayCycles = 0, recordCycles = 0;
    // the calls that changed the object they were given (the renderer's own writes into it), by word
    uint64_t objectWrites[kCallKinds] = {};
    uint64_t writeAt[kCallKinds][kObjectBytes / 8] = {};
};

struct State {
    bool installed = false;
    ShadowPrepassTarget t;
    Fn10 orig[kCallKinds] = {};
    char** objData = nullptr;  // the render objects' data array (its elements pointer)
    TArray* waves = nullptr;   // the wave forms
    size_t fogVector = 0;      // the fog contributions, in the renderer
    uint32_t batchList = 0, freeList = 0;    // the renderer's particle batch list and temporary objects' free list
    TArray* particlePool = nullptr;          // the particle elements
    uint32_t batchStart = 0, freeStart = 0;  // the two lists' counts when the traversal began
    uint32_t vertexPool = 0, indexPool = 0;  // the renderer's polygon pools
    uint32_t polyObjects = 0;                // the renderer's list of the frame's polygon objects
    const int32_t* vertexSize = nullptr;     // the polygons' vertex size (a global of the renderer's)
    uint32_t polyObjectsStart = 0;           // the list's count when the traversal began
    uint32_t lightList = 0;                  // the renderer's light lists (level 0's)
    uint32_t lightObjects = 0;               // a light's coronas' objects (level 0's)
    uint32_t lightPointer = 0;               // a render object's m_pLight
    uint32_t lightStart = 0;                 // the traversal's level's list's count when it began
    Fn10 meshOrig[kMeshUpdates] = {};        // the render meshes' updates (observed)
    std::unordered_set<void*> drawn[2];      // the render meshes rendered on the main thread: this frame, the last
    int drawnNow = 0;
    std::unordered_set<void*> updated;       // the meshes the traversal updated
    Fn10 meshNext[kMeshSlots] = {};          // the render meshes' other slots (observed): what was behind each
    int meshDepth = 0;                       // inside an update (its own calls aren't the traversal's)
    uint64_t traversalStart = 0, firstSettingAt = 0, firstRebuildAt = 0;
    // the predictions' state (see above): what they read, and what they keep through a traversal
    bool predicting = false;                 // (their code found)
    uint32_t lightFlags = 0, fakeBits = 0;   // a light's flags, EF_IsFakeDLight's bits
    const int32_t* gsFlag = nullptr;         // the renderer's flag EF_AddParticlesToScene checks
    uint32_t techIndex = 0, techCount = 0, techArray = 0, techFlags = 0, techBit = 0;
    uint32_t fogPredicted = 0, lightPredicted = 0;
    std::vector<void*> polyPredicted;
    uint8_t objectTemplate[kObjectBytes] = {}, nextTemplate[kObjectBytes] = {};
    bool haveTemplate = false, haveNext = false, firstObject = false;
    // step 5.2, on: the traversal's producer calls answered, not made, and made at the flush's entry (see the header)
    bool on = false;          // FlushThreadRecord=on (or beside)
    bool beside = false;      // FlushThreadRecord=beside: the traversal beside the flush (step 5.3)
    bool onReady = false;     // the starts and a template learned from a frame whose calls were made
    bool answering = false;   // this traversal's calls answered
    bool joined = false;      // this traversal joined: its record made, its calls made from there on
    bool joiningNow = false;  // (the record being made at a join: the flush's join's callback leaves it be)
    unsigned gateIndex[kCallKinds] = {};  // the record's slots' thunks (they pass the gate while it answers)
    unsigned besideIndex[kBesides] = {};          // the thunks of the slots made beside the flush (kBesideSlots)
    unsigned meshSkipIndex[kMeshSkips] = {~0u, ~0u};  // the thunks of the mesh settings not made when they change
                                                      // nothing (~0u: none)
    bool observe = false;                         // FlushThreadRecordObserve=on (beside): one joined traversal in
                                                  // kObserveEvery observed
    bool observing = false;                       // the rest of this traversal observed (beside, after its join)
    uint64_t joinedAt = 0;                        // when this traversal's join ended (beside, inside it)
    uint64_t madeStart = 0;                       // when this traversal, made with no flush beside it, started
    uint64_t observeFirst[kObserved] = {};        // when each observed slot came first in it
    unsigned observeJoins = 0;
    uint64_t joinAt = 0;      // when this traversal joined, and why (a kJoinWhy, or the gate's thunk)
    int joinWhy = -1;
    unsigned joinGate = ~0u;
    char* renderer = nullptr;
    // what the last real EF_StartEf left (the fog contributions, the particle lists, the polygon objects, the
    // level's lights, the frame's objects)
    uint32_t learnedFog = 0, learnedBatch = 0, learnedFree = 0, learnedPolyObjects = 0, learnedLights = 0;
    int learnedObjects = 0;
    int objectsAnswered = 0;  // the frame's objects handed out so far (a stand-in's m_VisId)
    // the stand-ins: render objects, their data and wave forms, the storage a polygon call hands out
    uint8_t* standInSets[2] = {};  // (two sets in turn, each the objects, the data, the wave forms, the storage)
    int standInSet = 0;
    uint8_t* standIns = nullptr;
    uint8_t* standInData = nullptr;
    uint8_t* standInWaves = nullptr;
    uint8_t* standInStorage = nullptr;
    uint32_t standInsUsed = 0, standInDataUsed = 0, standInWavesUsed = 0;
    size_t standInStorageUsed = 0;
    std::vector<uint8_t> snapNow;  // a stand-in's state now (the final merge's)
    // the level's lights as the traversal has them so far (EF_Query's light by index): on, what the last real
    // EF_StartEf left in the list, then each light answered at its index; verify, the list's, then each light added
    std::vector<void*> lightsNow, learnedLightEntries;
    const void* dataDefault = nullptr;  // GetObjData's default for a new entry's pointer
    uint32_t dataDefaultAt = 0;
    // the record's real objects (permanent ones, which the 3D engine writes into itself) as they were when the
    // record began to be made
    std::vector<uint8_t> liveSaved;
    std::vector<uint32_t> liveIndex;
    Fn10 queryNext = nullptr;  // EF_Query: what its thunk went to (the render queue's hook)
    DWORD main = 0;
    ULONG_PTR stackLow = 0, stackHigh = 0;
    // this frame's record
    bool recording = false, broken = false;
    int depth = 0;
    std::vector<Call> calls;
    std::vector<uint8_t> bytes;
    std::vector<Item> items;
    std::vector<void*> objects;  // the record's objects, in EF_GetObject's order
    std::vector<Attach> attach;  // what was handed out for each of them
    std::unordered_map<void*, uint32_t> objectIndex;
    uint32_t fogBase = 0;  // the fog contributions when the traversal began
    // the replay
    std::vector<uint8_t> permanentSaved;  // the permanent objects' contents, put back after it
    std::vector<void*> batchSaved, freeSaved;  // the particle batch list's and the free list's entries, put back
    std::vector<void*> polyObjectsSaved;       // the polygon objects' list's
    std::vector<void*> lightsSaved;            // the level's light list's
    std::vector<void*> coronas;                // the lights' corona objects, their contents put back after it
    std::vector<uint8_t> coronasSaved;
    std::vector<void*> replayObjects;
    std::vector<Attach> replayAttach;
    std::vector<uint32_t> replaySeen;  // per record object: its state as the 3D engine last saw it (in the bytes)
    std::vector<Item> replayItems;
    int logged = 0;
    Stats st;
};
State g;

sp::ItemList* Lists() { return reinterpret_cast<sp::ItemList*>(g.t.lists); }
void Counts(uint32_t* c) {
    for (int k = 0; k < sp::kLists; ++k) c[k] = Lists()[k].count;
}
void Added(const uint32_t* before, std::vector<Item>& out) {
    sp::ItemList* const lists = Lists();
    for (int k = 0; k < sp::kLists; ++k)
        for (uint32_t i = before[k]; i < lists[k].count; ++i) out.push_back(Item{uint8_t(k), lists[k].elems[i], kNone});
}
// a word into the main thread's live stack: from this call's frame up (the callers' frames). Below it, the stack's
// reserve isn't all committed: a word there isn't the caller's data, and reading it can fault (core crashed on an
// SRendParams word of 0x74409, in the reserve's range, its page never committed)
bool OnStack(uint64_t v) {
    volatile uint8_t here = 0;
    const uint64_t live = uint64_t(uintptr_t(&here));
    return v >= live && v >= g.stackLow && v + 8 <= g.stackHigh;
}
uint32_t Copy(const void* p, size_t n) {
    const uint32_t at = uint32_t(g.bytes.size());
    const uint8_t* const b = static_cast<const uint8_t*>(p);
    g.bytes.insert(g.bytes.end(), b, b + n);
    return at;
}
// an object's state: the object, and what was handed out for it (a: null for none), into s (kStateBytes) ...
void SnapTo(uint8_t* s, const void* obj, const Attach* a) {
    std::memset(s, 0, kStateBytes);
    std::memcpy(s, obj, kObjectBytes);
    uint8_t mask = 0;
    for (size_t k = 0; a && k < kAttachments; ++k) {
        if (!a->at[k]) continue;
        std::memcpy(s + kAttachOffset[k], a->at[k], kAttachBytes[k]);
        mask = uint8_t(mask | (1u << k));
    }
    s[kStateMask] = mask;
}
// ... or into the bytes
uint32_t Snap(const void* obj, const Attach* a) {
    const uint32_t at = uint32_t(g.bytes.size());
    g.bytes.resize(size_t(at) + kStateBytes, 0);
    SnapTo(g.bytes.data() + at, obj, a);
    return at;
}
uint32_t CopyParams(const uint8_t* params) {
    const uint32_t at = Copy(params, kParamsBytes);
    uint64_t words[kParamsWords];
    std::memcpy(words, params, kParamsBytes);
    uint32_t n = 0;
    for (size_t w = 0; w < kParamsWords; ++w) n += OnStack(words[w]) ? 1 : 0;
    Copy(&n, 4);
    for (size_t w = 0; w < kParamsWords; ++w) {
        if (!OnStack(words[w])) continue;
        const size_t bytes = (std::min)(kStackCopyBytes, size_t(g.stackHigh - words[w]));
        uint8_t data[kStackCopyBytes] = {};
        std::memcpy(data, reinterpret_cast<const void*>(words[w]), bytes);
        const StackWord s{uint32_t(w), 0};
        Copy(&s, sizeof s);
        Copy(data, kStackCopyBytes);
        ++g.st.stackWords;
    }
    return at;
}
// the params as the replay passes them: the copy, its words into the stack pointed at their copies
void ReplayParams(uint32_t at, uint8_t* out) {
    std::memcpy(out, g.bytes.data() + at, kParamsBytes);
    uint32_t n;
    std::memcpy(&n, g.bytes.data() + at + kParamsBytes, 4);
    const uint8_t* p = g.bytes.data() + at + kParamsBytes + 4;
    for (uint32_t i = 0; i < n; ++i, p += sizeof(StackWord) + kStackCopyBytes) {
        StackWord s;
        std::memcpy(&s, p, sizeof s);
        const uint64_t copy = uint64_t(uintptr_t(p + sizeof(StackWord)));
        std::memcpy(out + s.word * 8, &copy, 8);
    }
}
template <typename T>
T At(const char* p, size_t at) {
    T v;
    std::memcpy(&v, p + at, sizeof v);
    return v;
}
TArray* Member(uint64_t renderer, uint32_t at) { return reinterpret_cast<TArray*>(uintptr_t(renderer) + at); }
TArray* Polygons(uint32_t level) { return reinterpret_cast<TArray*>(g.t.polyLists + size_t(level) * 16); }
TArray* Lights(uint64_t renderer, uint32_t level) { return Member(renderer, g.lightList + level * 16); }

// --- step 5's answers, predicted (see above) ---------------------------------------------------------------------------
int g_predictLogged = 0;
void LogPredict(const char* what, uint64_t predicted, uint64_t actual) {
    if (++g_predictLogged > kLoggedDiffer) return;
    Log("traversal record: PREDICTION difference #%d, frame %llu: %s: 0x%llX predicted, 0x%llX answered",
        g_predictLogged, g.st.frames, what, predicted, actual);
}
uint16_t PredictFog() {
    const uint32_t n = g.fogBase + g.fogPredicted;
    if (n >= 0xFFFF) return 0xFFFF;
    ++g.fogPredicted;
    return uint16_t(n);
}
int32_t PredictLightId(const char* light) {
    const uint32_t flags = At<uint32_t>(light, g.lightFlags);
    if (flags & g.fakeBits) return -1;
    if (g.lightPredicted >= 32) return -1;
    return int32_t(g.lightPredicted++);
}
bool SameLighting(const char* p, const char* o) {
    for (size_t at : kMergeWords)
        if (At<int32_t>(p, at) != At<int32_t>(o, at)) return false;
    for (size_t at : kMergeFloats) {
        const float a = At<float>(o, at), b = At<float>(p, at);
        if (a < b || a > b) return false;  // (ucomiss's equal: unordered counts as equal)
    }
    return p[kMergeByte] == o[kMergeByte];
}
uint64_t PredictPolygonObject(uint64_t obj, bool merge) {
    if (!merge || !obj) return obj;
    void* const o = reinterpret_cast<void*>(uintptr_t(obj));
    for (void* p : g.polyPredicted)
        if (p == o) return obj;
    for (void* p : g.polyPredicted)
        if (SameLighting(static_cast<const char*>(p), static_cast<const char*>(o))) return uint64_t(uintptr_t(p));
    g.polyPredicted.push_back(o);
    return obj;
}
bool PredictCanUseGS(uint64_t shaderItem, bool in) {
    if (!in || !g.gsFlag || !*g.gsFlag || !shaderItem) return false;
    const char* const item = reinterpret_cast<const char*>(uintptr_t(shaderItem));
    int32_t index = At<int32_t>(item, g.techIndex);
    if (index < 0) index = 0;
    const char* const shader = At<const char*>(item, 0);
    if (!shader || index >= At<int32_t>(shader, g.techCount)) return false;
    const char* const technique = At<const char* const*>(shader, g.techArray)[index];
    return technique && ((At<uint32_t>(technique, g.techFlags) >> g.techBit) & 1);
}
// a temporary object as EF_GetObject handed it out, against the frame before's first one in the fields its reset
// writes; the frame's first is the next frame's template
void PredictObject(const uint8_t* handedOut) {
    if (g.haveTemplate) {
        ++g.st.predObjects;
        if (g.firstObject) ++g.st.predTemplates;
        for (const auto& f : kResetFields) {
            if (std::memcmp(handedOut + f[0], g.objectTemplate + f[0], f[1]) == 0) continue;
            ++g.st.predObjectsDiffer;
            if (g.firstObject) ++g.st.predTemplatesDiffer;
            if (!g.st.predObjectFirstAt) g.st.predObjectFirstAt = f[0];
            LogPredict("a temporary object's reset field (its offset)", f[0], At<uint64_t>(
                reinterpret_cast<const char*>(handedOut), f[0]));
            break;
        }
    }
    if (g.firstObject) {
        std::memcpy(g.nextTemplate, handedOut, kObjectBytes);
        g.haveNext = true;
        g.firstObject = false;
    }
}
// at the traversal's start (after EF_StartEf: the lists and pools as it left them)
void PredictStart() {
    g.fogPredicted = 0;
    g.lightPredicted = g.lightStart;
    g.polyPredicted.clear();
    if (g.haveNext) {
        std::memcpy(g.objectTemplate, g.nextTemplate, kObjectBytes);
        g.haveTemplate = true;
    }
    g.firstObject = true;
}
char* PolygonAt(uint32_t level, uint32_t i) { return reinterpret_cast<char**>(Polygons(level)->elems)[i]; }
Vector* Fog(uint64_t renderer) { return reinterpret_cast<Vector*>(uintptr_t(renderer) + g.fogVector); }
uint32_t FogCount(uint64_t renderer) {
    const Vector* const v = Fog(renderer);
    return uint32_t((v->end - v->begin) / kColorBytes);
}

void Begin() {
    g.calls.clear();
    g.bytes.clear();
    g.items.clear();
    g.objects.clear();
    g.attach.clear();
    g.objectIndex.clear();
    g.recording = true;
    g.broken = false;
    // on: this traversal answered (once a frame whose calls were made taught the starts and a template); its stand-ins
    // from the other of the two sets (a stand-in the 3D engine kept from the frame before goes on to a set unused now)
    g.joined = false;
    g.answering = g.on && g.onReady;
    g.joinWhy = -1;
    g.joinGate = ~0u;
    if (g.on) {
        g.standInSet ^= 1;
        g.standIns = g.standInSets[g.standInSet];
        g.standInData = g.standIns + size_t(kStandIns) * kObjectBytes;
        g.standInWaves = g.standInData + size_t(kStandInData) * kDataBytes;
        g.standInStorage = g.standInWaves + size_t(kStandInWaves) * kWaveBytes;
    }
    g.standInsUsed = g.standInDataUsed = g.standInWavesUsed = 0;
    g.standInStorageUsed = 0;
    g.objectsAnswered = 0;
}

// --- on: a call answered (step 5.2) -------------------------------------------------------------------------------
// What the 3D engine gets for a producer call not made: a stand-in object (a temporary one from the template, a
// permanent one a copy of the real one, read from the renderer's objects by id), stand-in storage (an object's data,
// its two wave forms, a polygon's arrays), the predicted answers. False when it can't be answered (*why): the traversal
// then joins. The renderer's fields a stand-in gets ids of its own for (its data's, its wave forms') aren't merged
// into the real object.
bool Fail(JoinWhy* why, JoinWhy w) {
    *why = w;
    return false;
}
// one of this traversal's stand-in objects
bool IsStandIn(uint64_t p) {
    const uint64_t s = uint64_t(uintptr_t(g.standIns));
    return g.standIns && p >= s && p < s + uint64_t(kStandIns) * kObjectBytes;
}
// (below: the record so far made, the traversal's calls made from there on; atEnd: at the traversal's end, not counted
// as a join)
// (startedAt: when the flush thread's join that calls it began, the gate's; 0: now)
void Join(JoinWhy why, bool atEnd = false, uint64_t startedAt = 0);
uint64_t MapToReal(uint64_t word);  // (a stand-in the 3D engine still has: its real object, what it wrote merged in)
template <CallKind K>
bool Answer(uint64_t self, const uint64_t* a, Call& c, uint64_t predictedObject, bool predictedGS, LightRec& light,
            PolyRec& poly, uint64_t* v, JoinWhy* why) {
    *v = 0;
    if (c.unknownObject) {  // (what it is: a stand-in of the frame before's set, of this one's, or else)
        static int logged = 0;
        if (++logged <= 8) {
            const int arg = kSlots[K].objectArg;
            const uint64_t o = arg == kSelf ? self : a[arg];
            const auto in = [o](const uint8_t* set) {
                return set && o >= uint64_t(uintptr_t(set)) && o < uint64_t(uintptr_t(set)) + kStandInSetBytes;
            };
            Log("traversal record: on: %s given an object it didn't hand out: 0x%llX (%s), frame %llu, call %zu",
                kCallNames[K], o,
                in(g.standInSets[g.standInSet ^ 1]) ? "a stand-in of the frame before's"
                : in(g.standInSets[g.standInSet])   ? "a stand-in of this frame's (not in the record)"
                                                    : "not a stand-in",
                g.st.frames, g.calls.size());
        }
        return Fail(why, kJoinUnknownObject);
    }
    if (K == kPolygon) {  // (arrays the record couldn't copy)
        const int32_t points = int32_t(a[1]), indices = int32_t(a[6]), size = *g.vertexSize;
        if (points < 0 || points > kMaxPolyPoints || indices < 0 || indices > kMaxPolyPoints || size <= 0 ||
            size > kMaxVertexBytes)
            return Fail(why, kJoinOther);
    }
    if (K == kStartEf) return true;  // (its level, which the traversal reads through EF_Query(19), answered: 1)
    if (K == kGetObject) {
        const bool temp = (a[0] & 0xFF) != 0;
        const int32_t id = int32_t(a[1]);
        if (g.learnedObjects + g.objectsAnswered >= kFrameObjects) return true;  // (as the renderer: none left)
        if (!temp && id == 0) return Fail(why, kJoinNewPermanent);
        if (!temp && id < 0) return Fail(why, kJoinOther);
        if (!temp) {  // (a permanent one: the real one, which its render node keeps and passes on itself; registered
                      // in the frame's list when the record is made)
            const TArray* const all = Member(self, uint32_t(kRAllObjects));
            if (!all->elems || uint32_t(id) >= all->count || !reinterpret_cast<void**>(all->elems)[id])
                return Fail(why, kJoinPermanentId);
            ++g.objectsAnswered;
            *v = uint64_t(uintptr_t(reinterpret_cast<void**>(all->elems)[id]));
            return true;
        }
        if (g.standInsUsed >= kStandIns) return Fail(why, kJoinNoRoom);
        uint8_t* const s = g.standIns + size_t(g.standInsUsed) * kObjectBytes;
        std::memcpy(s, g.objectTemplate, kObjectBytes);
        ++g.standInsUsed;
        ++g.st.onStandIns;
        const uint16_t visible = uint16_t(g.learnedObjects + g.objectsAnswered++);
        std::memcpy(s + kObjectIds + 2, &visible, 2);
        *v = uint64_t(uintptr_t(s));
        return true;
    }
    if (K == kGetObjData || K == kAddWaves) {
        if (c.object == kNone) return Fail(why, kJoinUnknownObject);
        if (!IsStandIn(self)) return Fail(why, kJoinRendererData);  // (a real one: its ids are the renderer's)
        Attach& at = g.attach[c.object];
        uint8_t* const o = reinterpret_cast<uint8_t*>(uintptr_t(self));
        if (K == kGetObjData) {
            if (!at.at[0]) {
                if (At<int16_t>(reinterpret_cast<const char*>(o), kObjDataId) >= 0)
                    return Fail(why, kJoinRendererData);  // (the renderer's data, from before)
                if (g.standInDataUsed >= kStandInData) return Fail(why, kJoinNoRoom);
                uint8_t* const d = g.standInData + size_t(g.standInDataUsed) * kDataBytes;
                std::memset(d, 0, kDataBytes);  // (and the pointer GetObjData resets)
                std::memcpy(d + g.dataDefaultAt, &g.dataDefault, sizeof(void*));
                const int16_t fake = int16_t(g.standInDataUsed++ & 0x3FFF);
                std::memcpy(o + kObjDataId, &fake, 2);
                at.at[0] = d;
            }
            *v = uint64_t(uintptr_t(at.at[0]));
            return true;
        }
        if (!a[0]) return Fail(why, kJoinOther);
        if (!at.at[1]) {
            if (At<uint16_t>(reinterpret_cast<const char*>(o), kWaveId) != 0) return Fail(why, kJoinRendererData);
            if (g.standInWavesUsed + 2 > kStandInWaves) return Fail(why, kJoinNoRoom);
            at.at[1] = g.standInWaves + size_t(g.standInWavesUsed) * kWaveBytes;
            at.at[2] = at.at[1] + kWaveBytes;
            std::memset(at.at[1], 0, 2 * kWaveBytes);  // (as AddWaves starts them)
            std::memcpy(at.at[1], &kWaveStartType, 4);
            std::memcpy(at.at[2], &kWaveStartType, 4);
            const uint16_t fake = uint16_t(0x8000 | (g.standInWavesUsed & 0x7FFF));
            g.standInWavesUsed += 2;
            std::memcpy(o + kWaveId, &fake, 2);
        }
        std::memcpy(reinterpret_cast<void*>(uintptr_t(a[0])), &at.at[1], 8);
        std::memcpy(reinterpret_cast<uint8_t*>(uintptr_t(a[0])) + 8, &at.at[2], 8);
        return true;
    }
    if (K == kFogContrib) {
        *v = PredictFog();
        return true;
    }
    if (K == kParticles) {
        if (a[5]) *reinterpret_cast<uint8_t*>(uintptr_t(a[5])) = uint8_t(predictedGS);
        return true;
    }
    if (K == kPolygon) {
        *v = predictedObject;
        return true;
    }
    if (K == kPolygonOut) {  // (the storage it hands out, the stand-ins' own: its offsets kept in the record)
        const int32_t points = int32_t(a[2]), indices = int32_t(a[3]), size = *g.vertexSize;
        if (points < 0 || points > kMaxPolyPoints || indices < 0 || indices > kMaxPolyPoints || size <= 0 ||
            size > kMaxVertexBytes)
            return Fail(why, kJoinOther);
        const size_t vb = size_t(points) * size_t(size), tb = size_t(points) * kTangentBytes, ib = size_t(indices) * 2;
        const auto room = [](size_t n) { return (n + 15) & ~size_t(15); };
        if (g.standInStorageUsed + room(vb) + room(tb) + room(ib) > kStandInStorage) return Fail(why, kJoinNoRoom);
        uint8_t* const p = g.standInStorage + g.standInStorageUsed;
        poly.verts = uint32_t(g.standInStorageUsed);
        poly.tangs = uint32_t(g.standInStorageUsed + room(vb));
        poly.inds = uint32_t(g.standInStorageUsed + room(vb) + room(tb));
        g.standInStorageUsed += room(vb) + room(tb) + room(ib);
        uint8_t* const out[3] = {p, p + room(vb), p + room(vb) + room(tb)};
        for (int k = 0; k < 3; ++k)
            if (a[4 + k]) std::memcpy(reinterpret_cast<void*>(uintptr_t(a[4 + k])), &out[k], 8);
        *v = predictedObject;
        return true;
    }
    if (K == kAddLight) {
        if (!a[0]) return true;  // (the renderer warns and does nothing: so does the record's call)
        if (OnStack(a[0])) return Fail(why, kJoinOther);  // (a temporary light: gone by the flush's entry)
        const int32_t id = PredictLightId(reinterpret_cast<const char*>(uintptr_t(a[0])));
        std::memcpy(reinterpret_cast<void*>(uintptr_t(a[0])), &id, 4);
        light.id = id;
        light.added = id >= 0 ? 1 : 0;
        if (id >= 0) {  // (where EF_Query finds it)
            if (g.lightsNow.size() <= size_t(id)) g.lightsNow.resize(size_t(id) + 1, nullptr);
            g.lightsNow[size_t(id)] = reinterpret_cast<void*>(uintptr_t(a[0]));
        }
        return true;
    }
    (void)self;
    (void)c;
    return true;  // (EF_AddEf, Render, AddRenderElements, AddRE: void)
}

template <CallKind K>
uint64_t Hook(uint64_t self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
              uint64_t a8, uint64_t a9) {
    const Fn10 f = g.orig[K];
    if (K == kAddEf) RenderQueue_NoteAddEf(reinterpret_cast<void*>(a1));  // (the queue's ocean observer)
    if (GetCurrentThreadId() != g.main || g.depth > 0) return f(self, a1, a2, a3, a4, a5, a6, a7, a8, a9);
    // (a mesh the flush will draw: in the traversal, at its entry; the render meshes' updates measured, not beside)
    if ((K == kRender || K == kAddRenderElements || K == kAddRE) && g.recording && !g.beside)
        g.drawn[g.drawnNow].insert(reinterpret_cast<void*>(self));
    if (K == kStartEf) {
        // (the traversal's start: its level 0, or beside the flush, whose level that is, the first since the last
        // flush's entry)
        if (g.beside && g.onReady && !g.recording && *g.t.level == 0 && !FlushThread_Beside()) {
            ++g.st.besideNone;  // (no flush beside it, the update joined it: its calls made, not recorded)
            g.madeStart = __rdtsc();
        } else if (g.beside && g.onReady && !g.recording && FlushThread_Beside() && FlushThread_Ended()) {
            FlushThread_JoinNow();  // (the flush beside it has ended: joined now, with no wait, as without the record)
            ++g.st.besideEnded;
            g.madeStart = __rdtsc();
        } else if (FlushThread_Beside() ? !g.recording : *g.t.level == 0) {
            g.drawnNow ^= 1;  // (the meshes the last frame drew are the other set's)
            g.drawn[g.drawnNow].clear();
            g.updated.clear();
            ++g.st.meshFrames;
            g.traversalStart = __rdtsc();
            g.firstSettingAt = g.firstRebuildAt = 0;
            Begin();  // the main traversal's level opens
        } else if (g.recording && g.answering) {
            Join(kJoinLevel);  // (a level inside the traversal: made, as the rest of it)
        } else if (g.recording && !g.joined) {
            g.broken = true;  // (a level inside the traversal: its calls aren't the traversal's own)
        }
    }
    if (!g.recording) return f(self, a1, a2, a3, a4, a5, a6, a7, a8, a9);
    // (beside, the flush beside it ended: nothing to wait for any more, so the record is made now, and the calls from
    // here on)
    if (g.answering && g.beside && FlushThread_Ended()) Join(kJoinFlushEnded);
    if (g.joined) {  // (on, after the traversal joined: made, the stand-ins the 3D engine passes their real objects)
        if (g.objectIndex.empty()) return f(self, a1, a2, a3, a4, a5, a6, a7, a8, a9);  // (none handed out)
        uint64_t s = self, w[kWords] = {a1, a2, a3, a4, a5, a6, a7, a8, a9};
        const int arg = kSlots[K].objectArg;
        if (arg == kSelf) s = MapToReal(s);
        if (arg >= 0) w[arg] = MapToReal(w[arg]);
        return f(s, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]);
    }
    const uint64_t t0 = __rdtsc();
    Call c;
    c.kind = K;
    c.self = self;
    const uint64_t a[kWords] = {a1, a2, a3, a4, a5, a6, a7, a8, a9};
    std::memcpy(c.a, a, sizeof a);
    const int arg = kSlots[K].objectArg;
    void* const obj = arg == kSelf ? reinterpret_cast<void*>(self)
                      : arg >= 0   ? reinterpret_cast<void*>(a[arg])
                                   : nullptr;
    if (obj) {
        auto it = g.objectIndex.find(obj);
        if (it == g.objectIndex.end()) {
            c.unknownObject = true;
            ++g.st.unknownObjects[K];
        } else {
            c.object = it->second;
        }
        c.before = Snap(obj, c.object != kNone ? &g.attach[c.object] : nullptr);
    }
    if (K == kAddEf && a2) c.copy = Copy(reinterpret_cast<const void*>(a2), kShaderItemBytes);
    if (K == kRender && OnStack(a1)) c.copy = CopyParams(reinterpret_cast<const uint8_t*>(a1));
    if (K == kFogContrib && a1) c.copy = Copy(reinterpret_cast<const void*>(a1), kColorBytes);
    if (K == kAddWaves && a1) c.copy = Copy(reinterpret_cast<const void*>(a1), kWavesArg);
    if (K == kParticles) {
        if (a1) c.copy = Copy(reinterpret_cast<const void*>(a1), kShaderItemBytes);
        if (OnStack(a4)) {  // (the render info, on the caller's stack: its bytes there)
            uint8_t info[kRenInfoBytes] = {};
            std::memcpy(info, reinterpret_cast<const void*>(a4), (std::min)(kRenInfoBytes, size_t(g.stackHigh - a4)));
            c.copy2 = Copy(info, kRenInfoBytes);
        }
        if (a6) c.flagBefore = *reinterpret_cast<const uint8_t*>(a6);
    }
    PolyRec poly;
    uint32_t polygons = 0;
    if (K == kPolygon || K == kPolygonOut) {
        if (a1) c.copy = Copy(reinterpret_cast<const void*>(a1), kShaderItemBytes);
        const int32_t size = *g.vertexSize;
        if (size <= 0 || size > kMaxVertexBytes) g.broken = true;  // (not replayed)
        if (K == kPolygon) {  // (the arrays, on the caller's stack: their bytes)
            const int32_t points = int32_t(a2), indices = int32_t(a7);
            const bool sane = points >= 0 && points <= kMaxPolyPoints && size > 0 && size <= kMaxVertexBytes;
            if (!sane || indices < 0 || indices > kMaxPolyPoints) g.broken = true;
            const size_t n = sane ? size_t(points) : 0;
            if (a3) poly.verts = Copy(reinterpret_cast<const void*>(a3), n * size_t(sane ? size : 0));
            if (a4) poly.tangs = Copy(reinterpret_cast<const void*>(a4), n * kTangentBytes);
            if (a6 && indices > 0 && indices <= kMaxPolyPoints)
                poly.inds = Copy(reinterpret_cast<const void*>(a6), size_t(indices) * 2);
        }
        poly.level = g.answering ? 1u : uint32_t(*g.t.level);  // (answering: the traversal's level, 1)
        if (poly.level >= kPolyLevels) {
            g.broken = true;
            poly.level = 0;
        }
        polygons = Polygons(poly.level)->count;
        poly.vert0 = Member(self, g.vertexPool)->count;
        poly.index0 = Member(self, g.indexPool)->count;
        poly.objects0 = Member(self, g.polyObjects)->count;
    }
    // step 5's answers, predicted before the call (see above)
    uint64_t predictedObject = 0;
    bool predictedGS = false;
    if (K == kPolygon && g.predicting) predictedObject = PredictPolygonObject(a5, (a9 & 0xFF) != 0);
    if (K == kPolygonOut && g.predicting) predictedObject = PredictPolygonObject(a2, (a9 & 0xFF) != 0);
    if (K == kParticles && a6 && g.predicting) predictedGS = PredictCanUseGS(a1, c.flagBefore != 0);
    LightRec light;
    if (K == kAddLight && a1) {
        c.copy = Copy(reinterpret_cast<const void*>(a1), kLightBytes);
        light.level = g.answering ? 1u : uint32_t(*g.t.level);
        if (light.level >= kLightLevels) {
            g.broken = true;
            light.level = 0;
        }
        light.count0 = Lights(self, light.level)->count;
    }
    uint32_t counts[sp::kLists];
    Counts(counts);
    g.st.recordCycles += __rdtsc() - t0;
    uint64_t v = 0;
    if (g.answering) {  // (on: not made, answered; else the traversal joins and the call is made)
        JoinWhy why = kJoinOther;
        if (!Answer<K>(self, a, c, predictedObject, predictedGS, light, poly, &v, &why)) {
            Join(why);
            uint64_t s = self, w[kWords];
            std::memcpy(w, a, sizeof w);
            if (arg == kSelf) s = MapToReal(s);
            if (arg >= 0) w[arg] = MapToReal(w[arg]);
            return f(s, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]);
        }
    } else {
        ++g.depth;
        v = f(self, a1, a2, a3, a4, a5, a6, a7, a8, a9);
        --g.depth;
    }
    const uint64_t t1 = __rdtsc();
    c.result = v;
    if (K == kParticles && a6) c.flagAfter = *reinterpret_cast<const uint8_t*>(a6);
    // ... and checked against what the call answered (verify; on, the record's calls are checked when made)
    if ((K == kPolygon || K == kPolygonOut) && g.predicting && !g.answering) {
        const uint64_t given = K == kPolygon ? a5 : a2;
        ++g.st.predPoly;
        if (predictedObject != given) ++g.st.predPolyMerged;
        if (predictedObject != v) {
            ++g.st.predPolyDiffer;
            LogPredict("a polygon's object (with merge)", predictedObject, v);
            if (g_predictLogged <= kLoggedDiffer) {  // (what the merge saw: the lists, the fields that agree)
                const auto at = [](uint64_t p) {
                    const auto it = std::find(g.polyPredicted.begin(), g.polyPredicted.end(),
                                              reinterpret_cast<void*>(uintptr_t(p)));
                    return it == g.polyPredicted.end() ? -1 : int(it - g.polyPredicted.begin());
                };
                const auto same = [](uint64_t a, uint64_t b) {
                    unsigned m = 0, k = 0;
                    if (!a || !b) return 0u;
                    const char* const p = reinterpret_cast<const char*>(uintptr_t(a));
                    const char* const q = reinterpret_cast<const char*>(uintptr_t(b));
                    for (size_t o : kMergeWords) m |= (At<int32_t>(p, o) == At<int32_t>(q, o) ? 1u : 0u) << k++;
                    for (size_t o : kMergeFloats) {
                        const float x = At<float>(p, o), y = At<float>(q, o);
                        m |= (!(x < y || x > y) ? 1u : 0u) << k++;
                    }
                    m |= (p[kMergeByte] == q[kMergeByte] ? 1u : 0u) << k;
                    return m;
                };
                const TArray* const real = Member(self, g.polyObjects);
                int realAt = -1, firstApart = -1;
                for (uint32_t i = 0; real->elems && i < real->count; ++i) {
                    void* const e = reinterpret_cast<void**>(real->elems)[i];
                    if (e == reinterpret_cast<void*>(uintptr_t(v))) realAt = int(i);
                    if (firstApart < 0 && (i >= g.polyPredicted.size() || g.polyPredicted[i] != e)) firstApart = int(i);
                }
                Log("traversal record: the merge: given 0x%llX (merge %d), the lists: predicted %zu, real %u (apart "
                    "from %d); the answer at %d in the prediction's, %d in the real one, the prediction at %d; the "
                    "fields the given and the answer share 0x%X (words, the floats, then +0xCC), the given and the "
                    "prediction 0x%X",
                    given, int(a9 & 0xFF), g.polyPredicted.size(), real->count, firstApart, at(v), realAt,
                    at(predictedObject), same(given, v), same(given, predictedObject));
            }
        }
    }
    if (K == kParticles && a6 && g.predicting && !g.answering) {
        ++g.st.predParticles;
        if (uint8_t(predictedGS) != c.flagAfter) {
            ++g.st.predParticlesDiffer;
            LogPredict("a particle call's bool&", predictedGS, c.flagAfter);
        }
    }
    if (K == kFogContrib && g.predicting && !g.answering) {
        const uint16_t predicted = PredictFog();
        ++g.st.predFog;
        if (predicted != uint16_t(v)) {
            ++g.st.predFogDiffer;
            LogPredict("a fog contribution's index", predicted, uint16_t(v));
        }
    }
    if (K == kPolygon || K == kPolygonOut) {
        poly.poly = Polygons(poly.level)->count > polygons ? polygons : kNone;
        poly.vertAdded = Member(self, g.vertexPool)->count - poly.vert0;
        poly.indexAdded = Member(self, g.indexPool)->count - poly.index0;
        poly.objectsAdded = Member(self, g.polyObjects)->count - poly.objects0;
        c.copy2 = Copy(&poly, sizeof poly);
    }
    if (K == kAddLight && a1 && g.answering) c.copy2 = Copy(&light, sizeof light);  // (on: what it was answered)
    if (K == kAddLight && a1 && !g.answering) {  // (its index, its coronas' objects and their contents after it)
        const uint8_t* const l = reinterpret_cast<const uint8_t*>(a1);
        light.added = Lights(self, light.level)->count - light.count0;
        std::memcpy(&light.id, l, 4);
        if (light.added && light.id >= 0) {  // (where EF_Query finds it)
            if (g.lightsNow.size() <= size_t(light.id)) g.lightsNow.resize(size_t(light.id) + 1, nullptr);
            g.lightsNow[size_t(light.id)] = reinterpret_cast<void*>(uintptr_t(a1));
        }
        const int32_t predicted = g.predicting ? PredictLightId(reinterpret_cast<const char*>(l)) : light.id;
        if (g.predicting) ++g.st.predLight;
        if (predicted != light.id) {
            ++g.st.predLightDiffer;
            LogPredict("a light's index", uint32_t(predicted), uint32_t(light.id));
        }
        std::memcpy(light.objects, l + g.lightObjects + light.level * kLightLevelBytes, kLightLevelBytes);
        light.objectsAt = uint32_t(g.bytes.size());
        g.bytes.resize(g.bytes.size() + kLightObjects * kObjectBytes, 0);
        for (uint32_t k = 0; k < kLightObjects; ++k) {
            const char* const o = reinterpret_cast<const char*>(uintptr_t(light.objects[k]));
            if (!o || At<uint64_t>(o, g.lightPointer) != a1) continue;
            light.touched = uint8_t(light.touched | (1u << k));
            std::memcpy(g.bytes.data() + light.objectsAt + k * kObjectBytes, o, kObjectBytes);
        }
        c.copy2 = Copy(&light, sizeof light);
    }
    c.items = uint32_t(g.items.size());
    Added(counts, g.items);
    c.itemCount = uint32_t(g.items.size()) - c.items;
    if (!g.answering) {
        g.st.itemsByKind[K] += c.itemCount;
        if (!c.itemCount) ++g.st.emptyByKind[K];
    }
    for (size_t i = c.items; i < g.items.size(); ++i) {
        char* const o = g.items[i].item.obj;
        if (o && g.objectIndex.find(o) == g.objectIndex.end()) g.items[i].objCopy = Copy(o, kObjectBytes);
    }
    if (c.object != kNone) {  // (what the call handed out for its object)
        Attach& at = g.attach[c.object];
        if (K == kGetObjData && v) at.at[0] = reinterpret_cast<uint8_t*>(v);
        if (K == kAddWaves && a1) {
            std::memcpy(&at.at[1], reinterpret_cast<const void*>(a1), 8);
            std::memcpy(&at.at[2], reinterpret_cast<const uint8_t*>(a1) + 8, 8);
        }
    }
    if (obj) {
        c.after = Snap(obj, c.object != kNone ? &g.attach[c.object] : nullptr);
        const uint8_t* const b = g.bytes.data() + c.before;
        const uint8_t* const e = g.bytes.data() + c.after;
        bool wrote = false;
        for (size_t w = 0; w < kObjectBytes / 8; ++w) {
            if (std::memcmp(b + w * 8, e + w * 8, 8) == 0) continue;
            ++g.st.writeAt[K][w];
            wrote = true;
        }
        if (wrote) ++g.st.objectWrites[K];
    }
    if (K == kGetObject && v) {
        g.objectIndex[reinterpret_cast<void*>(v)] = uint32_t(g.objects.size());
        g.objects.push_back(reinterpret_cast<void*>(v));
        g.attach.emplace_back();
        c.after = Snap(reinterpret_cast<void*>(v), nullptr);  // (the object as it is handed out)
        if ((a1 & 0xFF) && g.predicting && !g.answering) PredictObject(g.bytes.data() + c.after);  // (a temporary one)
    }
    if (K == kStartEf) {
        g.renderer = reinterpret_cast<char*>(uintptr_t(self));
        if (g.answering) {  // (on: what the last real EF_StartEf left)
            g.fogBase = g.learnedFog;
            g.batchStart = g.learnedBatch;
            g.freeStart = g.learnedFree;
            g.polyObjectsStart = g.learnedPolyObjects;
            g.lightStart = g.learnedLights;
            g.lightsNow = g.learnedLightEntries;
        } else {
            g.fogBase = FogCount(self);
            g.batchStart = reinterpret_cast<const TArray*>(uintptr_t(self) + g.batchList)->count;
            g.freeStart = reinterpret_cast<const TArray*>(uintptr_t(self) + g.freeList)->count;
            g.polyObjectsStart = Member(self, g.polyObjects)->count;
            const int lv = *g.t.level;
            const TArray* const lights = lv >= 0 && uint32_t(lv) < kLightLevels ? Lights(self, uint32_t(lv)) : nullptr;
            g.lightStart = lights ? lights->count : 0;
            g.lightsNow.assign(lights && lights->elems ? reinterpret_cast<void**>(lights->elems) : nullptr,
                               lights && lights->elems ? reinterpret_cast<void**>(lights->elems) + lights->count
                                                       : nullptr);
            g.learnedLightEntries = g.lightsNow;
            g.learnedObjects = *reinterpret_cast<const int*>(uintptr_t(self) + kRObjectCount);
        }
        PredictStart();
    }
    g.calls.push_back(c);
    ++g.st.calls[K];
    g.st.recordCycles += __rdtsc() - t1;
    return v;
}
// a render mesh's chunks' elements, each to f (false from it: stop, and false)
template <typename F>
bool EachElement(const char* mesh, F f) {
    const char* const chunks = *reinterpret_cast<const char* const*>(mesh + kMeshChunks);
    if (!chunks) return true;
    const char* const elems = *reinterpret_cast<const char* const*>(chunks);
    const int32_t count = *reinterpret_cast<const int32_t*>(chunks + 8);
    for (int32_t i = 0; i < count; ++i) {
        const char* const re = *reinterpret_cast<const char* const*>(elems + size_t(i) * kChunkBytes + kChunkElement);
        if (re && !f(re)) return false;
    }
    return true;
}
// whether a mesh setting (SetRECustomData, SetMaterial: see kMeshSkipOf) would leave the mesh and its elements as they
// are (the custom data: as the flush's after-step puts it back)
bool MeshSettingChangesNothing(int m, uint64_t self, uint64_t a1, uint64_t a2) {
    const char* const mesh = reinterpret_cast<const char*>(uintptr_t(self));
    if (m == kMeshSetMaterial) {
        if (*reinterpret_cast<const uint64_t*>(mesh + kMeshMaterial) != a1) return false;
        if (uint32_t(a2) == 0) return true;  // (no texture id: the elements left alone)
        return EachElement(mesh, [a2](const char* re) {
            return *reinterpret_cast<const uint32_t*>(re + kElementTexBind) == uint32_t(a2);
        });
    }
    const char* const data = reinterpret_cast<const char*>(uintptr_t(a1));
    return EachElement(mesh, [data](const char* re) {
        return EngineData_Original(*reinterpret_cast<const char* const*>(re + kElementCustomData)) == data;
    });
}
// a render mesh's update: counted when it is the traversal's own (outermost), with whether its mesh is one the last
// frame drew
template <int M>
uint64_t MeshHook(uint64_t self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                  uint64_t a7, uint64_t a8, uint64_t a9) {
    const bool mine = GetCurrentThreadId() == g.main;
    // (beside, while the record answers: a setting passed the gate; not made if it changes nothing, else joined)
    constexpr int skip = M == kMeshSetCustomData ? 0 : M == kMeshSetMaterial ? 1 : -1;
    if constexpr (skip >= 0) {
        if (mine && g.answering && FlushThread_Beside()) {
            if (MeshSettingChangesNothing(M, self, a1, a2)) {
                ++g.st.meshSkipped[skip];
                return *reinterpret_cast<const uint64_t*>(uintptr_t(self) + kMeshChunks);
            }
            ++g.st.meshChanged[skip];
            Join(kJoinMeshSetting);
        }
    }
    if (mine && g.recording && !g.beside) {      // (measured, not beside)
        if (g.depth > 0 || g.meshDepth > 0) {  // (within a producer, or another update)
            ++g.st.meshNested[M];
        } else {
            void* const mesh = reinterpret_cast<void*>(self);
            const bool drawn = g.drawn[g.drawnNow ^ 1].count(mesh) != 0;
            ++g.st.meshCalls[M];
            if (drawn) {
                ++g.st.meshOnDrawn[M];
                uint64_t& first = kMeshSetting[M] ? g.firstSettingAt : g.firstRebuildAt;
                if (!first) first = __rdtsc();
            }
            if (g.updated.insert(mesh).second) {
                ++g.st.meshesUpdated;
                if (drawn) ++g.st.meshesUpdatedDrawn;
            }
        }
    }
    if (!mine) return g.meshOrig[M](self, a1, a2, a3, a4, a5, a6, a7, a8, a9);
    ++g.meshDepth;
    const uint64_t v = g.meshOrig[M](self, a1, a2, a3, a4, a5, a6, a7, a8, a9);
    --g.meshDepth;
    return v;
}
// a render mesh's other slot: the traversal's own calls (not a producer's or an update's), and those on a mesh it
// updated earlier. Integer work only before it goes on, which leaves float arguments in their registers.
template <size_t N>
uint64_t MeshReadHook(uint64_t self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                      uint64_t a7, uint64_t a8, uint64_t a9) {
    if (__readgsdword(0x48) == g.main && g.recording && !g.beside && g.depth == 0 && g.meshDepth == 0) {
        ++g.st.meshReads[N];
        if (g.updated.find(reinterpret_cast<void*>(self)) != g.updated.end()) ++g.st.meshReadsAfter[N];
    }
    return g.meshNext[N](self, a1, a2, a3, a4, a5, a6, a7, a8, a9);
}
// EF_Query: the traversal's queries counted by their type; its light by index answered from the lights it was
// answered (on: the renderer's list isn't the frame's yet), or checked against that (verify)
uint64_t QueryHook(uint64_t self, uint64_t type, uint64_t param, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                   uint64_t a7, uint64_t a8, uint64_t a9) {
    if (__readgsdword(0x48) == g.main && g.recording && g.depth == 0 && !g.joined) {
        const uint32_t t = uint32_t(type);
        ++g.st.queries[t < kQueryTypes ? t : kQueryTypes - 1];
        if (t == kQueryLevel && g.answering) return 1;  // (the traversal's level: EF_StartEf, answered, opened it)
        if (t == kQueryLight) {
            const uint32_t index = uint32_t(param);
            const uint64_t emulated = index < g.lightsNow.size() ? uint64_t(uintptr_t(g.lightsNow[index])) : 0;
            if (g.answering) {
                ++g.st.queryLightAnswered;
                return emulated;
            }
            const uint64_t v = g.queryNext(self, type, param, a3, a4, a5, a6, a7, a8, a9);
            ++g.st.queryLightChecked;
            if (v != emulated) {
                ++g.st.queryLightDiffer;
                LogPredict("EF_Query's light by index", emulated, v);
            }
            return v;
        }
    }
    return g.queryNext(self, type, param, a3, a4, a5, a6, a7, a8, a9);
}
template <size_t... I>
constexpr std::array<Fn10, sizeof...(I)> MeshReadHooks(std::index_sequence<I...>) {
    return {&MeshReadHook<I>...};
}
const std::array<Fn10, kMeshSlots> kMeshReadHooks = MeshReadHooks(std::make_index_sequence<kMeshSlots>{});
const Fn10 kMeshHooks[] = {&MeshHook<0>, &MeshHook<1>, &MeshHook<2>, &MeshHook<3>, &MeshHook<4>, &MeshHook<5>,
                           &MeshHook<6>, &MeshHook<7>, &MeshHook<8>, &MeshHook<9>, &MeshHook<10>};
static_assert(sizeof kMeshHooks / sizeof kMeshHooks[0] == kMeshUpdates, "a hook per mesh update");

const Fn10 kHooks[] = {&Hook<kStartEf>,    &Hook<kGetObject>, &Hook<kAddEf>,
                       &Hook<kRender>,     &Hook<kAddRenderElements>, &Hook<kAddRE>,
                       &Hook<kFogContrib>, &Hook<kAddWaves>,  &Hook<kGetObjData>,
                       &Hook<kParticles>,  &Hook<kPolygon>,   &Hook<kPolygonOut>,
                       &Hook<kAddLight>};
static_assert(sizeof kHooks / sizeof kHooks[0] == kCallKinds, "a hook per kind");

// the object an item points at, in the record's terms: its index (kNone: not one the record's EF_GetObject returned)
uint32_t RecordIndex(const char* obj) {
    auto it = g.objectIndex.find(const_cast<char*>(obj));
    return it == g.objectIndex.end() ? kNone : it->second;
}

void LogDiffer(const Call& c, size_t i, const char* what, uint64_t va, uint64_t vb) {
    if (++g.logged > kLoggedDiffer) return;
    Log("traversal record: VERIFY difference #%d, frame %llu, call %zu (%s): %s: 0x%llX recorded, 0x%llX replayed",
        g.logged, g.st.frames, i, kCallNames[c.kind], what, va, vb);
}

// before a replayed call: what the 3D engine changed in the object's state since it last saw it
// (on: a real object for its stand-in also keeps the fields the renderer manages, its data's id and its wave forms',
// which the stand-in has its own of)
void Merge(char* object, const Attach& ra, const uint8_t* rec, const uint8_t* seen, bool on = false) {
    for (size_t o = 0; o < kObjectBytes; ++o) {
        if (rec[o] == seen[o] || (o >= kObjectIds && o < (on ? kObjectOwnEnd : kObjectIdsEnd))) continue;
        if (on && o >= kWaveId && o < kWaveId + 2) continue;
        object[o] = char(rec[o]);
    }
    for (size_t k = 0; k < kAttachments; ++k) {
        if (!ra.at[k] || !(rec[kStateMask] & (1u << k))) continue;
        const bool had = (seen[kStateMask] & (1u << k)) != 0;
        for (size_t o = 0; o < kAttachBytes[k]; ++o) {
            const uint8_t v = rec[kAttachOffset[k] + o];
            if (!had || v != seen[kAttachOffset[k] + o]) ra.at[k][o] = v;
        }
    }
}

// one replayed call's items against the recorded call's (objects mapped); its object's state after it
bool CompareCall(const Call& c, size_t i, const Item* got, uint32_t n, const char* object, const Attach* ra) {
    g.st.items += c.itemCount;
    bool same = true;
    if (n != c.itemCount) {
        LogDiffer(c, i, "the items added", c.itemCount, n);
        g.st.itemsDiffer += c.itemCount > n ? c.itemCount : n;
        same = false;
    } else {
        for (uint32_t k = 0; k < n; ++k) {
            const Item& a = g.items[c.items + k];
            const Item& b = got[k];
            const char* what = nullptr;
            uint64_t va = 0, vb = 0;
            if (a.list != b.list) what = "an item's list", va = a.list, vb = b.list;
            else if (a.item.sortVal != b.item.sortVal) what = "an item's sort value", va = a.item.sortVal, vb = b.item.sortVal;
            else if (a.item.elem != b.item.elem &&
                     !(c.kind == kParticles && a.item.elem && b.item.elem && SameParticle(a.item.elem, b.item.elem)))
                what = "an item's element", va = uintptr_t(a.item.elem), vb = uintptr_t(b.item.elem);
            else if (a.item.objSort != b.item.objSort) what = "an item's object sort key", va = a.item.objSort, vb = b.item.objSort;
            else if (a.item.dynLM != b.item.dynLM) what = "an item's light mask", va = a.item.dynLM, vb = b.item.dynLM;
            else if (a.item.batchFlags != b.item.batchFlags) what = "an item's batch flags", va = a.item.batchFlags, vb = b.item.batchFlags;
            else if (a.item.obj != b.item.obj) {
                const uint32_t ia = RecordIndex(a.item.obj);
                if (ia != kNone) {  // (one the record's EF_GetObject returned: the replay's in its place)
                    if (ia >= g.replayObjects.size() || g.replayObjects[ia] != b.item.obj)
                        what = "an item's object", va = uintptr_t(a.item.obj), vb = uintptr_t(b.item.obj);
                } else if (a.objCopy == kNone || !b.item.obj ||  // (one the call took itself: by its contents)
                           std::memcmp(g.bytes.data() + a.objCopy + kObjectCompareFrom, b.item.obj + kObjectCompareFrom,
                                       kObjectBytes - kObjectCompareFrom) != 0) {
                    what = "an item's own object", va = uintptr_t(a.item.obj), vb = uintptr_t(b.item.obj);
                } else {
                    ++g.st.ownObjects;
                }
            }
            if (what) {
                LogDiffer(c, i, what, va, vb);
                ++g.st.itemsDiffer;
                same = false;
                break;
            }
            if (a.item.elem != b.item.elem) ++g.st.particlesByContents;
        }
    }
    if (object && c.after != kNone) {
        ++g.st.objectsCompared;
        const uint8_t* const rec = g.bytes.data() + c.after;
        for (size_t o = kObjectCompareFrom; o < kObjectBytes; ++o) {
            if (rec[o] == uint8_t(object[o]) || (o >= kWaveId && o < kWaveId + 2)) continue;
            ++g.st.objectsDiffer;
            uint64_t va = 0, vb = 0;
            std::memcpy(&va, rec + (o & ~size_t(7)), 8);
            std::memcpy(&vb, object + (o & ~size_t(7)), 8);
            char what[48];
            snprintf(what, sizeof what, "its object after it at +0x%zX", o);
            LogDiffer(c, i, what, va, vb);
            same = false;
            break;
        }
        for (size_t k = 0; ra && k < kAttachments; ++k) {
            if (!ra->at[k] || !(rec[kStateMask] & (1u << k))) continue;
            ++g.st.attachCompared;
            if (std::memcmp(rec + kAttachOffset[k], ra->at[k], kAttachBytes[k]) == 0) continue;
            ++g.st.objectsDiffer;
            uint64_t va = 0, vb = 0;
            std::memcpy(&va, rec + kAttachOffset[k], 8);
            std::memcpy(&vb, ra->at[k], 8);
            LogDiffer(c, i, k ? "a wave form handed out for its object" : "its object's data", va, vb);
            same = false;
        }
    }
    return same;
}

// a polygon's object (or a polygon call's result) in the replay against the recorded one's: for the call's own
// object the replay's in its place, for one the record's EF_GetObject returned the replay's (the merge answers an
// earlier one), any other the same
bool SameObject(uint64_t recorded, uint64_t replayed, uint64_t recordedOwn, uint64_t replayedOwn) {
    if (recorded == recordedOwn) return replayed == replayedOwn;
    const uint32_t ia = RecordIndex(reinterpret_cast<const char*>(uintptr_t(recorded)));
    if (ia == kNone) return recorded == replayed;
    return ia < g.replayObjects.size() && uint64_t(uintptr_t(g.replayObjects[ia])) == replayed;
}

// slot 91: what the 3D engine wrote into the storage the recorded call handed out (in the pools, at its polygon's
// offsets, as the flush will find it) written into what the replayed call handed out
void HandOut(const PolyRec& p, uint8_t* const* out, const TArray* vertexPool, const TArray* indexPool) {
    const char* const rec = PolygonAt(p.level, p.poly);
    const size_t points = At<uint16_t>(rec, kPolyVertsAt), indices = At<uint16_t>(rec, kPolyIndicesAt);
    const int32_t tangents = At<int32_t>(rec, kPolyTangOffsAt);
    if (out[0])
        std::memcpy(out[0], vertexPool->elems + At<uint32_t>(rec, kPolyVertOffsAt), points * size_t(*g.vertexSize));
    if (out[1] && tangents >= 0) std::memcpy(out[1], vertexPool->elems + tangents, points * kTangentBytes);
    if (out[2]) std::memcpy(out[2], indexPool->elems + size_t(At<uint32_t>(rec, kPolyIndexOffsAt)) * 2, indices * 2);
}

// a replayed polygon call against the recorded one: its result, what it added (a polygon, the pools' bytes, an entry
// in the polygon objects' list), and the polygon (its fields, its offsets from where each call's began, its vertices,
// tangents and indices)
bool ComparePolygon(const Call& c, size_t i, const PolyRec& p, uint32_t level, const uint32_t* before, uint64_t result,
                    const uint64_t* a, const TArray* vertexPool, const TArray* indexPool, const TArray* polyObjects) {
    const int arg = kSlots[c.kind].objectArg;
    const uint64_t own = c.a[arg], replayOwn = a[arg];
    bool same = true;
    ++g.st.resultsCompared;
    if (c.result != own) ++g.st.polyMerged;
    if (!SameObject(c.result, result, own, replayOwn)) {
        LogDiffer(c, i, "its result (its polygon's object)", c.result, result);
        same = false;
    }
    const struct {
        const char* what;
        uint32_t recorded, replayed;
    } added[] = {{"the polygons it added", p.poly != kNone ? 1u : 0u, Polygons(level)->count - before[0]},
                 {"the vertex pool's bytes it added", p.vertAdded, vertexPool->count - before[1]},
                 {"the indices it added", p.indexAdded, indexPool->count - before[2]},
                 {"the polygon objects it listed", p.objectsAdded, polyObjects->count - before[3]}};
    for (const auto& k : added) {
        if (k.recorded == k.replayed) continue;
        LogDiffer(c, i, k.what, k.recorded, k.replayed);
        same = false;
    }
    if (p.objectsAdded == 1 && same && p.objects0 < g.polyObjectsSaved.size() &&
        !SameObject(uint64_t(uintptr_t(g.polyObjectsSaved[p.objects0])),
                    uint64_t(uintptr_t(reinterpret_cast<void**>(polyObjects->elems)[before[3]])), own, replayOwn)) {
        LogDiffer(c, i, "the polygon object it listed", uint64_t(uintptr_t(g.polyObjectsSaved[p.objects0])),
                  uint64_t(uintptr_t(reinterpret_cast<void**>(polyObjects->elems)[before[3]])));
        same = false;
    }
    if (!same || p.poly == kNone) return same;
    const char* const rec = PolygonAt(p.level, p.poly);
    const char* const rep = PolygonAt(level, before[0]);
    ++g.st.polysCompared;
    const int32_t recTangents = At<int32_t>(rec, kPolyTangOffsAt), repTangents = At<int32_t>(rep, kPolyTangOffsAt);
    const char* what = nullptr;
    size_t field = 0;
    if (std::memcmp(rec + kPolyItemAt, rep + kPolyItemAt, kShaderItemBytes) != 0)
        what = "its shader item", field = kPolyItemAt;
    else if (!SameObject(At<uint64_t>(rec, kPolyObjectAt), At<uint64_t>(rep, kPolyObjectAt), own, replayOwn))
        what = "its object", field = kPolyObjectAt;
    else if (At<uint32_t>(rec, kPolyVertsAt) != At<uint32_t>(rep, kPolyVertsAt) || rec[kPolyAWAt] != rep[kPolyAWAt])
        what = "its counts or nAW", field = kPolyVertsAt;
    else if (At<uint32_t>(rec, kPolyVertOffsAt) - p.vert0 != At<uint32_t>(rep, kPolyVertOffsAt) - before[1])
        what = "its vertices' offset", field = kPolyVertOffsAt;
    else if ((recTangents < 0) != (repTangents < 0) ||
             (recTangents >= 0 && uint32_t(recTangents) - p.vert0 != uint32_t(repTangents) - before[1]))
        what = "its tangents' offset", field = kPolyTangOffsAt;
    else if (At<uint32_t>(rec, kPolyIndexOffsAt) - p.index0 != At<uint32_t>(rep, kPolyIndexOffsAt) - before[2])
        what = "its indices' offset", field = kPolyIndexOffsAt;
    if (what) {
        LogDiffer(c, i, what, At<uint64_t>(rec, field), At<uint64_t>(rep, field));
        return false;
    }
    const size_t points = At<uint16_t>(rec, kPolyVertsAt), indices = At<uint16_t>(rec, kPolyIndicesAt);
    const size_t vertexBytes = points * size_t(*g.vertexSize), tangentBytes = recTangents >= 0 ? points * kTangentBytes : 0;
    const uint8_t* const pool = vertexPool->elems;
    const uint8_t* const indexElems = indexPool->elems;
    if (std::memcmp(pool + At<uint32_t>(rec, kPolyVertOffsAt), pool + At<uint32_t>(rep, kPolyVertOffsAt), vertexBytes))
        what = "its vertices";
    else if (tangentBytes && std::memcmp(pool + recTangents, pool + repTangents, tangentBytes))
        what = "its tangents";
    else if (std::memcmp(indexElems + size_t(At<uint32_t>(rec, kPolyIndexOffsAt)) * 2,
                         indexElems + size_t(At<uint32_t>(rep, kPolyIndexOffsAt)) * 2, indices * 2))
        what = "its indices";
    g.st.polyBytes += vertexBytes + tangentBytes + indices * 2;
    if (what) {
        LogDiffer(c, i, what, points, indices);
        return false;
    }
    return true;
}

// a replayed light call against the recorded one: what it added to its level's list (its copy, where the recorded
// light went), the index it gave the light, and its coronas' objects after it (from +0x10; their m_pLight the copy
// where the recorded call's was the recorded light)
bool CompareLight(const Call& c, size_t i, const LightRec& lr, const uint8_t* copy, const TArray* lights,
                  uint32_t lights0) {
    ++g.st.lightsCompared;
    const uint32_t added = lights->count - lights0;
    if (added != lr.added) {
        LogDiffer(c, i, "the lights it added to its level's list", lr.added, added);
        return false;
    }
    bool same = true;
    const void* const entry = added == 1 ? reinterpret_cast<void* const*>(lights->elems)[lights0] : nullptr;
    if (added == 1 && entry != copy) {
        LogDiffer(c, i, "the light it added to its level's list", uintptr_t(copy), uintptr_t(entry));
        same = false;
    }
    const int32_t id = At<int32_t>(reinterpret_cast<const char*>(copy), 0);
    if (id != lr.id) {
        LogDiffer(c, i, "the light's index", uint32_t(lr.id), uint32_t(id));
        same = false;
    }
    for (uint32_t k = 0; k < kLightObjects; ++k) {
        if (!(lr.touched & (1u << k))) continue;
        ++g.st.coronasCompared;
        const uint8_t* const rec = g.bytes.data() + lr.objectsAt + k * kObjectBytes;
        const uint8_t* const now = reinterpret_cast<const uint8_t*>(uintptr_t(lr.objects[k]));
        for (size_t o = kObjectCompareFrom; o < kObjectBytes; ++o) {
            if ((o >= g.lightPointer && o < g.lightPointer + 8) || rec[o] == now[o]) continue;
            char what[64];
            snprintf(what, sizeof what, "a corona's object after it at +0x%zX", o);
            LogDiffer(c, i, what, At<uint64_t>(reinterpret_cast<const char*>(rec), o & ~size_t(7)),
                      At<uint64_t>(reinterpret_cast<const char*>(now), o & ~size_t(7)));
            same = false;
            break;
        }
        const uint64_t recLight = At<uint64_t>(reinterpret_cast<const char*>(rec), g.lightPointer);
        const uint64_t nowLight = At<uint64_t>(reinterpret_cast<const char*>(now), g.lightPointer);
        if (recLight == c.a[0] ? nowLight != uint64_t(uintptr_t(copy)) : nowLight != recLight) {
            LogDiffer(c, i, "a corona's object's m_pLight", recLight, nowLight);
            same = false;
        }
    }
    return same;
}

void Replay(char* r) {
    const uint64_t t0 = __rdtsc();
    const int level = *g.t.level;
    int* const objects = reinterpret_cast<int*>(r + kRObjectCount);
    const int objectsBefore = *objects;
    if (objectsBefore + int(g.objects.size()) + kObjectsMargin > kMaxObjects) {
        ++g.st.noRoom;
        return;
    }
    // what the replay will add to the frame's pools (where a recorded call gave its object data or waves; the fog
    // contributions), within their room
    uint32_t needData = 0, needWaves = 0, needFog = 0;
    for (const Call& c : g.calls) {
        if (c.kind == kFogContrib) ++needFog;
        if (c.before == kNone || c.after == kNone) continue;
        int16_t was, now;
        std::memcpy(&was, g.bytes.data() + c.before + kObjDataId, 2);
        std::memcpy(&now, g.bytes.data() + c.after + kObjDataId, 2);
        needData += was < 0 && now >= 0 ? 1 : 0;
        uint16_t wave;
        std::memcpy(&wave, g.bytes.data() + c.before + kWaveId, 2);
        needWaves += c.kind == kAddWaves && wave == 0 ? 2 : 0;
    }
    char* const dataElems = *g.objData;
    uint32_t dataCount = 0;
    if (dataElems) {
        uint32_t capacity;
        std::memcpy(&dataCount, dataElems - 4, 4);
        std::memcpy(&capacity, dataElems - 8, 4);
        if (dataCount + needData > capacity) needData = kNone;
    }
    if (needData == kNone || (!dataElems && needData)) {
        ++g.st.noDataRoom;
        return;
    }
    const TArray waves = *g.waves;
    if (needWaves && waves.count + needWaves > waves.capacity) {
        ++g.st.noWavesRoom;
        return;
    }
    const Vector fog = *Fog(uint64_t(uintptr_t(r)));
    if (needFog && size_t(fog.capacityEnd - fog.end) < size_t(needFog) * kColorBytes) {
        ++g.st.noFogRoom;
        return;
    }
    size_t needLights = 0;
    for (const Call& c : g.calls) needLights += c.kind == kAddLight ? 1 : 0;
    if (needLights > kReplayLights || level < 0 || uint32_t(level) >= kLightLevels) {
        ++g.st.noLightRoom;
        return;
    }
    const uint32_t fogStart = FogCount(uint64_t(uintptr_t(r)));
    // the particle batch list and the temporary objects' free list as at the traversal's start for the replay (their
    // entries put back after it), and the particle elements' pool's count
    TArray* const batch = reinterpret_cast<TArray*>(r + g.batchList);
    TArray* const freeList = reinterpret_cast<TArray*>(r + g.freeList);
    const uint32_t batchCount = batch->count, freeCount = freeList->count, poolCount = g.particlePool->count;
    g.batchSaved.assign(reinterpret_cast<void**>(batch->elems), reinterpret_cast<void**>(batch->elems) + batchCount);
    g.freeSaved.assign(reinterpret_cast<void**>(freeList->elems),
                       reinterpret_cast<void**>(freeList->elems) + freeCount);
    if (g.batchStart <= batchCount) batch->count = g.batchStart;
    if (g.freeStart <= freeCount) freeList->count = g.freeStart;
    // the polygon objects' list too; the polygons' storage at the level and their pools, cut back after it
    TArray* const polyObjects = Member(uint64_t(uintptr_t(r)), g.polyObjects);
    g.polyObjectsSaved.assign(reinterpret_cast<void**>(polyObjects->elems),
                              reinterpret_cast<void**>(polyObjects->elems) + polyObjects->count);
    if (g.polyObjectsStart <= polyObjects->count) polyObjects->count = g.polyObjectsStart;
    TArray* const vertexPool = Member(uint64_t(uintptr_t(r)), g.vertexPool);
    TArray* const indexPool = Member(uint64_t(uintptr_t(r)), g.indexPool);
    const uint32_t polygons = Polygons(uint32_t(level))->count, vertices = vertexPool->count;
    const uint32_t indices = indexPool->count;
    // the level's light list from its count at the traversal's start (its entries put back after the replay), the
    // lights' corona objects' contents put back after it; the lights' copies on this stack
    TArray* const lights = Lights(uint64_t(uintptr_t(r)), uint32_t(level));
    g.lightsSaved.assign(reinterpret_cast<void**>(lights->elems),
                         reinterpret_cast<void**>(lights->elems) + lights->count);
    if (g.lightStart <= lights->count) lights->count = g.lightStart;
    g.coronas.clear();
    for (const Call& c : g.calls) {
        if (c.kind != kAddLight || c.copy2 == kNone) continue;
        LightRec lr;
        std::memcpy(&lr, g.bytes.data() + c.copy2, sizeof lr);
        for (uint32_t k = 0; k < kLightObjects; ++k) {
            void* const o = reinterpret_cast<void*>(uintptr_t(lr.objects[k]));
            if ((lr.touched & (1u << k)) && std::find(g.coronas.begin(), g.coronas.end(), o) == g.coronas.end())
                g.coronas.push_back(o);
        }
    }
    g.coronasSaved.resize(g.coronas.size() * kObjectBytes);
    for (size_t k = 0; k < g.coronas.size(); ++k)
        std::memcpy(g.coronasSaved.data() + k * kObjectBytes, g.coronas[k], kObjectBytes);
    alignas(16) uint8_t lightCopies[kReplayLights][kLightBytes];
    size_t lightsUsed = 0;
    // the permanent objects (returned again by their id): their contents, put back after the replay
    g.permanentSaved.clear();
    std::vector<void*> permanent;
    for (const Call& c : g.calls)
        if (c.kind == kGetObject && !(c.a[0] & 0xFF) && c.result) permanent.push_back(reinterpret_cast<void*>(c.result));
    for (void* p : permanent) {
        const uint8_t* const b = static_cast<const uint8_t*>(p);
        g.permanentSaved.insert(g.permanentSaved.end(), b, b + kObjectBytes);
    }
    g.replayObjects.clear();
    g.replayAttach.clear();
    g.replaySeen.clear();
    g.replayItems.clear();
    // at the traversal's own level (its items' batch flags follow the level: the main pass's z-pass, ...), after its
    // items, cut back to them afterwards
    uint32_t start[sp::kLists];
    Counts(start);
    for (size_t i = 1; i < g.calls.size(); ++i) {
        const Call& c = g.calls[i];
        uint64_t a[kWords];
        std::memcpy(a, c.a, sizeof a);
        uint64_t self = c.self;
        char* object = nullptr;
        Attach* ra = nullptr;
        const int arg = kSlots[c.kind].objectArg;
        if (arg != kNoObject && (arg == kSelf ? c.self : a[arg])) {
            if (c.unknownObject || c.object >= g.replayObjects.size() || !g.replayObjects[c.object])
                continue;  // (an object the replay doesn't have: not replayed)
            object = static_cast<char*>(g.replayObjects[c.object]);
            ra = &g.replayAttach[c.object];
            Merge(object, *ra, g.bytes.data() + c.before, g.bytes.data() + g.replaySeen[c.object]);
            if (arg == kSelf)
                self = uint64_t(uintptr_t(object));
            else
                a[arg] = uint64_t(uintptr_t(object));
        }
        uint8_t params[kParamsBytes];
        uint8_t wavesOut[kWavesArg];
        if (c.kind == kAddEf && c.copy != kNone) a[1] = uint64_t(uintptr_t(g.bytes.data() + c.copy));
        if (c.kind == kRender && c.copy != kNone) {
            ReplayParams(c.copy, params);
            a[0] = uint64_t(uintptr_t(params));
        }
        if (c.kind == kFogContrib && c.copy != kNone) a[0] = uint64_t(uintptr_t(g.bytes.data() + c.copy));
        if (c.kind == kAddWaves && c.copy != kNone) {
            std::memcpy(wavesOut, g.bytes.data() + c.copy, kWavesArg);
            a[0] = uint64_t(uintptr_t(wavesOut));
        }
        uint64_t flag = c.flagBefore;  // (EF_AddParticlesToScene's bool&)
        if (c.kind == kParticles) {
            if (c.copy != kNone) a[0] = uint64_t(uintptr_t(g.bytes.data() + c.copy));
            if (c.copy2 != kNone) a[3] = uint64_t(uintptr_t(g.bytes.data() + c.copy2));
            if (a[5]) a[5] = uint64_t(uintptr_t(&flag));
        }
        // a polygon: the copies of its shader item and arrays (slot 92), or what it hands out into the replay's own
        // pointers (slot 91); the counts before it
        const bool polygon = c.kind == kPolygon || c.kind == kPolygonOut;
        PolyRec poly;
        uint8_t* handedOut[3] = {};
        uint32_t polyBefore[4] = {};
        if (polygon) {
            std::memcpy(&poly, g.bytes.data() + c.copy2, sizeof poly);
            if (c.copy != kNone) a[0] = uint64_t(uintptr_t(g.bytes.data() + c.copy));
            if (c.kind == kPolygon) {
                if (poly.verts != kNone) a[2] = uint64_t(uintptr_t(g.bytes.data() + poly.verts));
                if (poly.tangs != kNone) a[3] = uint64_t(uintptr_t(g.bytes.data() + poly.tangs));
                if (poly.inds != kNone) a[5] = uint64_t(uintptr_t(g.bytes.data() + poly.inds));
            } else {
                for (int k = 0; k < 3; ++k)
                    if (a[4 + k]) a[4 + k] = uint64_t(uintptr_t(&handedOut[k]));
            }
            polyBefore[0] = Polygons(uint32_t(level))->count;
            polyBefore[1] = vertexPool->count;
            polyBefore[2] = indexPool->count;
            polyBefore[3] = polyObjects->count;
        }
        // a light: its copy as the recorded call got it, with the objects the recorded call left in it (it may have
        // made them: the replay takes them again by their id)
        uint8_t* lightCopy = nullptr;
        LightRec lr;
        uint32_t lights0 = 0;
        if (c.kind == kAddLight && c.copy != kNone) {
            std::memcpy(&lr, g.bytes.data() + c.copy2, sizeof lr);
            lightCopy = lightCopies[lightsUsed++];
            std::memcpy(lightCopy, g.bytes.data() + c.copy, kLightBytes);
            std::memcpy(lightCopy + g.lightObjects + lr.level * kLightLevelBytes, lr.objects, kLightLevelBytes);
            a[0] = uint64_t(uintptr_t(lightCopy));
            lights0 = lights->count;
        }
        uint32_t counts[sp::kLists];
        Counts(counts);
        const size_t from = g.replayItems.size();
        const uint64_t v = g.orig[c.kind](self, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
        Added(counts, g.replayItems);
        ++g.st.replayed[c.kind];
        if (c.kind == kPolygonOut && poly.poly != kNone) HandOut(poly, handedOut, vertexPool, indexPool);
        if (ra) {  // (what the replayed call handed out for its object)
            if (c.kind == kGetObjData && v) ra->at[0] = reinterpret_cast<uint8_t*>(v);
            if (c.kind == kAddWaves) {
                std::memcpy(&ra->at[1], wavesOut, 8);
                std::memcpy(&ra->at[2], wavesOut + 8, 8);
            }
            // what it handed out new starts as the recorded call's did (GetObjData doesn't set all of its data: the
            // rest is what the array's slot held, another one in the replay)
            const uint8_t* const before = g.bytes.data() + c.before;
            const uint8_t* const after = g.bytes.data() + c.after;
            for (size_t k = 0; k < kAttachments; ++k)
                if (ra->at[k] && (after[kStateMask] & (1u << k)) && !(before[kStateMask] & (1u << k)))
                    std::memcpy(ra->at[k], after + kAttachOffset[k], kAttachBytes[k]);
        }
        if (c.kind == kGetObject && c.result) {  // (one of the record's objects: as the 3D engine got it, the record's
                                                 // copy then, but the replay's indices)
            char* const o = reinterpret_cast<char*>(v);
            if (o) {
                const uint8_t* const rec = g.bytes.data() + c.after;
                std::memcpy(o, rec, kObjectIds);
                std::memcpy(o + kObjectIdsEnd, rec + kObjectIdsEnd, kObjectBytes - kObjectIdsEnd);
            }
            g.replayObjects.push_back(o);  // (null: the calls given it aren't replayed)
            g.replayAttach.emplace_back();
            g.replaySeen.push_back(c.after);
        } else if (object) {
            g.replaySeen[c.object] = c.after;  // (what the 3D engine finds in it after the call)
        }
        bool same = CompareCall(c, i, g.replayItems.data() + from, uint32_t(g.replayItems.size() - from), object, ra);
        if (c.kind == kFogContrib) {  // (its index, from where each run's contributions began)
            ++g.st.resultsCompared;
            if (uint16_t(v - fogStart) != uint16_t(c.result - g.fogBase)) {
                LogDiffer(c, i, "the contribution's index from the first", uint16_t(c.result - g.fogBase),
                          uint16_t(v - fogStart));
                same = false;
            }
        }
        if (c.kind == kParticles && c.a[5]) {  // (what it answered through its bool&)
            ++g.st.resultsCompared;
            if (uint8_t(flag) != c.flagAfter) {
                LogDiffer(c, i, "its bool& result", c.flagAfter, uint8_t(flag));
                same = false;
            }
        }
        if (polygon &&
            !ComparePolygon(c, i, poly, uint32_t(level), polyBefore, v, a, vertexPool, indexPool, polyObjects))
            same = false;
        if (lightCopy && !CompareLight(c, i, lr, lightCopy, lights, lights0)) same = false;
        if (!same) ++g.st.callsDiffer;
    }
    // the replay's items, objects and pools' entries dropped (the flush gets the frame as the traversal left it)
    sp::ItemList* const lists = Lists();
    for (int k = 0; k < sp::kLists; ++k)
        if (lists[k].count > start[k]) lists[k].count = start[k];
    *g.t.level = level;
    *objects = objectsBefore;
    if (dataElems && *g.objData == dataElems)
        std::memcpy(dataElems - 4, &dataCount, 4);
    else if (*g.objData != dataElems)
        ++g.st.poolsMoved;
    if (g.waves->elems == waves.elems)
        g.waves->count = waves.count;
    else
        ++g.st.poolsMoved;
    Vector* const fogNow = Fog(uint64_t(uintptr_t(r)));
    if (fogNow->begin == fog.begin)
        fogNow->end = fog.end;
    else
        ++g.st.poolsMoved;
    // the polygons: the storage's count at the level, the pools' (the polygons the replay took from the storage, or
    // made, are used again next frame)
    if (Polygons(uint32_t(level))->count > polygons) Polygons(uint32_t(level))->count = polygons;
    if (vertexPool->count > vertices) vertexPool->count = vertices;
    if (indexPool->count > indices) indexPool->count = indices;
    // the lights' corona objects (their m_pLight at the copies on this stack until now)
    for (size_t k = 0; k < g.coronas.size(); ++k)
        std::memcpy(g.coronas[k], g.coronasSaved.data() + k * kObjectBytes, kObjectBytes);
    // the particle lists' entries put back (the batch list's index in the particle fix rebuilt from them), the polygon
    // objects' and the level's lights'; the particle pool's elements the replay used, used again next frame
    for (auto [list, saved] : {std::pair{batch, &g.batchSaved}, std::pair{freeList, &g.freeSaved},
                               std::pair{polyObjects, &g.polyObjectsSaved}, std::pair{lights, &g.lightsSaved}}) {
        if (list->capacity < saved->size()) {
            ++g.st.poolsMoved;
            continue;
        }
        if (!saved->empty()) std::memcpy(list->elems, saved->data(), saved->size() * sizeof(void*));
        list->count = uint32_t(saved->size());
    }
    g.particlePool->count = poolCount;
    ParticleBatch_ResetIndex();
    for (size_t i = 0; i < permanent.size(); ++i)
        std::memcpy(permanent[i], g.permanentSaved.data() + i * kObjectBytes, kObjectBytes);
    ++g.st.verified;
    g.st.replayCycles += __rdtsc() - t0;
}

// --- on: the record made (step 5.2) -------------------------------------------------------------------------------
// what a real EF_StartEf left, for the next traversal's answers
void Learn(char* r) {
    const uint64_t rr = uint64_t(uintptr_t(r));
    g.learnedFog = FogCount(rr);
    g.learnedBatch = Member(rr, g.batchList)->count;
    g.learnedFree = Member(rr, g.freeList)->count;
    g.learnedPolyObjects = Member(rr, g.polyObjects)->count;
    const int level = *g.t.level;
    const TArray* const lights = level >= 0 && uint32_t(level) < kLightLevels ? Lights(rr, uint32_t(level)) : nullptr;
    g.learnedLights = lights ? lights->count : 0;
    g.learnedLightEntries.assign(lights && lights->elems ? reinterpret_cast<void**>(lights->elems) : nullptr,
                                 lights && lights->elems ? reinterpret_cast<void**>(lights->elems) + lights->count
                                                         : nullptr);
    g.learnedObjects = *reinterpret_cast<const int*>(r + kRObjectCount);
}

// each stand-in's state now merged into its real object (what the 3D engine wrote since its last call, or since the
// join)
void FinalMerge() {
    const size_t n = (std::min)(g.objects.size(), g.replayObjects.size());
    g.snapNow.resize(kStateBytes);
    for (size_t i = 0; i < n; ++i) {
        char* const real = static_cast<char*>(g.replayObjects[i]);
        if (!real || real == g.objects[i]) continue;
        SnapTo(g.snapNow.data(), g.objects[i], &g.attach[i]);
        Merge(real, g.replayAttach[i], g.snapNow.data(), g.bytes.data() + g.replaySeen[i], true);
        g.replaySeen[i] = Snap(g.objects[i], &g.attach[i]);
        ++g.st.onFinalMerges;
    }
}

// the record made for real: EF_StartEf first, each call with its stand-ins' real objects (what the 3D engine wrote
// into a stand-in merged in before each call, the renderer's fields of the real one kept), the storage the 3D engine
// filled copied into what the real call hands out, the answers it was given checked; then the final merge
void ReplayOn(char* r) {
    const uint64_t t0 = __rdtsc();
    g.replayObjects.clear();
    g.replayAttach.clear();
    g.replaySeen.clear();
    // the record's real objects as they are now: each call's merge takes one back to its state then, and the 3D
    // engine's writes after its last call are put back at the end
    g.liveSaved.clear();
    g.liveIndex.clear();
    for (size_t k = 0; k < g.objects.size(); ++k) {
        if (IsStandIn(uint64_t(uintptr_t(g.objects[k])))) continue;
        g.liveIndex.push_back(uint32_t(k));
        const size_t at = g.liveSaved.size();
        g.liveSaved.resize(at + kStateBytes);
        SnapTo(g.liveSaved.data() + at, g.objects[k], &g.attach[k]);
    }
    // ... each taken back to its state when it was handed out (the replay's merges then go through what the 3D
    // engine wrote in order)
    {
        uint32_t n = 0;
        size_t next = 0;
        std::vector<void*> restored;  // (an object handed out twice: from the first time)
        for (const Call& c : g.calls) {
            if (c.kind != kGetObject || !c.result) continue;
            if (next < g.liveIndex.size() && g.liveIndex[next] == n) {
                if (std::find(restored.begin(), restored.end(), g.objects[n]) == restored.end()) {
                    std::memcpy(g.objects[n], g.bytes.data() + c.after, kObjectBytes);
                    restored.push_back(g.objects[n]);
                }
                ++next;
            }
            ++n;
        }
    }
    bool firstTemp = true;
    for (size_t i = 0; i < g.calls.size(); ++i) {
        const Call& c = g.calls[i];
        uint64_t a[kWords];
        std::memcpy(a, c.a, sizeof a);
        uint64_t self = c.self;
        char* object = nullptr;
        Attach* ra = nullptr;
        const int arg = kSlots[c.kind].objectArg;
        if (arg != kNoObject && (arg == kSelf ? c.self : a[arg])) {
            if (c.unknownObject || c.object >= g.replayObjects.size() || !g.replayObjects[c.object]) {
                ++g.st.onSkipped;  // (an object the renderer had none for: the 3D engine got none either)
                continue;
            }
            object = static_cast<char*>(g.replayObjects[c.object]);
            ra = &g.replayAttach[c.object];
            Merge(object, *ra, g.bytes.data() + c.before, g.bytes.data() + g.replaySeen[c.object], true);
            if (arg == kSelf)
                self = uint64_t(uintptr_t(object));
            else
                a[arg] = uint64_t(uintptr_t(object));
        }
        uint8_t params[kParamsBytes];
        uint8_t wavesOut[kWavesArg] = {};
        if (c.kind == kAddEf && c.copy != kNone) a[1] = uint64_t(uintptr_t(g.bytes.data() + c.copy));
        if (c.kind == kRender && c.copy != kNone) {
            ReplayParams(c.copy, params);
            a[0] = uint64_t(uintptr_t(params));
        }
        if (c.kind == kFogContrib && c.copy != kNone) a[0] = uint64_t(uintptr_t(g.bytes.data() + c.copy));
        if (c.kind == kAddWaves && a[0]) a[0] = uint64_t(uintptr_t(wavesOut));
        uint64_t flag = c.flagBefore;
        if (c.kind == kParticles) {
            if (c.copy != kNone) a[0] = uint64_t(uintptr_t(g.bytes.data() + c.copy));
            if (c.copy2 != kNone) a[3] = uint64_t(uintptr_t(g.bytes.data() + c.copy2));
            if (a[5]) a[5] = uint64_t(uintptr_t(&flag));
        }
        PolyRec poly;
        uint8_t* handedOut[3] = {};
        if (c.kind == kPolygon || c.kind == kPolygonOut) {
            std::memcpy(&poly, g.bytes.data() + c.copy2, sizeof poly);
            if (c.copy != kNone) a[0] = uint64_t(uintptr_t(g.bytes.data() + c.copy));
            if (c.kind == kPolygon) {
                if (poly.verts != kNone) a[2] = uint64_t(uintptr_t(g.bytes.data() + poly.verts));
                if (poly.tangs != kNone) a[3] = uint64_t(uintptr_t(g.bytes.data() + poly.tangs));
                if (poly.inds != kNone) a[5] = uint64_t(uintptr_t(g.bytes.data() + poly.inds));
            } else {
                for (int k = 0; k < 3; ++k)
                    if (a[4 + k]) a[4 + k] = uint64_t(uintptr_t(&handedOut[k]));
            }
        }
        LightRec lr;
        if (c.kind == kAddLight && c.copy2 != kNone) std::memcpy(&lr, g.bytes.data() + c.copy2, sizeof lr);
        uint32_t listsBefore[sp::kLists];
        Counts(listsBefore);
        const uint64_t v = g.orig[c.kind](self, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
        ++g.st.onReplayed;
        {  // (the items it added)
            uint32_t listsAfter[sp::kLists];
            Counts(listsAfter);
            uint64_t n = 0;
            for (int k = 0; k < sp::kLists; ++k) n += listsAfter[k] > listsBefore[k] ? listsAfter[k] - listsBefore[k] : 0;
            g.st.itemsByKind[c.kind] += n;
            if (!n) ++g.st.emptyByKind[c.kind];
        }
        if (c.kind == kStartEf) Learn(r);
        if (ra) {  // (what the real call handed out for the object, started as its stand-in's was; the 3D engine's
                   // writes into the stand-in's come with the next merge)
            if (c.kind == kGetObjData && v) ra->at[0] = reinterpret_cast<uint8_t*>(v);
            if (c.kind == kAddWaves) {
                std::memcpy(&ra->at[1], wavesOut, 8);
                std::memcpy(&ra->at[2], wavesOut + 8, 8);
            }
            const uint8_t* const before = g.bytes.data() + c.before;
            const uint8_t* const after = g.bytes.data() + c.after;
            for (size_t k = 0; k < kAttachments; ++k)
                if (ra->at[k] && (after[kStateMask] & (1u << k)) && !(before[kStateMask] & (1u << k)))
                    std::memcpy(ra->at[k], after + kAttachOffset[k], kAttachBytes[k]);
        }
        if (c.kind == kPolygonOut && handedOut[0]) {  // (what the 3D engine wrote into the stand-ins' storage)
            const size_t points = size_t(int32_t(c.a[2])), indices = size_t(int32_t(c.a[3]));
            if (handedOut[0]) std::memcpy(handedOut[0], g.standInStorage + poly.verts, points * size_t(*g.vertexSize));
            if (handedOut[1]) std::memcpy(handedOut[1], g.standInStorage + poly.tangs, points * kTangentBytes);
            if (handedOut[2]) std::memcpy(handedOut[2], g.standInStorage + poly.inds, indices * 2);
        }
        if (c.kind == kGetObject) {
            char* const o = reinterpret_cast<char*>(v);
            if (o && IsStandIn(c.result)) {  // (it starts as its stand-in did, but its ids: what the 3D engine wrote
                                             // into the stand-in then shows in each merge, a value it wrote equal to
                                             // what was there included)
                const uint8_t* const rec = g.bytes.data() + c.after;
                std::memcpy(o, rec, kObjectIds);
                std::memcpy(o + kObjectIdsEnd, rec + kObjectIdsEnd, kObjectBytes - kObjectIdsEnd);
            }
            if (c.result) {  // (one of the record's objects, as the record has them)
                g.replayObjects.push_back(o);
                g.replayAttach.emplace_back();
                g.replaySeen.push_back(c.after);  // (the stand-in as the 3D engine got it)
            }
            if (o && (c.a[0] & 0xFF) && firstTemp) {  // (the template for the next traversal's)
                std::memcpy(g.nextTemplate, o, kObjectBytes);
                g.haveNext = true;
                firstTemp = false;
            }
            ++g.st.onChecked;
            if ((o != nullptr) != (c.result != 0)) {
                ++g.st.onDiffer;
                LogDiffer(c, i, "an object given (0: none)", c.result, v);
            }
        } else if (object) {
            g.replaySeen[c.object] = c.after;
        }
        // the answers the 3D engine was given
        const char* what = nullptr;
        uint64_t given = 0, real = 0;
        if (c.kind == kFogContrib) {
            what = "the fog contribution's index", given = uint16_t(c.result), real = uint16_t(v);
        } else if (c.kind == kAddLight && c.a[0] && c.copy2 != kNone) {
            what = "the light's index", given = uint32_t(lr.id);
            real = uint32_t(At<int32_t>(reinterpret_cast<const char*>(uintptr_t(c.a[0])), 0));
        } else if (c.kind == kPolygon || c.kind == kPolygonOut) {
            what = "the polygon's object", given = c.result, real = v;
            if (SameObject(c.result, v, c.a[arg], a[arg])) real = given;
        } else if (c.kind == kParticles && c.a[5]) {
            what = "the particle call's bool&", given = c.flagAfter, real = uint8_t(flag);
        }
        if (what) {
            ++g.st.onChecked;
            if (given != real) {
                ++g.st.onDiffer;
                LogDiffer(c, i, what, given, real);
            }
        }
    }
    for (size_t k = 0; k < g.liveIndex.size(); ++k) {  // (the real objects: the 3D engine's writes after their last call)
        const uint32_t i = g.liveIndex[k];
        if (i >= g.replayObjects.size() || !g.replayObjects[i]) continue;
        Merge(static_cast<char*>(g.replayObjects[i]), g.replayAttach[i], g.liveSaved.data() + k * kStateBytes,
              g.bytes.data() + g.replaySeen[i], true);
        g.replaySeen[i] = Snap(g.objects[i], &g.attach[i]);
    }
    FinalMerge();
    g.st.replayCycles += __rdtsc() - t0;
}

void Join(JoinWhy why, bool atEnd, uint64_t startedAt) {
    if (g.joined || g.joiningNow) return;
    g.joiningNow = true;
    const uint64_t t0 = startedAt ? startedAt : __rdtsc();
    if (FlushThread_Beside()) FlushThread_JoinNow();  // (beside the flush: it ends first, with its steps after it)
    const uint64_t t1 = __rdtsc();
    if (!atEnd) {
        ++g.st.onJoins;
        ++g.st.onJoinWhy[why];
        if (g.joinWhy < 0) {
            g.joinWhy = int(why);
            g.joinAt = t0;
        }
    }
    g.answering = false;
    ReplayOn(g.renderer);
    g.joined = true;
    g.joiningNow = false;
    if (g.beside && !atEnd) {
        const uint64_t t2 = __rdtsc();
        ++g.st.timedJoins;
        g.st.answeredCycles += t0 > g.traversalStart ? t0 - g.traversalStart : 0;
        g.st.joinFlushCycles += t1 - t0;
        g.st.joinRecordCycles += t2 - t1;
        g.joinedAt = t2;
    }
    // (beside, joined inside the traversal: one in kObserveEvery observed for the rest of it, the gate kept on the main
    // thread with no flush beside it: the flush thread's callback tells each call that would have joined)
    if (g.observe && !atEnd && ++g.observeJoins % kObserveEvery == 0) {
        g.observing = true;
        memset(g.observeFirst, 0, sizeof g.observeFirst);
        RenderGate_Set(g.main);
    }
}

uint64_t MapToReal(uint64_t word) {
    if (!IsStandIn(word)) return word;  // (a real object, or none: as it is, with no lookup)
    const auto it = g.objectIndex.find(reinterpret_cast<void*>(uintptr_t(word)));
    if (it == g.objectIndex.end()) return word;
    const uint32_t i = it->second;
    if (i >= g.replayObjects.size() || !g.replayObjects[i]) return word;
    char* const real = static_cast<char*>(g.replayObjects[i]);
    if (real == g.objects[i]) return word;
    g.snapNow.resize(kStateBytes);
    SnapTo(g.snapNow.data(), g.objects[i], &g.attach[i]);
    Merge(real, g.replayAttach[i], g.snapNow.data(), g.bytes.data() + g.replaySeen[i], true);
    g.replaySeen[i] = Snap(g.objects[i], &g.attach[i]);
    return uint64_t(uintptr_t(real));
}

// a module global's address from a rip-relative operand: the rel32 at `at` + 3 of a 7-byte instruction at `at`
char* RipTarget(const char* fn, size_t at) {
    int32_t rel;
    std::memcpy(&rel, fn + at + 3, 4);
    return const_cast<char*>(fn + at + 7 + rel);
}
#endif  // _WIN64

}  // namespace

void TraversalRecord_Install(const wchar_t* moduleName, char* base, const ShadowPrepassTarget& t) {
#ifdef _WIN64
    const std::wstring mode =
        ConfigString(L"ShadowPrepass", L"FlushThreadRecord", L"OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD_RECORD", L"off");
    if (_wcsicmp(mode.c_str(), L"verify") != 0 && _wcsicmp(mode.c_str(), L"on") != 0 &&
        _wcsicmp(mode.c_str(), L"beside") != 0)
        return;
    g.beside = _wcsicmp(mode.c_str(), L"beside") == 0;
    g.on = g.beside || _wcsicmp(mode.c_str(), L"on") == 0;
    g.observe = g.beside && _wcsicmp(ConfigString(L"ShadowPrepass", L"FlushThreadRecordObserve",
                                                  L"OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD_RECORD_OBSERVE", L"off")
                                         .c_str(),
                                     L"on") == 0;
    if (RenderGate_Slots() == 0) {
        Log("%ls: traversal record: not installed (it needs FlushThread=overlap: the gate's redirects)", moduleName);
        return;
    }
    if (!t.level || !t.lists || !t.starts) {
        Log("%ls: traversal record: not installed (the shadow pre-pass's lists weren't found)", moduleName);
        return;
    }
    int index[kCallKinds];
    for (int k = 0; k < kCallKinds; ++k) {
        char** const vt = reinterpret_cast<char**>(FindRttiVtable(base, kSlots[k].cls));
        index[k] = vt ? RenderGate_IndexOf(vt, kSlots[k].slot) : -1;
        void* const fn = index[k] >= 0 ? CallTrace_Original(vt[kSlots[k].slot]) : nullptr;
        if (!fn || !IsInCode(base, static_cast<const char*>(fn), 1)) {
            Log("%ls: traversal record: not installed (%s's slot %u)", moduleName, kSlots[k].cls, kSlots[k].slot);
            return;
        }
        g.orig[k] = reinterpret_cast<Fn10>(fn);
    }
    // the frame's pools, from the code that adds to them: the render objects' data (GetObjData's two references to
    // it agree), the wave forms (AddWaves' count), the fog contributions (PushFogVolumeContribution's displacement)
    const char* const getData = reinterpret_cast<const char*>(g.orig[kGetObjData]);
    if (IsInCode(base, getData, kGetObjDataBytes) && MatchesAt(getData, ParsePattern(kGetObjDataCode)) &&
        RipTarget(getData, kObjDataLoadAt) == RipTarget(getData, kObjDataLeaAt))
        g.objData = reinterpret_cast<char**>(RipTarget(getData, kObjDataLoadAt));
    const char* const addWaves = reinterpret_cast<const char*>(g.orig[kAddWaves]);
    if (IsInCode(base, addWaves, kAddWavesBytes) && MatchesAt(addWaves, ParsePattern(kAddWavesCode)))
        g.waves = reinterpret_cast<TArray*>(RipTarget(addWaves, kWavesCountAt) - offsetof(TArray, count));
    const char* const fog = reinterpret_cast<const char*>(g.orig[kFogContrib]);
    if (IsInCode(base, fog, kFogContribBytes) && MatchesAt(fog, ParsePattern(kFogContribCode))) {
        int32_t d;
        std::memcpy(&d, fog + kFogVectorAt, 4);
        g.fogVector = size_t(uint32_t(d));
    }
    if (!g.objData || !g.waves || !g.fogVector) {
        Log("%ls: traversal record: not installed (the code of %s isn't as expected: a frame's pool not found)",
            moduleName, !g.objData ? kCallNames[kGetObjData] : !g.waves ? kCallNames[kAddWaves] : kCallNames[kFogContrib]);
        return;
    }
    // the particles: the batch list and the free list (the particle fix's reading of the merge), the elements' pool
    // (the function EF_AddParticlesToScene calls to take one, among its direct calls, by its code)
    {
        ParticleBatchTarget pb;
        const char* why = nullptr;
        if (ParticleBatch_Installed(&pb) || ParticleBatch_Locate(base, &pb, &why)) {
            g.batchList = pb.listOffset;
            g.freeList = pb.freeListOffset;
        }
        const char* const add = reinterpret_cast<const char*>(g.orig[kParticles]);
        const auto pool = ParsePattern(kParticlePoolCode);
        for (size_t i = 0; IsInCode(base, add, kParticlesScan) && !g.particlePool && i + 5 <= kParticlesScan; ++i) {
            if (uint8_t(add[i]) != 0xE8) continue;
            int32_t rel;
            std::memcpy(&rel, add + i + 1, 4);
            const char* const callee = add + i + 5 + rel;
            if (IsInCode(base, callee, kParticlePoolBytes) && MatchesAt(callee, pool))
                g.particlePool =
                    reinterpret_cast<TArray*>(RipTarget(callee, kParticlePoolCountAt) - offsetof(TArray, count));
        }
        if (!g.batchList || !g.freeList || !g.particlePool) {
            Log("%ls: traversal record: not installed (the particles' %s not found)", moduleName,
                !g.particlePool ? "elements' pool" : "batch list (the particle fix's merge)");
            return;
        }
    }
    // the polygons: the two forms' starts (the merge they call), the merge's list, slot 92's pools and vertex size,
    // its level's storage (the shadow pre-pass's, from EF_StartEf: one of its first rip-relative leas)
    {
        const char* const add = reinterpret_cast<const char*>(g.orig[kPolygon]);
        const char* const out = reinterpret_cast<const char*>(g.orig[kPolygonOut]);
        const char* why = nullptr;
        const char* merge = nullptr;
        if (!t.polyLists || !IsInCode(base, add, kPolyIndicesScan) || !MatchesAt(add, ParsePattern(kPolygonCode)) ||
            !IsInCode(base, out, kPolygonOutBytes) || !MatchesAt(out, ParsePattern(kPolygonOutCode))) {
            why = "their code isn't as expected";
        } else {
            merge = RelTarget(const_cast<char*>(add + kPolygonMergeCallAt));
            if (RelTarget(const_cast<char*>(out + kPolygonOutMergeCallAt)) != merge || !IsInCode(base, merge, kPolyMergeBytes) ||
                !MatchesAt(merge, ParsePattern(kPolyMergeCode)) ||
                ReadU32(merge + kPolyMergeElemsAt) + 8 != ReadU32(merge + kPolyMergeCountAt))
                why = "the polygon objects' list not found";
        }
        bool storage = false;
        for (size_t i = 0; !why && !storage && i + 7 <= kPolygonScan; ++i)  // (a lea r, [rip+d])
            storage = (uint8_t(add[i]) & 0xFB) == 0x48 && uint8_t(add[i + 1]) == 0x8D &&
                      (uint8_t(add[i + 2]) & 0xC7) == 0x05 && RipTarget(add, i) == t.polyLists;
        if (!why && !storage) why = "their level's storage isn't the shadow pre-pass's";
        const std::vector<char*> verts =
            why ? std::vector<char*>{} : FindInRange(const_cast<char*>(add), kPolyVertsScan, ParsePattern(kPolyVertsCode), 2);
        const std::vector<char*> inds =
            why ? std::vector<char*>{} : FindInRange(const_cast<char*>(add), kPolyIndicesScan, ParsePattern(kPolyIndicesCode), 2);
        if (!why && (verts.size() != 1 || inds.size() != 1)) why = "their pools not found";
        if (!why) {
            const char* const v = verts[0];
            const char* const n = inds[0];
            const uint32_t count = ReadU32(v + kPolyVertCountAt), indexCount = ReadU32(n + kPolyIndexCountAt);
            int32_t rel;
            std::memcpy(&rel, v + kPolyVertSizeAt + 2, 4);  // (mov ecx, [rip+d]: 6 bytes)
            const char* const size = v + kPolyVertSizeAt + 6 + rel;
            if (ReadU32(v + kPolyVertCountAt2) != count || ReadU32(v + kPolyVertCapacityAt[0]) != count + 4 ||
                ReadU32(v + kPolyVertCapacityAt[1]) != count + 4 || ReadU32(n + kPolyIndexCountAt2) != indexCount ||
                ReadU32(n + kPolyIndexCapacityAt) != indexCount + 4 || !IsInImage(base, size, 4)) {
                why = "their pools aren't as expected";
            } else {
                g.vertexPool = count - 8;
                g.indexPool = indexCount - 8;
                g.vertexSize = reinterpret_cast<const int32_t*>(size);
                g.polyObjects = ReadU32(merge + kPolyMergeElemsAt);
            }
        }
        if (why) {
            Log("%ls: traversal record: not installed (EF_AddPolygonToScene: %s)", moduleName, why);
            return;
        }
    }
    // the lights: the level's list (the count EF_ADDDlight checks, at the shadow pre-pass's level), the coronas'
    // objects in the light, their m_pLight
    {
        char* const add = reinterpret_cast<char*>(g.orig[kAddLight]);
        const bool code = IsInCode(base, add, kAddLightBodyScan);
        const std::vector<char*> list =
            code ? FindInRange(add, kAddLightScan, ParsePattern(kAddLightListCode), 2) : std::vector<char*>{};
        const std::vector<char*> objects =
            code ? FindInRange(add, kAddLightBodyScan, ParsePattern(kAddLightObjectsCode), 2) : std::vector<char*>{};
        const std::vector<char*> corona =
            code ? FindInRange(add, kAddLightBodyScan, ParsePattern(kAddLightCoronaCode), 2) : std::vector<char*>{};
        const char* why = nullptr;
        if (list.size() != 1 || objects.size() != 1 || corona.size() != 1) {
            why = "its code isn't as expected";
        } else if (RipTarget(list[0], kAddLightLevelAt) != reinterpret_cast<char*>(t.level)) {
            why = "its level isn't the shadow pre-pass's";
        } else {
            g.lightList = ReadU32(list[0] + kAddLightCountAt) - 8;
            g.lightObjects = ReadU32(objects[0] + kAddLightObjectsAt);
            g.lightPointer = ReadU32(corona[0] + kAddLightPointerAt);
            if (g.lightObjects + kLightLevels * kLightLevelBytes > kLightBytes || g.lightPointer + 8 > kObjectBytes)
                why = "its offsets aren't as expected";
        }
        if (why) {
            Log("%ls: traversal record: not installed (EF_ADDDlight: %s)", moduleName, why);
            return;
        }
    }
    // step 5's answers' predictions: EF_IsFakeDLight's test, EF_AddParticlesToScene's flags and the technique lookup
    // it calls (without them the predictions are off)
    const char* predictWhy = nullptr;
    {
        char** const rvt = reinterpret_cast<char**>(FindRttiVtable(base, ".?AVCD3D9Renderer@@"));
        char* const fake = rvt ? static_cast<char*>(CallTrace_Original(rvt[kFakeLightSlot])) : nullptr;
        const std::vector<char*> test = fake && IsInCode(base, fake, kFakeLightScan)
                                            ? FindInRange(fake, kFakeLightScan, ParsePattern(kFakeLightCode), 2)
                                            : std::vector<char*>{};
        char* const add = reinterpret_cast<char*>(g.orig[kParticles]);
        const std::vector<char*> gs = IsInCode(base, add, kParticleGSScan)
                                          ? FindInRange(add, kParticleGSScan, ParsePattern(kParticleGSCode), 2)
                                          : std::vector<char*>{};
        char* const tech = gs.size() == 1 ? RelTarget(gs[0]) : nullptr;
        if (test.size() != 1) {
            predictWhy = "EF_IsFakeDLight's code isn't as expected";
        } else if (gs.size() != 1 || !IsInCode(base, tech, 34) || !MatchesAt(tech, ParsePattern(kTechniqueCode))) {
            predictWhy = "EF_AddParticlesToScene's code isn't as expected";
        } else {
            const uint8_t b0 = uint8_t(test[0][kFakeLightBitAt[0]]), b1 = uint8_t(test[0][kFakeLightBitAt[1]]);
            int32_t rel;
            std::memcpy(&rel, gs[0] + kParticleGSFlagAt + 2, 4);  // (cmp dword [rip+d], 0: 7 bytes)
            const char* const flag = gs[0] + kParticleGSFlagAt + 7 + rel;
            if (b0 > 31 || b1 > 31 || !IsInImage(base, flag, 4)) {
                predictWhy = "their values aren't as expected";
            } else {
                g.lightFlags = ReadU32(test[0] + kFakeLightFlagsAt);
                g.fakeBits = (1u << b0) | (1u << b1);
                g.gsFlag = reinterpret_cast<const int32_t*>(flag);
                g.techIndex = uint8_t(tech[kTechIndexAt]);
                g.techCount = uint8_t(tech[kTechCountAt]);
                g.techArray = uint8_t(tech[kTechArrayAt]);
                g.techFlags = uint8_t(gs[0][kParticleTechFlagsAt]);
                g.techBit = uint8_t(gs[0][kParticleTechBitAt]) & 31;
                g.predicting = true;
            }
        }
    }
    g.t = t;
    g.main = GetCurrentThreadId();
    GetCurrentThreadStackLimits(&g.stackLow, &g.stackHigh);
    g.calls.reserve(16384);
    g.bytes.reserve(size_t(32) << 20);
    g.items.reserve(16384);
    // on: the stand-ins' two sets; it answers with the predictions, and starts a stand-in's data as GetObjData does
    if (g.on) {
        const char* why = !g.predicting ? predictWhy : nullptr;
        char* const objData = reinterpret_cast<char*>(g.orig[kGetObjData]);
        const std::vector<char*> reset = !why && IsInCode(base, objData, kDataDefaultScan)
                                             ? FindInRange(objData, kDataDefaultScan, ParsePattern(kDataDefaultCode), 2)
                                             : std::vector<char*>{};
        if (!why && reset.size() == 1) {
            int32_t rel;
            std::memcpy(&rel, reset[0] + kDataDefaultLeaAt + 3, 4);
            g.dataDefault = reset[0] + kDataDefaultLeaAt + 7 + rel;
            g.dataDefaultAt = uint8_t(reset[0][kDataDefaultFieldAt]);
            if (!IsInImage(base, static_cast<const char*>(g.dataDefault), 8) || g.dataDefaultAt + 8 > kDataBytes)
                why = "GetObjData's default isn't as expected";
        } else if (!why) {
            why = "GetObjData's code isn't as expected";
        }
        for (int s = 0; s < 2 && !why; ++s) {
            g.standInSets[s] = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, kStandInSetBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!g.standInSets[s]) why = "no memory for the stand-ins";
        }
        if (why) {
            Log("%ls: traversal record: not installed (on: %s)", moduleName, why);
            return;
        }
    }
    for (int k = 0; k < kCallKinds; ++k) {
        RenderGate_Redirect(unsigned(index[k]), reinterpret_cast<void*>(kHooks[k]));
        g.gateIndex[k] = unsigned(index[k]);
    }
    // EF_Query: on to what its thunk went to (the render queue's hook); the thunks of the slots made beside the flush
    {
        char** const rvt = reinterpret_cast<char**>(FindRttiVtable(base, ".?AVCD3D9Renderer@@"));
        for (int b = 0; b < kBesides; ++b) {
            const int bx = rvt ? RenderGate_IndexOf(rvt, kBesideSlots[b]) : -1;
            g.besideIndex[b] = bx >= 0 ? unsigned(bx) : ~0u;
        }
        const int ix = rvt ? RenderGate_IndexOf(rvt, kQuerySlot) : -1;
        void* const next = ix >= 0 ? RenderGate_Target(unsigned(ix)) : nullptr;
        if (next) {
            g.queryNext = reinterpret_cast<Fn10>(next);
            RenderGate_Redirect(unsigned(ix), reinterpret_cast<void*>(&QueryHook));
        } else if (g.on) {
            Log("%ls: traversal record: not installed (on: EF_Query's slot)", moduleName);
            return;
        }
    }
    // the render meshes' updates, observed: each on to what its thunk went to (its function, or another part's hook)
    int meshObserved = 0, meshReadSlots = 0;
    if (char** const meshVt = reinterpret_cast<char**>(FindRttiVtable(base, ".?AUCRenderMesh@@"))) {
        for (int m = 0; m < kMeshUpdates; ++m) {
            const int ix = RenderGate_IndexOf(meshVt, kMeshUpdateSlots[m]);
            void* const next = ix >= 0 ? RenderGate_Target(unsigned(ix)) : nullptr;
            if (!next) continue;
            g.meshOrig[m] = reinterpret_cast<Fn10>(next);
            RenderGate_Redirect(unsigned(ix), reinterpret_cast<void*>(kMeshHooks[m]));
            ++meshObserved;
        }
        // (beside: the two settings not made when they change nothing, each if its code is as expected)
        for (int k = 0; k < kMeshSkips; ++k) {
            const unsigned slot = kMeshUpdateSlots[kMeshSkipOf[k]];
            const char* const fn = static_cast<const char*>(CallTrace_Original(meshVt[slot]));
            const Pattern p = ParsePattern(kMeshSkipCode[k]);
            const int ix = RenderGate_IndexOf(meshVt, slot);
            const bool ok = g.meshOrig[kMeshSkipOf[k]] && ix >= 0 && IsInCode(base, fn, p.size()) && MatchesAt(fn, p);
            g.meshSkipIndex[k] = ok ? unsigned(ix) : ~0u;
            if (!ok && g.beside)
                Log("%ls: traversal record: %s joins beside the flush (its code isn't as expected)", moduleName,
                    kMeshUpdateNames[kMeshSkipOf[k]]);
        }
        // its other slots (but the producers'): each on to what was behind it
        for (size_t s = 0; s < kMeshSlots; ++s) {
            bool taken = false;
            for (int k = 0; k < kCallKinds; ++k)
                taken |= std::strcmp(kSlots[k].cls, ".?AUCRenderMesh@@") == 0 && kSlots[k].slot == s;
            for (int m = 0; m < kMeshUpdates; ++m) taken |= kMeshUpdateSlots[m] == s;
            const int ix = taken ? -1 : RenderGate_IndexOf(meshVt, unsigned(s));
            void* const next = ix >= 0 ? RenderGate_Target(unsigned(ix)) : nullptr;
            if (!next) continue;
            g.meshNext[s] = reinterpret_cast<Fn10>(next);
            RenderGate_Redirect(unsigned(ix), reinterpret_cast<void*>(kMeshReadHooks[s]));
            ++meshReadSlots;
        }
    }
    g.installed = true;
    char names[600] = "";
    size_t nu = 0;
    for (int k = 0; k < kCallKinds; ++k) {
        const int w = snprintf(names + nu, sizeof names - nu, "%s%s", k ? ", " : "", kCallNames[k]);
        if (w < 0 || size_t(w) >= sizeof names - nu) break;
        nu += size_t(w);
    }
    Log("%ls: traversal record: %s: the main traversal's producer calls recorded (%d methods: %s) and %s; the "
        "frame's pools: the objects' data +0x%llX, "
        "the wave forms +0x%llX, the fog contributions at the renderer's +0x%zX, the particle elements +0x%llX, the "
        "particle batch list and the free list at the renderer's +0x%X, +0x%X, the polygons' vertex and index pools "
        "and their objects' list at the renderer's +0x%X, +0x%X, +0x%X (the vertex size at +0x%llX), the light lists "
        "at the renderer's +0x%X (a light's coronas' objects at its +0x%X, their m_pLight at +0x%X); the render meshes' "
        "updates observed: %d of %d, their other slots: %d; step 5's answers predicted: %s%s",
        moduleName, g.on ? "on" : "verify", int(kCallKinds), names,
        g.on ? "answered, not made (stand-ins and predicted answers), then made at the flush's entry"
             : "replayed after its items at the flush's entry against what they made",
        static_cast<unsigned long long>(reinterpret_cast<char*>(g.objData) - base),
        static_cast<unsigned long long>(reinterpret_cast<char*>(g.waves) - base), g.fogVector,
        static_cast<unsigned long long>(reinterpret_cast<char*>(g.particlePool) - base), g.batchList, g.freeList,
        g.vertexPool, g.indexPool, g.polyObjects,
        static_cast<unsigned long long>(reinterpret_cast<const char*>(g.vertexSize) - base), g.lightList,
        g.lightObjects, g.lightPointer, meshObserved, kMeshUpdates, meshReadSlots, predictWhy ? "off, " : "on",
        predictWhy ? predictWhy : "");
#else
    (void)moduleName;
    (void)base;
    (void)t;
#endif
}

void TraversalRecord_OnFlushEntry(char* renderer) {
#ifdef _WIN64
    if (!g.installed || GetCurrentThreadId() != g.main) return;
    const bool was = g.recording;
    g.recording = false;
    if (g.madeStart) {  // (beside: a traversal made, not recorded, with no flush beside it: its time, for comparison)
        const uint64_t end = __rdtsc();
        g.st.madeTraversalCycles += end > g.madeStart ? end - g.madeStart : 0;
        ++g.st.madeTraversals;
        g.madeStart = 0;
    }
    if (!was) return;
    ++g.st.frames;
    const uint64_t now = __rdtsc();
    if (g.joinedAt) {  // (a traversal joined inside it, beside: the time made after the join)
        g.st.madeCycles += now > g.joinedAt ? now - g.joinedAt : 0;
        g.joinedAt = 0;
    }
    if (g.observing) {  // (the observed traversal's end: the gate off again until the hand-off)
        g.observing = false;
        RenderGate_Set(0);
        Stats& s = g.st;
        ++s.observedTraversals;
        for (int k = 0; k < s.observeds; ++k) {
            if (!g.observeFirst[k]) continue;
            ++s.observedIn[k];
            if (now > g.traversalStart && g.observeFirst[k] > g.traversalStart)
                s.observedFirst[k] += double(g.observeFirst[k] - g.traversalStart) / double(now - g.traversalStart);
        }
    }
    if (g.traversalStart && now > g.traversalStart) {  // (when its first updates on meshes drawn the frame before came)
        const double span = double(now - g.traversalStart);
        ++g.st.timedFrames;
        g.st.traversalCycles += now - g.traversalStart;
        if (g.firstSettingAt) {
            g.st.firstSetting += double(g.firstSettingAt - g.traversalStart) / span;
            ++g.st.framesSetting;
        }
        if (g.firstRebuildAt) {
            g.st.firstRebuild += double(g.firstRebuildAt - g.traversalStart) / span;
            ++g.st.framesRebuild;
        }
    }
    if (g.on) {  // (the record made now; after a join, what the 3D engine wrote into its stand-ins since; a frame
                 // whose calls were made teaches the starts and a template)
        if (g.beside && (g.answering || g.joined)) {  // (how much of it ran beside the flush, where it joined)
            ++g.st.besideFrames;
            const double share = g.joinWhy >= 0 && now > g.traversalStart
                                     ? double(g.joinAt - g.traversalStart) / double(now - g.traversalStart)
                                     : 1.0;
            g.st.besideShare += share;
            if (g.joinWhy < 0) {
                ++g.st.besideWhole;
            } else {
                const unsigned site = g.joinWhy == kJoinFlush ? g.joinGate : 0x80000000u | unsigned(g.joinWhy);
                int k = 0;
                while (k < g.st.joinSites && g.st.joinSite[k] != site) ++k;
                if (k == g.st.joinSites && k < kJoinSites) g.st.joinSite[g.st.joinSites++] = site;
                if (k < kJoinSites) {
                    ++g.st.joinSiteCount[k];
                    g.st.joinSiteShare[k] += share;
                }
            }
        }
        if (g.answering) {
            g.answering = false;
            ReplayOn(renderer);
            ++g.st.onFrames;
        } else if (g.joined) {
            FinalMerge();
            ++g.st.onFrames;
        } else {
            g.learnedFog = g.fogBase;
            g.learnedBatch = g.batchStart;
            g.learnedFree = g.freeStart;
            g.learnedPolyObjects = g.polyObjectsStart;
            g.learnedLights = g.lightStart;
            ++g.st.onLearning;
            if (g.haveNext && g.predicting) g.onReady = true;
        }
        return;
    }
    if (g.broken) {
        ++g.st.broken;
        return;
    }
    if (g.calls.empty() || g.calls[0].kind != kStartEf || *g.t.level != 1) {
        ++g.st.notStarted;
        return;
    }
    Replay(renderer);
#else
    (void)renderer;
#endif
}

bool TraversalRecord_Passes(unsigned gateIndex) {
#ifdef _WIN64
    if (!g.installed || GetCurrentThreadId() != g.main) return false;
    // (the traversal's EF_StartEf, which opens the record and its answers: the gate asks before the hook runs)
    if (gateIndex == g.gateIndex[kStartEf] && g.beside && g.onReady && !g.recording) return true;
    if (!g.answering) return false;
    for (int k = 0; k < kCallKinds; ++k)
        if (g.gateIndex[k] == gateIndex) return true;
    for (int b = 0; b < kBesides; ++b)  // (made beside the flush)
        if (g.besideIndex[b] == gateIndex) return true;
    for (int k = 0; k < kMeshSkips; ++k)  // (a mesh setting: not made if it changes nothing, else joined)
        if (g.meshSkipIndex[k] == gateIndex) return true;
#else
    (void)gateIndex;
#endif
    return false;
}

bool TraversalRecord_TakesTraversal() {
#ifdef _WIN64
    return g.installed && g.beside && g.onReady && !g.recording && GetCurrentThreadId() == g.main;
#else
    return false;
#endif
}

void TraversalRecord_OnWouldJoin(unsigned gateIndex, void* caller) {
#ifdef _WIN64
    if (!g.installed || !g.observing || GetCurrentThreadId() != g.main) return;
    for (int r = 0; r < kCallKinds; ++r)  // (the record's own slots: answered while it answers)
        if (g.gateIndex[r] == gateIndex) return;
    Stats& s = g.st;
    int k = 0;
    while (k < s.observeds && s.observed[k] != gateIndex) ++k;
    if (k == s.observeds) {
        if (k == kObserved) return;
        s.observed[s.observeds] = gateIndex;
        s.observedCaller[s.observeds++] = caller;
    }
    ++s.observedCalls[k];
    if (!g.observeFirst[k]) g.observeFirst[k] = __rdtsc();
#else
    (void)gateIndex;
    (void)caller;
#endif
}

void TraversalRecord_OnEndEf3D() {
#ifdef _WIN64
    if (!g.installed || !g.recording || !g.answering || GetCurrentThreadId() != g.main) return;
    if (FlushThread_Beside()) FlushThread_OnEntry();  // (the flush beside joined: the record made at its callback)
    if (g.answering) Join(kJoinFlush, true);           // (no flush beside: made here)
#endif
}

void TraversalRecord_OnJoined(unsigned gateIndex, bool nextFlush) {
#ifdef _WIN64
    if (!g.installed || !g.answering || g.joiningNow || GetCurrentThreadId() != g.main) return;
    g.joinGate = gateIndex;
    Join(kJoinFlush, nextFlush, FlushThread_JoinStartedAt());  // (at the next flush's entry: the whole traversal ran
                                                                // beside the flush)
#else
    (void)gateIndex;
    (void)nextFlush;
#endif
}

void TraversalRecord_Report(const char* when) {
#ifdef _WIN64
    if (!g.installed || !g.st.frames) return;
    const Stats& s = g.st;
    char kinds[1600] = "";
    size_t ku = 0;
    for (int k = 0; k < kCallKinds; ++k) {
        const int w = snprintf(kinds + ku, sizeof kinds - ku, "%s%s %.1f (replayed %.1f, objects not recorded %llu)",
                               ku ? ", " : "", kCallNames[k], double(s.calls[k]) / double(s.frames),
                               double(s.replayed[k]) / double(s.frames), s.unknownObjects[k]);
        if (w < 0 || size_t(w) >= sizeof kinds - ku) break;
        ku += size_t(w);
    }
    Log("traversal record [%s]: frames %llu, replayed %llu (a level inside the traversal or a call it can't copy %llu, "
        "not from its start %llu; "
        "no room: render objects %llu, their data %llu, wave forms %llu, fog contributions %llu, the lights' copies "
        "%llu; a pool moved by the replay %llu) | per frame: %s | items %llu, differing %llu (the calls' own objects "
        "the same by their contents "
        "%llu, the particle elements the same by their contents %llu); calls differing %llu; objects after their calls "
        "compared %llu (what was handed out for them %llu), differing %llu; results compared %llu; polygons compared "
        "%llu (%llu bytes of vertices, tangents and indices; their objects merged into an earlier one %llu); lights "
        "compared %llu (their coronas' objects %llu) | Render's params words into the stack copied %llu | %.3f ms "
        "per frame recording, %.3f ms per replay",
        when, s.frames, s.verified, s.broken, s.notStarted, s.noRoom, s.noDataRoom, s.noWavesRoom, s.noFogRoom,
        s.noLightRoom, s.poolsMoved, kinds, s.items, s.itemsDiffer, s.ownObjects, s.particlesByContents,
        s.callsDiffer, s.objectsCompared, s.attachCompared, s.objectsDiffer, s.resultsCompared, s.polysCompared,
        s.polyBytes, s.polyMerged, s.lightsCompared, s.coronasCompared, s.stackWords,
        double(s.recordCycles) / 3.0e6 / double(s.frames),
        (s.verified + s.onFrames) ? double(s.replayCycles) / 3.0e6 / double(s.verified + s.onFrames) : 0.0);
    char writes[2000] = "";
    size_t wu = 0;
    for (int k = 0; k < kCallKinds; ++k) {
        if (!s.objectWrites[k]) continue;
        int w = snprintf(writes + wu, sizeof writes - wu, "%s%s %llu of %llu calls (words:", wu ? "; " : "",
                         kCallNames[k], s.objectWrites[k], s.calls[k]);
        for (size_t o = 0; o < kObjectBytes / 8 && w > 0 && size_t(w) < sizeof writes - wu; ++o) {
            if (!s.writeAt[k][o]) continue;
            wu += size_t(w);
            w = snprintf(writes + wu, sizeof writes - wu, " +0x%zX %llu", o * 8, s.writeAt[k][o]);
        }
        if (w < 0 || size_t(w) >= sizeof writes - wu) break;
        wu += size_t(w);
        w = snprintf(writes + wu, sizeof writes - wu, ")");
        if (w < 0 || size_t(w) >= sizeof writes - wu) break;
        wu += size_t(w);
    }
    Log("traversal record [%s]: the calls that changed the render object they were given: %s", when,
        wu ? writes : "none");
    if (g.on) {
        char whys[400] = "";
        size_t yu = 0;
        for (int w = 0; w < kJoinWhys; ++w) {
            if (!s.onJoinWhy[w]) continue;
            const int n = snprintf(whys + yu, sizeof whys - yu, "%s%s %llu", yu ? ", " : "", kJoinNames[w],
                                   s.onJoinWhy[w]);
            if (n < 0 || size_t(n) >= sizeof whys - yu) break;
            yu += size_t(n);
        }
        Log("traversal record [%s]: on: frames answered %llu (of them joined %llu%s%s%s), frames whose calls were "
            "made (learning) %llu; stand-ins per answered frame %.1f; the record's calls made %llu, the answers "
            "checked against them %llu, differing %llu; calls skipped (no object) %llu; final merges %llu",
            when, s.onFrames, s.onJoins, yu ? ": " : "", whys, "", s.onLearning,
            s.onFrames ? double(s.onStandIns) / double(s.onFrames) : 0.0, s.onReplayed, s.onChecked, s.onDiffer,
            s.onSkipped, s.onFinalMerges);
    }
    if (g.beside && (s.besideFrames || s.besideNone || s.besideEnded)) {  // (how much of the traversals ran beside
                                                                           // the flush, where they joined)
        int order[kJoinSites];
        for (int k = 0; k < s.joinSites; ++k) order[k] = k;
        std::sort(order, order + s.joinSites, [&s](int a, int b) { return s.joinSiteCount[a] > s.joinSiteCount[b]; });
        char sites[1800] = "";
        size_t su = 0;
        for (int i = 0; i < s.joinSites && i < 16; ++i) {
            const int k = order[i];
            char name[96];
            if (s.joinSite[k] & 0x80000000u)
                snprintf(name, sizeof name, "%s", kJoinNames[s.joinSite[k] & 0xFF]);
            else if (s.joinSite[k] == ~0u)
                snprintf(name, sizeof name, "the queue or the allocator");
            else
                RenderGate_Describe(s.joinSite[k], name, sizeof name);
            const int n = snprintf(sites + su, sizeof sites - su, "%s%s %llu (%.0f %% of the traversal before)",
                                   su ? ", " : "", name, s.joinSiteCount[k],
                                   100.0 * s.joinSiteShare[k] / double(s.joinSiteCount[k]));
            if (n < 0 || size_t(n) >= sizeof sites - su) break;
            su += size_t(n);
        }
        Log("traversal record [%s]: beside: traversals answered beside the flush %llu, %.0f %% of their time before "
            "they joined on average, %llu ran whole (to the next flush's entry); made, not recorded: with no flush "
            "beside them at their start (the update joined it) %llu, with the flush beside them ended %llu | where "
            "they joined: %s",
            when, s.besideFrames, s.besideFrames ? 100.0 * s.besideShare / double(s.besideFrames) : 0.0,
            s.besideWhole, s.besideNone, s.besideEnded, su ? sites : "none");
        Log("traversal record [%s]: beside: the mesh settings while it answered: SetRECustomData not made %llu "
            "(changing nothing), joined %llu; SetMaterial not made %llu, joined %llu",
            when, s.meshSkipped[0], s.meshChanged[0], s.meshSkipped[1], s.meshChanged[1]);
        const double tj = s.timedJoins ? double(s.timedJoins) : 1.0;
        Log("traversal record [%s]: beside: the traversals that joined inside them (%llu), ms each: answered %.3f, the "
            "flush's join %.3f (its end, its steps after it and the queue's calls), the record made %.3f, made after "
            "it %.3f | those made with no flush beside them (%llu): %.3f",
            when, s.timedJoins, double(s.answeredCycles) / 3.0e6 / tj, double(s.joinFlushCycles) / 3.0e6 / tj,
            double(s.joinRecordCycles) / 3.0e6 / tj, double(s.madeCycles) / 3.0e6 / tj, s.madeTraversals,
            s.madeTraversals ? double(s.madeTraversalCycles) / 3.0e6 / double(s.madeTraversals) : 0.0);
    }
    if (g.beside && s.observedTraversals) {  // (the rest of the observed traversals: what would have joined too)
        int order[kObserved];
        for (int k = 0; k < s.observeds; ++k) order[k] = k;
        std::sort(order, order + s.observeds, [&s](int a, int b) {
            return s.observedFirst[a] / double(s.observedIn[a] ? s.observedIn[a] : 1) <
                   s.observedFirst[b] / double(s.observedIn[b] ? s.observedIn[b] : 1);
        });
        Log("traversal record [%s]: beside: the rest of %llu traversals after their join (one in %u), the calls that "
            "would have joined too, by where they came first:",
            when, s.observedTraversals, kObserveEvery);
        char seen[1600] = "";  // (a log line holds 2 KB: a few lines of them)
        size_t su = 0;
        for (int i = 0; i < s.observeds; ++i) {
            const int k = order[i];
            if (!s.observedIn[k]) continue;
            char name[96], from[96], one[320];
            RenderGate_Describe(s.observed[k], name, sizeof name);
            DescribeAddress(s.observedCaller[k], from, sizeof from);
            const int n = snprintf(one, sizeof one, "%s in %.0f %% (first at %.0f %%; %.1f calls; first from %s)", name,
                                   100.0 * double(s.observedIn[k]) / double(s.observedTraversals),
                                   100.0 * s.observedFirst[k] / double(s.observedIn[k]),
                                   double(s.observedCalls[k]) / double(s.observedIn[k]), from);
            if (n < 0) continue;
            if (su + size_t(n) + 3 > sizeof seen) {
                Log("traversal record [%s]:   %s", when, seen);
                su = 0;
                seen[0] = 0;
            }
            su += size_t(snprintf(seen + su, sizeof seen - su, "%s%s", su ? ", " : "", one));
        }
        if (su) Log("traversal record [%s]:   %s", when, seen);
    }
    {  // the items the calls added (verify: the traversal's own; on: the record's, made), by kind
        char made[1200] = "";
        size_t mu = 0;
        for (int k = 0; k < kCallKinds; ++k) {
            if (!s.itemsByKind[k] && !s.emptyByKind[k]) continue;
            const int n = snprintf(made + mu, sizeof made - mu, "%s%s %.1f (calls adding none %.1f)", mu ? ", " : "",
                                   kCallNames[k], double(s.itemsByKind[k]) / double(s.frames),
                                   double(s.emptyByKind[k]) / double(s.frames));
            if (n < 0 || size_t(n) >= sizeof made - mu) break;
            mu += size_t(n);
        }
        Log("traversal record [%s]: the items the %s calls added per frame, by kind: %s", when,
            g.on ? "record's (made at the flush's entry)" : "traversal's own", mu ? made : "none");
    }
    {  // EF_Query in the traversal, by its type
        char types[600] = "";
        size_t tu = 0;
        for (uint32_t k = 0; k < kQueryTypes; ++k) {
            if (!s.queries[k]) continue;
            const int n = snprintf(types + tu, sizeof types - tu, "%s%u: %.2f", tu ? ", " : "", k,
                                   double(s.queries[k]) / double(s.frames));
            if (n < 0 || size_t(n) >= sizeof types - tu) break;
            tu += size_t(n);
        }
        Log("traversal record [%s]: EF_Query in the traversal per frame by its type: %s | its light by index (27): "
            "answered %llu, checked %llu, differing %llu",
            when, tu ? types : "none", s.queryLightAnswered, s.queryLightChecked, s.queryLightDiffer);
    }
    if (g.predicting)
        Log("traversal record [%s]: step 5's answers predicted, compared with the calls' (differing): fog "
            "contributions' indices %llu (%llu), lights' indices %llu (%llu), polygons' objects %llu (%llu; merged "
            "into an earlier one %llu), particles' bool& %llu (%llu), temporary objects' reset fields against the "
            "frame before's first %llu (%llu; the frames' first ones %llu (%llu); the first field differing +0x%zX)",
            when, s.predFog, s.predFogDiffer, s.predLight, s.predLightDiffer, s.predPoly, s.predPolyDiffer,
            s.predPolyMerged, s.predParticles, s.predParticlesDiffer, s.predObjects, s.predObjectsDiffer,
            s.predTemplates, s.predTemplatesDiffer, s.predObjectFirstAt);
    if (s.meshFrames) {
        char meshes[1600] = "";
        size_t mu = 0;
        const double frames = double(s.meshFrames);
        for (int m = 0; m < kMeshUpdates; ++m) {
            const int w = snprintf(meshes + mu, sizeof meshes - mu, "%s%s %.1f (on such a mesh %.1f; nested %.1f)",
                                   mu ? ", " : "", kMeshUpdateNames[m], double(s.meshCalls[m]) / frames,
                                   double(s.meshOnDrawn[m]) / frames, double(s.meshNested[m]) / frames);
            if (w < 0 || size_t(w) >= sizeof meshes - mu) break;
            mu += size_t(w);
        }
        Log("traversal record [%s]: the render meshes' updates per frame in the traversal, its own, and of them on a "
            "mesh the frame before drew (rendered on the main thread in its traversal or at its flush's entry): %s | "
            "meshes updated per frame %.1f, of them drawn the frame before %.1f | the first on such a mesh, in the "
            "traversal's time (%.3f ms on average, the recording's cost in it): a setting (SetRECustomData, "
            "SetMaterial) at %.0f %% on average in %.0f %% of the frames, a rebuild at %.0f %% in %.0f %%",
            when, meshes, double(s.meshesUpdated) / frames, double(s.meshesUpdatedDrawn) / frames,
            s.timedFrames ? double(s.traversalCycles) / 3.0e6 / double(s.timedFrames) : 0.0,
            s.framesSetting ? 100.0 * s.firstSetting / double(s.framesSetting) : 0.0,
            s.timedFrames ? 100.0 * double(s.framesSetting) / double(s.timedFrames) : 0.0,
            s.framesRebuild ? 100.0 * s.firstRebuild / double(s.framesRebuild) : 0.0,
            s.timedFrames ? 100.0 * double(s.framesRebuild) / double(s.timedFrames) : 0.0);
        // the other slots the traversal calls on its own, the most called on a mesh it updated earlier first
        size_t order[kMeshSlots];
        for (size_t k = 0; k < kMeshSlots; ++k) order[k] = k;
        std::sort(order, order + kMeshSlots, [&s](size_t a, size_t b) {
            return s.meshReadsAfter[a] != s.meshReadsAfter[b] ? s.meshReadsAfter[a] > s.meshReadsAfter[b]
                                                              : s.meshReads[a] > s.meshReads[b];
        });
        char reads[2000] = "";
        size_t ru = 0;
        for (size_t k = 0; k < kMeshSlots && s.meshReads[order[k]]; ++k) {
            const size_t slot = order[k];
            const int w = snprintf(reads + ru, sizeof reads - ru, "%sslot %zu %.1f (after an update %.1f)",
                                   ru ? ", " : "", slot, double(s.meshReads[slot]) / frames,
                                   double(s.meshReadsAfter[slot]) / frames);
            if (w < 0 || size_t(w) >= sizeof reads - ru) break;
            ru += size_t(w);
        }
        Log("traversal record [%s]: the render meshes' other slots the traversal calls on its own, per frame, and on "
            "a mesh it updated earlier: %s", when, ru ? reads : "none");
    }
#else
    (void)when;
#endif
}

}  // namespace optiperf
