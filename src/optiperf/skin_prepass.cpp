// Skinning poses before the flush: Phase 2 of research/docs/cpu-multithreading-plan.md, after the particles (details
// in research/docs/skinning.md). x64 CryRenderD3D10.dll only.
//
// While drawing a skinned batch (DrawShader_BatchSkinned, +0x268D0), the flush asks the current object's skinnable
// (CRenderObject +0x110) for its pose: ISkinnable::GetSkeletonPose(LOD = the object's +0x88, its matrix, ...). It does
// so for items whose render element says it is skinned (slot 8: CREMesh's, its chunk has bones). The skinnables are
// CryAnimation's characters (CCharInstance) and skin attachments (CSkinInstance: their master's bones and a remap
// table), and the 3D engine's touch-bendable plants (CStatObjFoliage). A call returns pointers to the skinnable's bone
// arrays and a few values (skin_record.h); the renderer copies the bones into its shader constants right away. The
// first call of a frame also does that frame's work: the morph targets' blend (with the frame time), a plant's bones
// from its physics ropes. Every call redoes the motion-blur pose when asked for it (flag 4: cvars and the frame time)
// and copies the morph targets into the render mesh. That is animation and 3D-engine work inside the flush: on village
// 211 calls per frame, with their timer, cvar and render-mesh calls.
//   - on: right before the flush, each item the flush will draw with a skinned element and a skinnable gets one
//     GetSkeletonPose call per (skinnable, LOD, flags), from what is there by then: level 1's part of the render lists,
//     the shadow pre-pass's stashes, the reflections' captures (not their refraction lists, which a reflection
//     doesn't draw). Flags 2, and also 6 for items in the object motion-blur pass's lists (level 1's lists 2 and 6,
//     batch flag 0x80). In the flush the skinnables' slot 2, patched in their vtables, answers the renderer's calls
//     from those records: the same outputs, but the pointers at copies of what the chunks drawn with them read (the
//     bones by the chunks' bone ids, through a skin attachment's remap table; the table's bytes), taken once the
//     records are made, so the flush no longer reads the animation system's arrays. A skinnable's further calls in a
//     frame repeat its per-call work with the same result, so one record answers them all. A call that applies morph
//     targets (flags out 2) also copies the instance's blended morph targets into its render mesh's morph stream,
//     right before the draw, at every call (MorphTargetCopy): the pre-run keeps a copy of them, and the answer makes
//     that copy the same way, through the render mesh's and the renderer's buffer calls. Calls without a record (an
//     object made inside the flush, another LOD) go to vanilla.
//   - verify: as on, and each answered call runs vanilla's too (which makes the morph targets' copy itself); its
//     outputs, what its pointers point at (against the copies), and the morph targets it copied, are compared with
//     the record. The answer is the record's, with the copies.
//
// [ShadowPrepass] Skinning = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_SKINNING)

#include "skin_prepass.h"

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
#include "diag.h"
#include "flush_thread.h"
#include "log.h"
#include "reflection_prepass.h"
#include "scan.h"
#include "shadow_prepass.h"
#include "shadow_prepass_lists.h"
#include "skin_record.h"

namespace optiperf {
namespace {

enum class SMode { Off, On, Verify };

SMode ReadMode() {
    const std::wstring v = ConfigString(L"ShadowPrepass", L"Skinning", L"OPTIPERF_SHADOW_PREPASS_SKINNING", L"off");
    if (_wcsicmp(v.c_str(), L"on") == 0 || v == L"1") return SMode::On;
    if (_wcsicmp(v.c_str(), L"verify") == 0) return SMode::Verify;
    return SMode::Off;
}

#ifdef _WIN64
const char* ModeName(SMode m) { return m == SMode::On ? "on" : m == SMode::Verify ? "verify" : "off"; }

// --- patterns (x64, build 6156) ---------------------------------------------------------------------------------------
// CryRenderD3D10, DrawShader_BatchSkinned: the GetSkeletonPose call. Flags (m_PersFlags2 +0xAF8 & 0x1000000 | 0x800000)
// >> 22: 2, or 6 in the object motion-blur pass; the object's skinnable (+0x110), LOD (+0x88, a byte) and matrix
// (+0x14); then the call through slot 2
constexpr const char kCallSite[] =
    "8B 87 F8 0A 00 00 4C 8D 8C 24 A0 00 00 00 4C 8D 43 14 4C 89 4C 24 48 25 00 00 00 01 4C 8D 8C 24 "
    "B0 01 00 00 4C 89 4C 24 40 0F BA E8 17 4C 8D 8C 24 E0 00 00 00 4C 89 4C 24 38 C1 E8 16 4C 8D 8C "
    "24 A8 00 00 00 09 84 24 B0 01 00 00 48 8B 8B 10 01 00 00 0F B6 93 88 00 00 00 48 8B 01 4C 89 4C "
    "24 30 4C 8D 8C 24 B0 00 00 00 4C 89 4C 24 28 4C 8D 4C 24 78 4C 89 4C 24 20 4C 8D 8C 24 B8 00 00 "
    "00 FF 50 10 ";
constexpr int kCallSiteReturn = 132;
// CREMesh::mfIsHWSkinned (render element slot 8), which the skinned draw asks first: its chunk (+0x40) has bones (+0x40)
constexpr const char kIsHWSkinned[] = "48 8B 41 40 48 85 C0 75 01 C3 83 78 40 00 0F 95 C0 C3 ";
// the other elements' slot 8: false
constexpr const char kReturnFalse[] = "32 C0 C3 ";
// CCharInstance::GetSkeletonPose (CryAnimation): an instance of a skin attachment (+0xE8, of type 2) forwards to
// CSkinInstance's
constexpr const char kCharPose[] =
    "4C 89 44 24 18 89 54 24 10 53 55 56 57 48 81 EC 38 01 00 00 48 8B 81 E8 00 00 00 49 8B F9 49 8B "
    "D8 48 85 C0 8B EA 48 8B F1 74 7F 83 B8 30 01 00 00 02 74 0C 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? ";
// CSkinInstance::GetSkeletonPose (CryAnimation): its prologue (a frame of 0x2110 bytes)
constexpr const char kSkinPose[] =
    "48 89 5C 24 20 4C 89 44 24 18 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 B8 10 21 00 00 E8 ?? "
    "?? ?? ?? 48 2B E0 0F 29 B4 24 00 21 00 00 0F 29 BC 24 F0 20 00 00 44 0F 29 84 24 E0 20 00 00 44 "
    "0F 29 8C 24 D0 20 00 00 44 0F 29 94 24 C0 20 00 00 ";
// CStatObjFoliage::GetSkeletonPose (Cry3DEngine): its prologue, with the rope status it asks the physics for (type 14)
constexpr const char kFoliagePose[] =
    "4C 8B DC 4D 89 4B 20 57 41 57 48 81 EC 18 02 00 00 0F 57 C0 48 8B 84 24 78 02 00 00 49 89 5B 10 "
    "49 89 6B E8 49 89 73 E0 4D 89 63 D8 41 0F 29 73 B8 41 0F 29 7B A8 45 0F 29 43 98 45 33 E4 C7 84 "
    "24 20 01 00 00 08 00 00 00 C7 84 24 F0 00 00 00 0E 00 00 00 ";
// CSkinInstance::MorphTargetCopy (CryAnimation), the morph targets' blend: n vertices (r12d, the LOD entry's +0x130);
// the instance's render mesh for the LOD (+0xF8 + 8 x LOD): its system buffer (slot +0xB8) and video buffer (+0xE8);
// the morph stream (+0xC0) of the system buffer, or else the video buffer's, locked through the renderer (+0x118,
// UpdateBuffer(vb, 0, n, false, 0, 6)); the instance's blended morph targets (+0x60, 12 bytes per vertex) copied
// there; the video buffer unlocked when it was the one (+0x120, UnlockBuffer(vb, 6, n))
constexpr const char kMorphCopy[] =
    "45 85 E4 0F 84 ?? ?? ?? ?? 48 63 DB 48 8B 8C DE F8 00 00 00 48 8B 01 FF 90 B8 00 00 00 48 8B 8C "
    "DE F8 00 00 00 48 8B 11 4C 8B F0 FF 92 E8 00 00 00 49 8B 9E C0 00 00 00 48 85 DB 4C 8B E8 75 32 "
    "48 8B 0D ?? ?? ?? ?? C7 44 24 30 06 00 00 00 45 8B CC 4C 8B 11 45 33 C0 48 8B D0 89 7C 24 28 40 "
    "88 7C 24 20 41 FF 92 18 01 00 00 49 8B 9D C0 00 00 00 48 8B 7E 60 43 8D 2C 64 C1 E5 02 81 FD 00 "
    "40 00 00 7E 3A 8D 85 FF BF FF FF C1 E8 0E 83 C0 01 8B F0 F7 D8 C1 E0 0E 03 E8 41 B8 00 40 00 00 "
    "48 8B D7 48 8B CB E8 ?? ?? ?? ?? 48 81 C7 00 40 00 00 48 81 C3 00 40 00 00 48 83 EE 01 75 DB 4C "
    "63 C5 48 8B D7 48 8B CB E8 ?? ?? ?? ?? 49 83 BE C0 00 00 00 00 75 1C 48 8B 0D ?? ?? ?? ?? 45 8B "
    "CC 41 B8 06 00 00 00 48 8B 01 49 8B D5 FF 90 20 01 00 00 ";
constexpr int kMorphCopyRenderer = 0x43;  // the renderer pointer's rip-relative displacement (next instruction +0x47)
// the same function's linear morph sequence (the instance's blend weight +0x7C not negative): the system buffer's
// morph stream (+0xC0), n from the LOD entry (+0x130), then the per-frame flag (+0x160 bit 5)
constexpr const char kMorphSequence[] =
    "48 8B 8C DE F8 00 00 00 48 8B 01 FF 90 B8 00 00 00 45 8B BC 24 30 01 00 00 48 8B A8 C0 00 00 00 "
    "8B 86 60 01 00 00 44 89 BC 24 28 01 00 00 ";

const char* const kImplNames[kSkinImpls] = {"CCharInstance", "CSkinInstance", "CStatObjFoliage"};
const char* const kRtti[kSkinImpls] = {".?AVCCharInstance@@", ".?AVCSkinInstance@@", ".?AVCStatObjFoliage@@"};
const char* const kNoVtable[kSkinImpls] = {"no CCharInstance vtable in CryAnimation.dll",
                                           "no CSkinInstance vtable in CryAnimation.dll",
                                           "no CStatObjFoliage vtable in Cry3DEngine.dll"};
const char* const kOtherCode[kSkinImpls] = {"CCharInstance::GetSkeletonPose is not the expected code",
                                            "CSkinInstance::GetSkeletonPose is not the expected code",
                                            "CStatObjFoliage::GetSkeletonPose is not the expected code"};
const char* const kPatterns[kSkinImpls] = {kCharPose, kSkinPose, kFoliagePose};

constexpr int kPoseSlot = 2;          // ISkinnable::GetSkeletonPose
constexpr int kElemSkinnedSlot = 8;   // CRendElement::mfIsHWSkinned
constexpr int kElemChunk = 0x40, kChunkBones = 0x40;  // CREMesh's chunk, its bone count (mfIsHWSkinned's reads)
constexpr int kChunkBoneIds = 0x38;  // the chunk's bone ids (16-bit), which the skinned draw reads the bones by
// CRenderObject
constexpr int kObjMatrix = 0x14, kObjLod = 0x88, kObjSkinnable = 0x110;
// the morph targets' copy (kMorphCopy, kMorphSequence): the instance's render meshes (per LOD), its blended morph
// targets (a vector: begin, end) and blend weight; the render mesh's buffers; a vertex buffer's morph stream
constexpr int kInstMeshes = 0xF8, kInstMorph = 0x60, kInstBlend = 0x7C;
constexpr int kMeshSysBuffer = 0xB8, kMeshVideoBuffer = 0xE8, kBufferStream = 0xC0;
constexpr int kVfUpdateBuffer = 0x118, kVfUnlockBuffer = 0x120, kMorphStream = 6;
constexpr size_t kMorphVertex = 12;
// the render lists: 15 per side of the water; the object motion-blur pass (level 1 only) draws lists 2 and 6 with batch
// flag 0x80; a reflection (level 2) doesn't draw its refraction list (14)
constexpr int kListsPerSide = 15, kRefractList = 14;
constexpr uint32_t kBatchMotionBlur = 0x80;
constexpr uint32_t kFlagsBase = 2, kFlagsMotionBlur = 4, kFlagsMorph = 2;
constexpr uint32_t kNoRecord = 0xFFFFFFFFu;
constexpr uint32_t kSkinnableLod = 0xFF;  // the skinnables index's flags byte: (skinnable, 0, 0xFF) -> an LOD
enum Source { kLists, kStashes, kCaptures, kSources };
const char* const kSourceNames[kSources] = {"level 1's lists", "shadow stashes", "reflection captures"};
enum : uint8_t { kElemNever, kElemMesh, kElemCall };
#endif  // _WIN64

struct Rec {
    void* skinnable;
    uint8_t impl, lod, flagsIn;
    bool morph;          // the call applied morph targets (flags out 2): each call copies them into the render mesh
    bool morphReplay;    // their copy is recorded (else the flush's calls keep vanilla's)
    bool morphSequence;  // a linear morph sequence: the system buffer's stream only
    uint32_t calls;      // the flush's calls with its key
    void* mesh;          // morph targets: the render mesh, the vertex count, the blended targets (in morphData)
    uint32_t morphVerts, morphAt;
    float matrix[12];
    sk::Outputs out;     // what the call wrote: pointers to the skinnable's arrays
    sk::Extents ext;     // what the chunks drawn with it read through those pointers
    uint32_t copyAt[sk::kPtrOutputs];  // their copies in boneData (sk::kNotCopied: none)
    bool copied;         // the answer points at the copies (else at the skinnable's arrays)
    sk::Outputs answer;  // what the flush's calls get
};

struct ImplStats {
    uint64_t preruns = 0, preruns6 = 0;  // records made (of them with the motion-blur flag)
    uint64_t calls = 0, replayed = 0, morph = 0, morphReplayed = 0, notRecorded = 0;
    uint64_t unused = 0, unused6 = 0, unusedMorph = 0;
    uint64_t vCompared = 0, vSame = 0, vDiffer = 0, vMatrix = 0;
    uint64_t vMorphCompared = 0, vMorphDiffer = 0;  // the morph targets vanilla's call copied, against the record's
    uint64_t vMorphDest = 0, vMorphDestDiffer = 0;  // the morph stream the replay would copy into, after vanilla's copy
    uint64_t vCopies = 0, vCopiesDiffer = 0;        // the bone copies, against what vanilla's pointers point at
};

struct Count {
    const char* what;
    uint64_t n;
};

struct SStats {
    uint64_t flushes = 0, unknown = 0, keys = 0;
    uint64_t items[3] = {}, skinnedItems[3] = {}, skinnableNotSkinned = 0;  // per source; items with a skinnable, not drawn skinned
    int maxKeys = 0;
    uint64_t prerunCycles = 0, vanillaCycles = 0, replayCycles = 0;
    uint64_t missNoObject = 0, missLod = 0, missMotionBlur = 0, missUnknown = 0;
    uint64_t otherCaller = 0;
    uint64_t morphRecords = 0, morphNotRecorded = 0, morphBytes = 0, morphLocked = 0;
    uint64_t boneRecords = 0, boneBytes = 0, bonesNotCopied = 0, bonesUnread = 0;  // the bone copies
    uint64_t mbAgain = 0;  // motion-blur poses computed once more before their copy (FillMotionBlur)
    ImplStats impl[kSkinImpls];
    Count vWhat[12] = {};
};

#ifdef _WIN64
// a small cache of the render elements' vtables seen: what their slot 8 is
struct VtCache {
    void* const* key[128] = {};
    uint8_t value[128] = {};
    int Find(void* const* vt) const {
        for (uint32_t h = uint32_t(uintptr_t(vt) >> 4) & 127, n = 0; n < 128; h = (h + 1) & 127, ++n) {
            if (key[h] == vt) return value[h];
            if (!key[h]) return -1;
        }
        return -1;
    }
    void Put(void* const* vt, uint8_t v) {
        for (uint32_t h = uint32_t(uintptr_t(vt) >> 4) & 127, n = 0; n < 128; h = (h + 1) & 127, ++n)
            if (!key[h] || key[h] == vt) {
                key[h] = vt;
                value[h] = v;
                return;
            }
    }
};
#endif

struct SState {
    SMode mode = SMode::Off;
    bool installed = false, setupTried = false, ready = false, verify = false, inFlush = false;
#ifdef _WIN64
    SkinTarget t;
    const ShadowPrepassTarget* shadow = nullptr;
    char* base = nullptr;  // the renderer
    char* renderer = nullptr;
    DWORD mainThread = 0;
    sk::PoseFn orig[kSkinImpls] = {};  // what the patched slots held: the function, or the call tracer's thunk
    std::vector<Rec> recs;  // this flush's
    sk::KeyIndex index;       // (skinnable, LOD, flags) -> recs (kNoRecord: an unknown class)
    sk::KeyIndex skinnables;  // (skinnable, 0, 0xFF) -> an LOD it was pre-run with: why a call had no record
    VtCache elements;
    int source = kLists;  // the pre-run's item source being scanned
    bool morphReady = false;  // the morph targets' copy is replayed (MorphTargetCopy found, its renderer the flush's)
    std::vector<uint8_t> morphData;  // this flush's records' blended morph targets
    std::vector<uint8_t> boneData;   // ... and the copies of the bones and remap tables their answers point at
    sk::KeyIndex mbArrays;           // motion-blur array -> the records that returned it (FillMotionBlur)
    int loggedMiss = 0, loggedUnused = 0, loggedVerify = 0, loggedUnknown = 0, loggedOther = 0, loggedMorph = 0;
    int loggedBones = 0;
#endif
    SStats st;
};
SState g;

#ifdef _WIN64
template <class T>
T& At(char* p, int offset) {
    return *reinterpret_cast<T*>(p + offset);
}

char* Unique(char* base, const char* text, const char** why, const char* what) {
    const Pattern p = ParsePattern(text);
    const auto hits = FindInCode(base, p, 2);
    if (hits.size() != 1) *why = what;
    return hits.size() == 1 ? hits[0] : nullptr;
}

bool Matches(char* base, char* at, const char* text) {
    const Pattern p = ParsePattern(text);
    return at && IsInCode(base, at, p.size()) && MatchesAt(at, p);
}

// A vtable entry of a module mapped at base: the address as relocated, or, when the image wasn't (a test mapping it
// unrelocated), rebased from the image's preferred base.
char* Entry(char* base, void* entry) {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint64_t e = reinterpret_cast<uint64_t>(entry), size = nt->OptionalHeader.SizeOfImage;
    if (e - reinterpret_cast<uint64_t>(base) < size) return static_cast<char*>(entry);
    if (e - nt->OptionalHeader.ImageBase < size) return base + (e - nt->OptionalHeader.ImageBase);
    return nullptr;
}

void Append(char* line, size_t size, size_t& n, const char* fmt, ...) {
    if (n >= size) return;
    va_list a;
    va_start(a, fmt);
    const int k = vsnprintf(line + n, size - n, fmt, a);
    va_end(a);
    if (k > 0) n = (std::min)(size - 1, n + size_t(k));
}

// The class an object's MSVC RTTI names (x64 locators: image-relative), e.g. ".?AVCCharInstance@@"; "?" if none.
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

// which of the known classes a skinnable is (its vtable), or -1
int ImplOf(void* skinnable) {
    void* vt = nullptr;
    __try {
        vt = *static_cast<void**>(skinnable);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    for (int i = 0; i < kSkinImpls; ++i)
        if (vt == g.t.vtables[i]) return i;
    return -1;
}

void CountWhat(const char* what) {
    for (Count& c : g.st.vWhat) {
        if (c.what && std::strcmp(c.what, what) == 0) {
            ++c.n;
            return;
        }
        if (!c.what) {
            c = Count{what, 1};
            return;
        }
    }
}

// whether the flush's skinned draw will ask for this element's pose: its slot 8, as the renderer calls it
bool IsSkinnedElement(char* elem) {
    void* const* vt = *reinterpret_cast<void* const* const*>(elem);
    int kind = g.elements.Find(vt);
    if (kind < 0) {
        kind = SkinPrepass_ElementKind(g.base, g.t, vt);
        g.elements.Put(vt, uint8_t(kind));
    }
    if (kind == kElemNever) return false;
    if (kind == kElemMesh) {
        char* chunk = At<char*>(elem, kElemChunk);
        return chunk && At<int>(chunk, kChunkBones) != 0;
    }
    return reinterpret_cast<bool (*)(char*)>(vt[kElemSkinnedSlot])(elem);
}

// --- the morph targets' copy (MorphTargetCopy's, at each call) -------------------------------------------------------
using BufferFn = char* (*)(void* mesh);
using UpdateBufferFn = void (*)(char* r, char* vb, const void* src, int count, bool unlock, int offs, int type);
using UnlockBufferFn = void (*)(char* r, char* vb, int type, int count);

// A morph record: what each of the flush's calls copies into the render mesh. MorphTargetCopy only applies them at
// the instance's morph LOD (the call's, then), and the frame's first call (the pre-run's) has sized the blended morph
// targets to that LOD's vertex count.
void CaptureMorph(Rec& rec, char* inst) {
    void* mesh = At<void*>(inst, kInstMeshes + 8 * int(rec.lod));
    char* begin = At<char*>(inst, kInstMorph);
    char* end = At<char*>(inst, kInstMorph + 8);
    const size_t bytes = size_t(end - begin);
    if (!mesh || !begin || end < begin || bytes % kMorphVertex != 0 || bytes > (size_t(64) << 20)) {
        ++g.st.morphNotRecorded;
        if (++g.loggedMorph <= 5)
            Log("skinning: flush %llu: %s %p LOD %u: its morph targets (%p..%p, mesh %p) not recorded: its calls keep "
                "vanilla's", g.st.flushes, kImplNames[rec.impl], rec.skinnable, unsigned(rec.lod), begin, end, mesh);
        return;
    }
    const float blend = At<float>(inst, kInstBlend);
    rec.morphSequence = blend > 0.0f || blend == 0.0f;  // (as MorphTargetCopy tests it)
    rec.mesh = mesh;
    rec.morphVerts = uint32_t(bytes / kMorphVertex);
    rec.morphAt = uint32_t(g.morphData.size());
    g.morphData.insert(g.morphData.end(), reinterpret_cast<const uint8_t*>(begin), reinterpret_cast<const uint8_t*>(end));
    rec.morphReplay = true;
    ++g.st.morphRecords;
    g.st.morphBytes += bytes;
}

// the copy as MorphTargetCopy makes it: into the system buffer's morph stream, or else into the video buffer's,
// locked through the renderer for it (a linear morph sequence: the system buffer's only)
void ReplayMorph(const Rec& rec) {
    void* const* vt = *static_cast<void* const* const*>(rec.mesh);
    char* sys = reinterpret_cast<BufferFn>(vt[kMeshSysBuffer / 8])(rec.mesh);
    const uint8_t* data = g.morphData.data() + rec.morphAt;
    const size_t bytes = size_t(rec.morphVerts) * kMorphVertex;
    if (rec.morphSequence) {
        if (char* dst = At<char*>(sys, kBufferStream)) std::memcpy(dst, data, bytes);
        return;
    }
    char* vid = reinterpret_cast<BufferFn>(vt[kMeshVideoBuffer / 8])(rec.mesh);
    void* const* rvt = *reinterpret_cast<void* const* const*>(g.renderer);
    char* dst = At<char*>(sys, kBufferStream);
    if (!dst) {
        reinterpret_cast<UpdateBufferFn>(rvt[kVfUpdateBuffer / 8])(g.renderer, vid, nullptr, int(rec.morphVerts), false,
                                                                     0, kMorphStream);
        dst = At<char*>(vid, kBufferStream);
        ++g.st.morphLocked;
    }
    if (dst) std::memcpy(dst, data, bytes);
    if (!At<char*>(sys, kBufferStream))
        reinterpret_cast<UnlockBufferFn>(rvt[kVfUnlockBuffer / 8])(g.renderer, vid, kMorphStream, int(rec.morphVerts));
}

// verify: after vanilla's call (which made the copy), the instance's blended morph targets against the record's
void VerifyMorph(const Rec& rec, char* inst) {
    if (!rec.morphReplay) return;
    ImplStats& is = g.st.impl[rec.impl];
    ++is.vMorphCompared;
    char* begin = At<char*>(inst, kInstMorph);
    char* end = At<char*>(inst, kInstMorph + 8);
    const size_t bytes = size_t(rec.morphVerts) * kMorphVertex;
    const uint8_t* data = g.morphData.data() + rec.morphAt;
    const bool same = At<void*>(inst, kInstMeshes + 8 * int(rec.lod)) == rec.mesh && size_t(end - begin) == bytes &&
                      std::memcmp(begin, data, bytes) == 0;
    if (!same) {
        ++is.vMorphDiffer;
        if (++g.loggedVerify <= 10)
            Log("skinning: VERIFY flush %llu: %s %p LOD %u: the morph targets vanilla's call copied differ from the "
                "record's", g.st.flushes, kImplNames[rec.impl], rec.skinnable, unsigned(rec.lod));
    }
    // where the replay copies them: right after vanilla's copy it holds those bytes (the system buffer's morph stream;
    // a video buffer locked for the copy can't be read back)
    void* const* vt = *static_cast<void* const* const*>(rec.mesh);
    char* sys = reinterpret_cast<BufferFn>(vt[kMeshSysBuffer / 8])(rec.mesh);
    char* dst = sys ? At<char*>(sys, kBufferStream) : nullptr;
    if (!dst) return;
    ++is.vMorphDest;
    if (std::memcmp(dst, data, bytes) != 0) {
        ++is.vMorphDestDiffer;
        if (++g.loggedVerify <= 10)
            Log("skinning: VERIFY flush %llu: %s %p LOD %u: the render mesh's morph stream doesn't hold the record's "
                "morph targets after vanilla's copy", g.st.flushes, kImplNames[rec.impl], rec.skinnable, unsigned(rec.lod));
    }
}

// --- right before the flush: one call per (skinnable, LOD, flags) ----------------------------------------------------
// the record of the (skinnable, LOD, flags), made now if it is the first; kNoRecord for a skinnable of another class
uint32_t RecordPose(void* s, uint32_t lod, uint32_t flags, char* obj) {
    const uint64_t key = sk::Key(s, lod, flags);
    if (const uint32_t* at = g.index.Find(key)) return *at;
    const int impl = ImplOf(s);
    if (impl < 0) {
        g.index.Insert(key, kNoRecord);
        ++g.st.unknown;
        if (++g.loggedUnknown <= 5)
            Log("skinning: flush %llu: a skinnable of another class (%s, %p): its calls stay in the flush", g.st.flushes,
                RttiName(s), s);
        return kNoRecord;
    }
    Rec rec;
    rec.skinnable = s;
    rec.impl = uint8_t(impl);
    rec.lod = uint8_t(lod);
    rec.flagsIn = uint8_t(flags);
    rec.calls = 0;
    const float* matrix = reinterpret_cast<const float*>(obj + kObjMatrix);
    std::memcpy(rec.matrix, matrix, sizeof rec.matrix);
    sk::Prepare(rec.out, flags);
    sk::Finish(rec.out, sk::Call(g.orig[impl], s, lod, matrix, rec.out));
    rec.morph = (rec.out.flags & kFlagsMorph) != 0;
    rec.morphReplay = rec.morphSequence = false;
    rec.mesh = nullptr;
    rec.morphVerts = rec.morphAt = 0;
    rec.ext = sk::Extents{};
    for (uint32_t& a : rec.copyAt) a = sk::kNotCopied;
    rec.copied = false;
    if (rec.morph && g.morphReady) CaptureMorph(rec, static_cast<char*>(s));
    ImplStats& is = g.st.impl[impl];
    ++is.preruns;
    if (flags & kFlagsMotionBlur) ++is.preruns6;
    const uint32_t index = uint32_t(g.recs.size());
    g.index.Insert(key, index);
    const uint64_t any = sk::Key(s, 0, kSkinnableLod);
    if (!g.skinnables.Find(any)) g.skinnables.Insert(any, lod);
    g.recs.push_back(rec);
    return index;
}

// What the item's chunk will read through the record's pointers. The skinned draw takes the chunk at the element's
// +0x40 (whatever its class), its bone ids at the chunk's +0x38 and their count at +0x40; with more bones than the pose
// has, it doesn't draw.
void AddChunk(uint32_t r, char* elem) {
    if (r == kNoRecord) return;
    Rec& rec = g.recs[r];
    char* chunk = At<char*>(elem, kElemChunk);
    if (!chunk) return;
    const uint32_t n = At<uint32_t>(chunk, kChunkBones);
    const auto* ids = At<const uint16_t*>(chunk, kChunkBoneIds);
    if (!n || !ids || n > rec.out.ret) return;
    const bool remap = (rec.out.written & (1 << sk::kRemap)) && rec.out.p[sk::kRemap];
    sk::AddChunk(rec.ext, ids, n, remap ? static_cast<const uint8_t*>(rec.out.p[sk::kRemap]) : nullptr);
}

void ScanItem(const sp::RendItem& it, bool motionBlurList) {
    ++g.st.items[g.source];
    char* obj = it.obj;
    if (!obj || !it.elem) return;
    void* s = At<void*>(obj, kObjSkinnable);
    if (!s) return;
    if (!IsSkinnedElement(it.elem)) {
        ++g.st.skinnableNotSkinned;
        return;
    }
    ++g.st.skinnedItems[g.source];
    const uint32_t lod = At<uint8_t>(obj, kObjLod);
    AddChunk(RecordPose(s, lod, kFlagsBase, obj), it.elem);
    if (motionBlurList && (it.batchFlags & kBatchMotionBlur))
        AddChunk(RecordPose(s, lod, kFlagsBase | kFlagsMotionBlur, obj), it.elem);
}

// A character's motion-blur pose (CryAnimation +0x64A0, the master's for a skin attachment) is computed at each call
// that asks for it, but the first computation only sizes the array: its fill and blend loops run on the old size, 0.
// In vanilla the flush's next call fills it. A motion-blur array only one record returned (none of the others
// computed it again) gets that call now, before its copy: a first computation is filled, any other recomputed alike.
void FillMotionBlur() {
    g.mbArrays.Clear();
    for (Rec& rec : g.recs) {
        if (!(rec.flagsIn & kFlagsMotionBlur) || rec.impl == kSkinFoliage) continue;  // (a plant's: once a frame)
        for (int i : {sk::kMbQuatsL, sk::kMbQuatsS}) {
            if (!(rec.out.written & (1 << i)) || !rec.out.p[i]) continue;
            const uint64_t key = uint64_t(uintptr_t(rec.out.p[i]));
            if (uint32_t* n = g.mbArrays.Find(key))
                ++*n;
            else
                g.mbArrays.Insert(key, 1);
        }
    }
    for (Rec& rec : g.recs) {
        if (!(rec.flagsIn & kFlagsMotionBlur) || rec.impl == kSkinFoliage) continue;
        bool alone = false;
        for (int i : {sk::kMbQuatsL, sk::kMbQuatsS}) {
            const uint32_t* n = (rec.out.written & (1 << i)) && rec.out.p[i]
                                    ? g.mbArrays.Find(uint64_t(uintptr_t(rec.out.p[i])))
                                    : nullptr;
            if (n && *n == 1) alone = true;
        }
        if (!alone) continue;
        sk::Outputs again;
        sk::Prepare(again, rec.flagsIn);
        sk::Finish(again, sk::Call(g.orig[rec.impl], rec.skinnable, rec.lod, rec.matrix, again));
        ++g.st.mbAgain;
    }
}

// The bones and remap tables the flush will read through the records' pointers, copied once all the records are made
// (skin_record.h): the answers then point at the copies. A record whose chunks read past the count its call returned
// keeps the skinnable's arrays (with a remap table, the bound is the table's count: the bones are the master's).
void CopyBones() {
    FillMotionBlur();
    g.boneData.clear();
    for (Rec& rec : g.recs) {
        const bool remap = (rec.out.written & (1 << sk::kRemap)) && rec.out.p[sk::kRemap];
        if (rec.ext.bones == 0) {  // (no chunk it is drawn with reads anything)
            ++g.st.bonesUnread;
            continue;
        }
        if ((remap ? rec.ext.remap : rec.ext.bones) > rec.out.ret) {
            ++g.st.bonesNotCopied;
            if (++g.loggedBones <= 5)
                Log("skinning: flush %llu: %s %p LOD %u: its chunks read %u %s, past the %u its call returned: its "
                    "answer keeps pointing at its arrays", g.st.flushes, kImplNames[rec.impl], rec.skinnable,
                    unsigned(rec.lod), remap ? rec.ext.remap : rec.ext.bones, remap ? "remap entries" : "bones",
                    rec.out.ret);
            continue;
        }
        g.st.boneBytes += sk::CopyOut(rec.out, rec.ext, rec.flagsIn, g.boneData, rec.copyAt);
        rec.copied = true;
        ++g.st.boneRecords;
    }
    for (Rec& rec : g.recs) rec.answer = rec.copied ? sk::WithCopies(rec.out, g.boneData.data(), rec.copyAt) : rec.out;
}

// a shadow stash's or a reflection capture's items (ctx: non-null for a reflection's, whose refraction lists it
// doesn't draw)
void ScanStash(const sp::Stash& s, void* ctx) {
    const bool reflection = ctx != nullptr;
    const sp::RendItem* it = s.items.data();
    for (int k = 0; k < sp::kLists; ++k) {
        const uint32_t n = s.counts[k];
        if (!(reflection && k % kListsPerSide == kRefractList))
            for (uint32_t i = 0; i < n; ++i) ScanItem(it[i], false);
        it += n;
    }
}

void Prerun() {
    const uint64_t t0 = __rdtsc();
    g.recs.clear();
    g.index.Clear();
    g.skinnables.Clear();
    g.morphData.clear();
    // level 1's part of the render lists: the main pass, and the object motion-blur pass (flags 6 there)
    const auto* lists = reinterpret_cast<const sp::ItemList*>(g.shadow->lists);
    const uint32_t* starts = g.shadow->starts;
    g.source = kLists;
    for (int k = 0; k < sp::kLists; ++k) {
        const int list = k % kListsPerSide;
        const bool motionBlur = list == 2 || list == 6;
        const sp::ItemList& l = lists[k];
        for (uint32_t i = starts[k]; i < l.count; ++i) ScanItem(l.elems[i], motionBlur);
    }
    // the shadow maps the shadow pre-pass rendered early, and the reflections the dry run rendered early
    g.source = kStashes;
    ShadowPrepass_ForEachStash(&ScanStash, nullptr);
    g.source = kCaptures;
    static int reflection = 1;
    ReflectionPrepass_ForEachCapture(&ScanStash, &reflection);
    CopyBones();
    g.st.keys += g.recs.size();
    g.st.maxKeys = (std::max)(g.st.maxKeys, int(g.recs.size()));
    g.st.prerunCycles += __rdtsc() - t0;
}

// --- in the flush: the skinnables' slot 2 -------------------------------------------------------------------------------
uint32_t Vanilla(int impl, void* self, uint32_t lod, const float* m, void** l, void** s, void** ml, void** ms,
                 float* shape, uint32_t* flags, void** remap) {
    const uint64_t t0 = __rdtsc();
    const uint32_t ret = g.orig[impl](self, lod, m, l, s, ml, ms, shape, flags, remap);
    g.st.vanillaCycles += __rdtsc() - t0;
    return ret;
}

void Miss(int impl, void* self, uint32_t lod, uint32_t flags, bool unknown) {
    const char* why;
    if (unknown) {
        ++g.st.missUnknown;
        why = "a skinnable of another class";
    } else if (!g.skinnables.Find(sk::Key(self, 0, kSkinnableLod))) {
        ++g.st.missNoObject;
        why = "no item had this skinnable at the flush's entry";
    } else if ((flags & kFlagsMotionBlur) && g.index.Find(sk::Key(self, lod, flags & ~kFlagsMotionBlur))) {
        ++g.st.missMotionBlur;
        why = "asked for the motion-blur pose, not predicted";
    } else {
        ++g.st.missLod;
        why = "pre-run with another LOD";
    }
    if (++g.loggedMiss <= 10)
        Log("skinning: flush %llu: %s %p LOD %u flags %u (level %d) had no record: %s", g.st.flushes, kImplNames[impl],
            self, lod, flags, *g.shadow->level, why);
}

// verify: vanilla's call into scratch outputs, compared with the record
void Verify(const Rec& rec, int impl, void* self, uint32_t lod, const float* m, uint32_t flagsIn) {
    sk::Outputs o;
    sk::Prepare(o, flagsIn);
    const uint64_t t0 = __rdtsc();
    sk::Finish(o, sk::Call(g.orig[impl], self, lod, m, o));
    g.st.vanillaCycles += __rdtsc() - t0;
    ImplStats& is = g.st.impl[impl];
    ++is.vCompared;
    if (std::memcmp(rec.matrix, m, sizeof rec.matrix) != 0) ++is.vMatrix;
    if (rec.copied) {  // what the renderer will read from the copies: what vanilla's pointers point at now
        ++is.vCopies;
        if (!sk::SameCopies(o, rec.ext, g.boneData.data(), rec.copyAt)) {
            ++is.vCopiesDiffer;
            if (++g.loggedVerify <= 10)
                Log("skinning: VERIFY flush %llu: %s %p LOD %u flags %u: the bone copies (%u bones, %u remap entries) "
                    "differ from what the flush's call points at", g.st.flushes, kImplNames[impl], self, lod, flagsIn,
                    rec.ext.bones, rec.ext.remap);
        }
    }
    const char* what = nullptr;
    if (sk::Same(rec.out, o, &what)) {
        ++is.vSame;
        return;
    }
    ++is.vDiffer;
    CountWhat(what);
    if (++g.loggedVerify <= 10)
        Log("skinning: VERIFY flush %llu: %s %p LOD %u flags %u (level %d): the flush's call differs from the record in "
            "%s (bone count %u / %u, flags %u / %u)",
            g.st.flushes, kImplNames[impl], self, lod, flagsIn, *g.shadow->level, what, rec.out.ret, o.ret,
            rec.out.flags, o.flags);
}

uint32_t Answer(int impl, void* self, uint32_t lod, const float* m, void** l, void** s, void** ml, void** ms,
                float* shape, uint32_t* flags, void** remap, void* caller) {
    if (!g.inFlush || !OnFlushThread()) return g.orig[impl](self, lod, m, l, s, ml, ms, shape, flags, remap);
    if (caller != g.t.callReturn) {
        ++g.st.otherCaller;
        if (++g.loggedOther <= 5) {
            char where[128] = "";
            Log("skinning: flush %llu: %s::GetSkeletonPose called in the flush from %s: left to it", g.st.flushes,
                kImplNames[impl], DescribeAddress(caller, where, sizeof where));
        }
        return Vanilla(impl, self, lod, m, l, s, ml, ms, shape, flags, remap);
    }
    ImplStats& is = g.st.impl[impl];
    ++is.calls;
    const uint32_t flagsIn = *flags;
    const uint32_t* at = g.index.Find(sk::Key(self, lod, flagsIn));
    if (!at || *at == kNoRecord) {
        ++is.notRecorded;
        Miss(impl, self, lod, flagsIn, at != nullptr);
        return Vanilla(impl, self, lod, m, l, s, ml, ms, shape, flags, remap);
    }
    Rec& rec = g.recs[*at];
    ++rec.calls;
    if (g.verify) {
        Verify(rec, impl, self, lod, m, flagsIn);  // (vanilla's call, which also made the morph targets' copy)
        if (rec.morph) {
            ++is.morph;
            VerifyMorph(rec, static_cast<char*>(self));
        } else {
            ++is.replayed;
        }
        return sk::Apply(rec.answer, l, s, ml, ms, shape, flags, remap);  // (the copies, as on answers)
    }
    if (rec.morph) {  // the morph targets are copied into the render mesh at each call, right before the draw
        ++is.morph;
        if (!rec.morphReplay) return Vanilla(impl, self, lod, m, l, s, ml, ms, shape, flags, remap);
        ++is.morphReplayed;
        const uint64_t t0 = __rdtsc();
        ReplayMorph(rec);
        const uint32_t ret = sk::Apply(rec.answer, l, s, ml, ms, shape, flags, remap);
        g.st.replayCycles += __rdtsc() - t0;
        return ret;
    }
    ++is.replayed;
    const uint64_t t0 = __rdtsc();
    const uint32_t ret = sk::Apply(rec.answer, l, s, ml, ms, shape, flags, remap);
    g.st.replayCycles += __rdtsc() - t0;
    return ret;
}

uint32_t CharPose(void* self, uint32_t lod, const float* m, void** l, void** s, void** ml, void** ms, float* shape,
                  uint32_t* flags, void** remap) {
    return Answer(kSkinChar, self, lod, m, l, s, ml, ms, shape, flags, remap, _ReturnAddress());
}
uint32_t SkinPose(void* self, uint32_t lod, const float* m, void** l, void** s, void** ml, void** ms, float* shape,
                  uint32_t* flags, void** remap) {
    return Answer(kSkinSkin, self, lod, m, l, s, ml, ms, shape, flags, remap, _ReturnAddress());
}
uint32_t FoliagePose(void* self, uint32_t lod, const float* m, void** l, void** s, void** ml, void** ms, float* shape,
                     uint32_t* flags, void** remap) {
    return Answer(kSkinFoliage, self, lod, m, l, s, ml, ms, shape, flags, remap, _ReturnAddress());
}
const sk::PoseFn kHooks[kSkinImpls] = {&CharPose, &SkinPose, &FoliagePose};

// a vtable slot pointed at fn (one aligned store: other threads calling meanwhile get the old or the new entry)
bool PatchSlot(void** slot, void* fn, void** old) {
    DWORD protect;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
    *old = *slot;
    InterlockedExchangePointer(slot, fn);
    VirtualProtect(slot, sizeof(void*), protect, &protect);
    return true;
}

void Setup(char* r) {
    g.setupTried = true;
    g.mainThread = GetCurrentThreadId();
    g.renderer = r;
    char* anim = reinterpret_cast<char*>(GetModuleHandleW(L"CryAnimation.dll"));
    char* e3d = reinterpret_cast<char*>(GetModuleHandleW(L"Cry3DEngine.dll"));
    const char* why = anim ? (e3d ? nullptr : "no Cry3DEngine.dll") : "no CryAnimation.dll";
    if (!why) SkinPrepass_LocateSkinnables(anim, e3d, &g.t, &why);
    if (why) {
        Log("skinning: poses left in the flush: %s", why);
        return;
    }
    for (int i = 0; i < kSkinImpls; ++i) {
        void* old = nullptr;
        if (!PatchSlot(&g.t.vtables[i][kPoseSlot], reinterpret_cast<void*>(kHooks[i]), &old)) {
            for (int j = 0; j < i; ++j) PatchSlot(&g.t.vtables[j][kPoseSlot], reinterpret_cast<void*>(g.orig[j]), &old);
            Log("skinning: poses left in the flush: could not write %s's vtable", kImplNames[i]);
            return;
        }
        g.orig[i] = reinterpret_cast<sk::PoseFn>(old);
    }
    const char* morphWhy = nullptr;
    if (!SkinPrepass_LocateMorphCopy(anim, &g.t, &morphWhy))
        Log("skinning: calls that apply morph targets keep vanilla's call: %s", morphWhy);
    else if (*g.t.animRenderer != r)
        Log("skinning: calls that apply morph targets keep vanilla's call: CryAnimation's renderer (%p) is not the "
            "flush's (%p)", *g.t.animRenderer, static_cast<void*>(r));
    else
        g.morphReady = true;
    g.recs.reserve(512);
    g.morphData.reserve(size_t(1) << 20);
    g.boneData.reserve(size_t(1) << 20);
    g.ready = true;
    Log("skinning: poses asked for before the flush (mode=%s): GetSkeletonPose of CCharInstance CryAnimation.dll+0x%X "
        "(vtable +0x%X), CSkinInstance +0x%X (vtable +0x%X), CStatObjFoliage Cry3DEngine.dll+0x%X (vtable +0x%X); "
        "morph targets' copy %s (CryAnimation.dll+0x%X, +0x%X)",
        ModeName(g.mode), unsigned(g.t.poses[kSkinChar] - anim),
        unsigned(reinterpret_cast<char*>(g.t.vtables[kSkinChar]) - anim), unsigned(g.t.poses[kSkinSkin] - anim),
        unsigned(reinterpret_cast<char*>(g.t.vtables[kSkinSkin]) - anim), unsigned(g.t.poses[kSkinFoliage] - e3d),
        unsigned(reinterpret_cast<char*>(g.t.vtables[kSkinFoliage]) - e3d), g.morphReady ? "replayed" : "left to vanilla",
        g.t.morphCopy ? unsigned(g.t.morphCopy - anim) : 0u, g.t.morphSequence ? unsigned(g.t.morphSequence - anim) : 0u);
}

void Report(const char* when) {
    const SStats& s = g.st;
    const double f = double(s.flushes ? s.flushes : 1);
    char sources[384] = "";
    size_t n = 0;
    for (int i = 0; i < kSources; ++i)
        Append(sources, sizeof sources, n, "%s %s %llu (skinned %llu)", i ? "," : "", kSourceNames[i], s.items[i],
               s.skinnedItems[i]);
    char byClass[512] = "";
    n = 0;
    for (int i = 0; i < kSkinImpls; ++i)
        Append(byClass, sizeof byClass, n, "%s %s %llu (with motion blur %llu)", i ? "," : "", kImplNames[i],
               s.impl[i].preruns, s.impl[i].preruns6);
    Log("skinning [%s]: mode=%s | flushes=%llu | items scanned:%s; with a skinnable but not skinned %llu; skinnables of "
        "another class %llu | records=%llu (most in a flush %d):%s | %.3f ms per flush",
        when, ModeName(g.mode), s.flushes, sources, s.skinnableNotSkinned, s.unknown, s.keys, s.maxKeys, byClass,
        double(s.prerunCycles) / 3.0e6 / f);
    char calls[768] = "";
    n = 0;
    for (int i = 0; i < kSkinImpls; ++i)
        Append(calls, sizeof calls, n,
               "%s %s %llu: answered %llu, applying morph targets %llu (their copy replayed %llu), not recorded %llu",
               i ? ";" : "", kImplNames[i], s.impl[i].calls, s.impl[i].replayed, s.impl[i].morph,
               s.impl[i].morphReplayed, s.impl[i].notRecorded);
    uint64_t unused = 0, unused6 = 0;
    for (const ImplStats& is : s.impl) {
        unused += is.unused;
        unused6 += is.unused6;
    }
    Log("skinning [%s]: in the flush: GetSkeletonPose calls by class:%s | not recorded: no item at the flush's entry "
        "%llu, another LOD %llu, motion blur not predicted %llu, another class %llu | records not used=%llu (with motion "
        "blur %llu) | other callers in the flush=%llu | %.3f ms per flush in vanilla's calls, %.3f ms in the answers",
        when, calls, s.missNoObject, s.missLod, s.missMotionBlur, s.missUnknown, unused, unused6, s.otherCaller,
        double(s.vanillaCycles) / 3.0e6 / f, double(s.replayCycles) / 3.0e6 / f);
    char unusedBy[320] = "";
    n = 0;
    for (int i = 0; i < kSkinImpls; ++i)
        Append(unusedBy, sizeof unusedBy, n, " %s %llu (with motion blur %llu, morph targets %llu);", kImplNames[i],
               s.impl[i].unused, s.impl[i].unused6, s.impl[i].unusedMorph);
    Log("skinning [%s]: records not used, by class:%s", when, unusedBy);
    Log("skinning [%s]: morph targets: records with their copy %llu (not recorded %llu), %.3f MB per flush; video buffers "
        "locked for the copy %llu",
        when, s.morphRecords, s.morphNotRecorded, double(s.morphBytes) / f / 1048576.0, s.morphLocked);
    Log("skinning [%s]: bones: records answered with copies %llu (%.1f KB per flush), not copied: read past the call's "
        "count %llu, nothing read %llu | motion-blur poses computed once more before their copy %llu",
        when, s.boneRecords, double(s.boneBytes) / f / 1024.0, s.bonesNotCopied, s.bonesUnread, s.mbAgain);
    if (g.verify) {
        char line[1024] = "";
        n = 0;
        for (int i = 0; i < kSkinImpls; ++i)
            Append(line, sizeof line, n,
                   "%s %s compared=%llu same=%llu differ=%llu (matrix differing %llu; bone copies compared %llu, "
                   "differing %llu; morph targets compared %llu, differing %llu; their stream checked %llu, differing "
                   "%llu)",
                   i ? ";" : "", kImplNames[i], s.impl[i].vCompared, s.impl[i].vSame, s.impl[i].vDiffer, s.impl[i].vMatrix,
                   s.impl[i].vCopies, s.impl[i].vCopiesDiffer, s.impl[i].vMorphCompared, s.impl[i].vMorphDiffer,
                   s.impl[i].vMorphDest, s.impl[i].vMorphDestDiffer);
        char what[256] = "";
        size_t m = 0;
        for (const Count& c : s.vWhat)
            if (c.what) Append(what, sizeof what, m, " %s %llu", c.what, c.n);
        Log("skinning [%s]: VERIFY the flush's calls against the records:%s%s%s", when, line, m ? " | differing in:" : "",
            what);
    }
}
#endif  // _WIN64

}  // namespace

bool SkinPrepass_LocateRenderer(char* base, SkinTarget* out, const char** why) {
#ifndef _WIN64
    (void)base;
    (void)out;
    *why = "64-bit renderer only";
    return false;
#else
    char* site = Unique(base, kCallSite, why, "the skinned draw's GetSkeletonPose call not found");
    if (!site) return false;
    void** mesh = FindRttiVtable(base, ".?AVCREMesh@@");
    char* skinned = mesh && IsInImage(base, reinterpret_cast<const char*>(mesh), (kElemSkinnedSlot + 1) * sizeof(void*))
                        ? Entry(base, CallTrace_Original(mesh[kElemSkinnedSlot]))
                        : nullptr;
    if (!Matches(base, skinned, kIsHWSkinned)) {
        *why = "CREMesh::mfIsHWSkinned is not the expected code";
        return false;
    }
    out->callReturn = site + kCallSiteReturn;
    out->isHWSkinned = skinned;
    return true;
#endif
}

int SkinPrepass_ElementKind(char* base, const SkinTarget& t, void* const* vt) {
#ifndef _WIN64
    (void)base;
    (void)t;
    (void)vt;
    return 2;
#else
    char* fn = Entry(base, CallTrace_Original(vt[kElemSkinnedSlot]));
    if (fn && fn == t.isHWSkinned) return kElemMesh;
    if (Matches(base, fn, kReturnFalse)) return kElemNever;
    return kElemCall;
#endif
}

bool SkinPrepass_LocateSkinnables(char* anim, char* e3d, SkinTarget* out, const char** why) {
#ifndef _WIN64
    (void)anim;
    (void)e3d;
    (void)out;
    *why = "64-bit game only";
    return false;
#else
    char* const bases[kSkinImpls] = {anim, anim, e3d};
    void** vtables[kSkinImpls] = {};
    char* poses[kSkinImpls] = {};
    for (int i = 0; i < kSkinImpls; ++i) {
        vtables[i] = FindRttiVtable(bases[i], kRtti[i]);
        if (!vtables[i] || !IsInImage(bases[i], reinterpret_cast<const char*>(vtables[i]), (kPoseSlot + 1) * sizeof(void*))) {
            *why = kNoVtable[i];
            return false;
        }
        poses[i] = Entry(bases[i], CallTrace_Original(vtables[i][kPoseSlot]));
        if (!Matches(bases[i], poses[i], kPatterns[i])) {
            *why = kOtherCode[i];
            return false;
        }
    }
    for (int i = 0; i < kSkinImpls; ++i) {
        out->vtables[i] = vtables[i];
        out->poses[i] = poses[i];
    }
    return true;
#endif
}

bool SkinPrepass_LocateMorphCopy(char* anim, SkinTarget* out, const char** why) {
#ifndef _WIN64
    (void)anim;
    (void)out;
    *why = "64-bit game only";
    return false;
#else
    char* copy = Unique(anim, kMorphCopy, why, "MorphTargetCopy's copy not found");
    char* sequence = copy ? Unique(anim, kMorphSequence, why, "MorphTargetCopy's morph sequence not found") : nullptr;
    if (!sequence) return false;
    char* field = copy + kMorphCopyRenderer;
    void** renderer = reinterpret_cast<void**>(field + 4 + int32_t(ReadU32(field)));
    if (!IsInImage(anim, reinterpret_cast<const char*>(renderer), sizeof(void*))) {
        *why = "MorphTargetCopy's renderer pointer is outside CryAnimation.dll";
        return false;
    }
    out->morphCopy = copy;
    out->morphSequence = sequence;
    out->animRenderer = renderer;
    return true;
#endif
}

void SkinPrepass_Install(const wchar_t* moduleName, char* base, const ShadowPrepassTarget& sp) {
#ifndef _WIN64
    (void)base;
    (void)sp;
    if (ReadMode() != SMode::Off) Log("%ls: skinning poses not moved: 64-bit game only", moduleName);
#else
    if (g.installed) return;
    g.mode = ReadMode();
    if (g.mode == SMode::Off) return;
    const char* why = nullptr;
    if (!SkinPrepass_LocateRenderer(base, &g.t, &why)) {
        Log("%ls: skinning poses left in the flush: %s", moduleName, why);
        return;
    }
    g.shadow = &sp;
    g.base = base;
    g.verify = g.mode == SMode::Verify;
    g.installed = true;
    Log("%ls: skinning: mode=%s, the skinned draw's GetSkeletonPose call returns at +0x%X, CREMesh::mfIsHWSkinned "
        "+0x%X (the skinnables' code is found at the first flush)",
        moduleName, ModeName(g.mode), unsigned(g.t.callReturn - base), unsigned(g.t.isHWSkinned - base));
#endif
}

void SkinPrepass_BeforeFlush(char* renderer) {
#ifdef _WIN64
    if (!g.installed) return;
    if (!g.setupTried) Setup(renderer);
    if (!g.ready || GetCurrentThreadId() != g.mainThread || renderer != g.renderer) return;
    ++g.st.flushes;
    Prerun();
    g.inFlush = true;
#else
    (void)renderer;
#endif
}

void SkinPrepass_AfterFlush() {
#ifdef _WIN64
    if (!g.inFlush) return;
    g.inFlush = false;
    for (const Rec& r : g.recs) {
        if (r.calls) continue;
        ImplStats& is = g.st.impl[r.impl];
        ++is.unused;
        if (r.flagsIn & kFlagsMotionBlur) ++is.unused6;
        if (r.morph) ++is.unusedMorph;
        if (++g.loggedUnused <= 10)
            Log("skinning: flush %llu: %s %p LOD %u flags %u was asked for before the flush but not in it", g.st.flushes,
                kImplNames[r.impl], r.skinnable, unsigned(r.lod), unsigned(r.flagsIn));
    }
#endif
}

void SkinPrepass_Report(const char* when) {
#ifdef _WIN64
    if (g.installed && g.ready) Report(when);
#else
    (void)when;
#endif
}

}  // namespace optiperf
