// Engine data: see engine_data.h; the copies' bookkeeping is in range_copies.h.
//
// What the renderer reads, from where, and what rewrites it (x64; CryRenderD3D10 RVAs unless marked; details in
// research/docs/render-thread.md §6):
//   - The sky. SSkyLightRenderParams (0xD8 bytes: the sky dome's mesh; the size, place, pitch and time stamp of its
//     Mie and Rayleigh texel data; the scattering constants and colours) lies inside the 3D engine's CSkyLightManager
//     (+0x1A8), which rewrites its own frame id every frame (+0xE4) and the parameters when an incremental update of
//     the sky completes (as the time of day moves). The renderer keeps a pointer to them (the field
//     SetSkyLightRenderParams, IRenderer slot 193, sets), and so does the sky's render element (CREHDRSky +0x30). The
//     frame's shader constants (+0x3C280) and the sky dome's draw (+0x9C300) read them, and the draw uploads the texel
//     data (0x80 x 0x40 texels, 128 KB each) whenever the time stamp differs from the one it uploaded last (the
//     element's +0x3C), or its textures were made again. The shader constants also keep the pointer in a static of
//     the renderer's (once a frame, +0x658E50), and a reflection's replay in the flush calls SetSkyLightRenderParams
//     again with the pointer its traversal passed (the reflection part's recorded setters).
//   - The ocean. COcean (0x3B0 bytes, the terrain's +0x1F8) rewrites in its Render (Cry3DEngine +0xC5D00), every frame,
//     its grid's parameters (+0x90, 0x218 bytes of the 3D engine's copied in; +0x2A8, the waves), which the grid's
//     render object points at (+0x128 and the custom data +0xB8; its m_pID, +0xF0, is the COcean); and in RenderFog
//     (+0xC5440) its fog volume's: a water volume's parameters (+0x2E8, 0x50 bytes) with the vertices it lays around
//     the camera (0x14 bytes each) and their indices, and the ocean's own (+0x338). The fog's CREWaterVolume points at
//     those two (+0x30, +0x38).
//   - The water volumes. A CWaterVolumeRenderNode keeps its CREWaterVolumes' parameters (+0x50, 0x50 bytes: vertices,
//     indices and their counts, whether the camera is inside or close to it, the fog's plane, density and colour),
//     which its Render rewrites as the camera moves. Their vertices and indices change only when the volume is made,
//     so they are not copied.
//   - The far trees' terrain colour. Each far-tree sprite quad (the renderer's list, 0x38 bytes each, made before the
//     flush with FarTrees=on) points at its terrain node's texture set (+8: the texture's id, offsets and scale; at
//     CTerrainNode +0xD4, rewritten as the terrain's textures stream), which the sprites' outdoor ambient occlusion
//     reads (+0xC40A0, from DrawObjSprites): 0x14 bytes, 0x18 copied.
//   - The render elements' custom data (CREMesh +0x10, set by CRenderMesh::SetRECustomData, slot 25, on each chunk's
//     element): float arrays of the 3D engine's. The terrain sectors' are their node's texture coordinate generation
//     (a 0x48-byte array at CTerrainNode +0xC0, a float4 per recursion level, rewritten by SetupTexturing as the
//     terrain's textures stream); a big decal's lie inside it (CDecal +0x90, in the decal manager's array); a voxel
//     object's are its own. The shader parameters read floats from it by an index (+0x3FD75: up to 255) and the
//     terrain's texture coordinates three of them (+0x387A0), through the element drawn, or, for a mesh that draws
//     from another's vertices (its render mesh's +0x80), through that mesh's first chunk's element. Each is copied 1 KB
//     (an index's reach) from its pointer, up to its block's end when the allocator knows it; overlapping windows are
//     merged into one copy.
//
// At the hand-off (the outer EF_EndEf3D's entry, after the other parts' captures and the lights, before the census),
// each is copied with a pristine twin (range_copies.h), and the renderer's pointers to it are pointed at the copy: the
// renderer's sky field, the shader constants' static and the reflections' recorded setters; among the items the
// flush will draw (level 1's lists, the shadow pre-pass's stashes, the
// reflections' captures), the sky elements' and the water volumes' parameters, the mesh elements' custom data (and
// their vertex containers' first elements'), and every render object's custom data and +0x128 that point into a copy;
// the quads' texture sets. The copy of the ocean's fog parameters points at copies of its vertices and
// indices, the sky's copy at copies of its texel data (kept across flushes; copied again when the time stamp, or the
// data's size or place, changes). After the flush the bytes it wrote into the copies are written into the originals,
// and the pointers are put back. The copies are kept until the flush after next (two sets, used in turn), as a render
// thread will need: a pointer still at the last flush's copy at the next hand-off (something copied it during the
// flush) is put back to its original then, and counted and logged.
//
// verify: as on, plus searches: at the hand-off for pointers into the originals left after the redirect (in the items'
// objects and elements, the renderer object, the quads and the copies), after the flush for pointers into the copies
// kept in the renderer object, the objects, the elements and the quads. Also which copies the flush wrote, and which
// originals changed while it ran (none should: the flush reads the copies); the first of those are logged.

#include "engine_data.h"

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

#include "allocator.h"
#include "calltrace.h"
#include "config.h"
#include "log.h"
#include "range_copies.h"
#include "reflection_prepass.h"
#include "scan.h"
#include "shadow_prepass.h"
#include "shadow_prepass_lists.h"

namespace optiperf {
namespace {

enum class EMode { Off, On, Verify };

EMode ReadMode() {
    const std::wstring v =
        ConfigString(L"ShadowPrepass", L"EngineData", L"OPTIPERF_SHADOW_PREPASS_ENGINE_DATA", L"off");
    if (_wcsicmp(v.c_str(), L"on") == 0 || v == L"1") return EMode::On;
    if (_wcsicmp(v.c_str(), L"verify") == 0) return EMode::Verify;
    return EMode::Off;
}

#ifdef _WIN64
const char* ModeName(EMode m) { return m == EMode::On ? "on" : m == EMode::Verify ? "verify" : "off"; }

// IRenderer::SetSkyLightRenderParams (Mod SDK slot 193): mov [rcx + field], rdx; ret
constexpr int kSlotSkyParams = 193;
const char* const kSkyParamsSetter = "48 89 91 ?? ?? ?? ?? C3";
constexpr int kSetterField = 3;
constexpr int kMaxSkyField = 0x30000;  // (the renderer object's fields end at +0x2CEEC)
// the frame's shader constants (+0x3C280): the renderer's sky field kept in a static (mov [rip+X], rcx) before reading
// the parameters' Mie and Rayleigh constants (+0x7C)
const char* const kSkyCacheCode =
    "48 8B 05 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 48 89 0D ?? ?? ?? ?? F3 0F 10 51 7C";
constexpr int kSkyCacheField = 10, kSkyCacheStore = 0x17;  // the renderer field's offset; the store (disp at +3)

// CRendElement (Mod SDK RendElement.h): m_Type, and the types read here (and kTypeMesh below)
constexpr int kEType = 0x08;
constexpr int32_t kTypeHDRSky = 24, kTypeWaterVolume = 26;
// the sky: CREHDRSky's pointer at SSkyLightRenderParams (I3DEngine.h), and that
constexpr int kSkyParams = 0x30;
constexpr size_t kSkyBytes = 0xD8;
constexpr int kSkyWidth = 0x08, kSkyHeight = 0x0C, kSkyMie = 0x10, kSkyRayleigh = 0x18, kSkyPitch = 0x20,
              kSkyStamp = 0x28;
constexpr uint64_t kMaxTexels = 4u << 20;  // texel data copied, at most (128 KB each in the game)
// CREWaterVolume: its parameters (SParams), and the ocean's (the ocean's fog volume only)
constexpr int kWvParams = 0x30, kWvOcean = 0x38;
constexpr size_t kWvBytes = 0x50;
constexpr int kWvVerts = 0x00, kWvInds = 0x08, kWvNumVerts = 0x10, kWvNumInds = 0x18;
constexpr uint64_t kVertexBytes = 0x14, kIndexBytes = 2;
constexpr uint64_t kMaxFogVerts = 1u << 18, kMaxFogInds = 1u << 20;
// COcean
constexpr size_t kOceanBytes = 0x3B0;
constexpr int kOceanGrid = 0x90, kOceanWaves = 0x2A8, kOceanFog = 0x2E8, kOceanFogOcean = 0x338;
constexpr int kOceanVertVector = 0x380, kOceanIndVector = 0x398;  // the vectors the fog's arrays are (their begin)
// CRenderObject
constexpr size_t kObjectBytes = 0x130;
constexpr int kOCustom = 0xB8, kOID = 0xF0, kOUnion = 0x128;
// a far-tree sprite quad, and the terrain texture set it points at (CTerrainNode +0xD4)
constexpr size_t kQuadBytes = 0x38, kTexSetBytes = 0x18;
constexpr int kQuadTexSet = 0x08;
// CREMesh: its custom data, its render mesh; a CRenderMesh's vertex container and chunks (0x48 bytes each, the
// element at +0x20)
constexpr int32_t kTypeMesh = 9;
constexpr int kMeshCustom = 0x10, kMeshRenderMesh = 0x48, kRmContainer = 0x80, kRmChunks = 0x108, kChunkElement = 0x20;
constexpr size_t kCustomWindow = 0x400;    // an index's reach: 256 floats
constexpr size_t kMaxMerged = 256u << 10;  // a merged window, at most

constexpr size_t kArenaBytes = 16u << 20;
constexpr size_t kRendererBytes = 0x2CEF0;  // the renderer object's fields (the census found them up to +0x2CEEC)
constexpr size_t kElementBytes = 0x100;     // a render element, as far as verify searches it
constexpr int kLoggedStale = 10;            // pointers still at the last flush's copies, logged
constexpr int kLoggedChanged = 10;          // originals that changed while the flush ran, logged
constexpr int kPlaces = 16;                 // distinct (source, offset, kind) a search keeps for the report

enum Kind { kSky, kOcean, kFogVerts, kFogInds, kWater, kTexSet, kCustom, kKinds };
const char* const kKindNames[kKinds + 1] = {"sky",          "ocean",       "ocean fog vertices", "ocean fog indices",
                                            "water volume", "texture set", "custom data",        "a copy"};
static_assert(kKinds <= rc::kKinds, "the range copies count this many kinds");
constexpr int kIntoCopy = kKinds;  // (what the kept search finds)

enum Src { kSrcObject, kSrcElement, kSrcRenderer, kSrcQuad, kSrcCopy, kSrcs };
const char* const kSrcNames[kSrcs] = {"CRenderObject", "element", "renderer", "sprite quad", "copy"};

struct Found {  // pointers a search found, by where they were and what they point into
    uint64_t total = 0;
    int places = 0, logged = 0;
    struct {
        int src;
        uint32_t offset;
        int into;
        uint64_t count;
    } at[kPlaces] = {};
    void Add(int src, uint32_t offset, int into) {
        ++total;
        for (int i = 0; i < places; ++i)
            if (at[i].src == src && at[i].offset == offset && at[i].into == into) {
                ++at[i].count;
                return;
            }
        if (places < kPlaces) at[places++] = {src, offset, into, 1};
    }
};

struct Texels {  // the sky's texel data, copied: kept across flushes
    std::vector<char> mie, rayleigh;
    const char* mieFrom = nullptr;
    const char* rayleighFrom = nullptr;
    int32_t stamp = 0;
    uint32_t width = 0, height = 0;
    uint64_t pitch = 0;
};

struct Stats {
    uint64_t flushes = 0;
    uint64_t sky = 0, skyElements = 0, skyOther = 0, texelCopies = 0, texelBytes = 0, texelSkipped = 0;
    uint64_t oceans = 0, oceanObjects = 0, oceanFogs = 0, fogVertBytes = 0, fogIndBytes = 0, fogImplausible = 0;
    uint64_t waters = 0, waterElements = 0, oceanImplausible = 0;
    uint64_t quadFlushes = 0, quads = 0, texSets = 0, quadFields = 0;
    uint64_t customFields = 0, customFromContainers = 0, customPointers = 0, customRanges = 0, customBytes = 0;
    uint64_t customNotRedirected = 0, objectFields = 0;
    uint64_t stale = 0, staleOlder = 0;  // pointers at the last flush's copies at the hand-off (put back); older
    uint64_t skySetters = 0, skyCache = 0, skyAfter = 0;  // recorded setters, the static, put back after the flush
    uint64_t unreadable = 0, refused = 0;
    uint64_t restored = 0, writtenBytes = 0, translated = 0;
    uint64_t arenaMax = 0;
    uint64_t copyCycles = 0, afterCycles = 0;
    // the hand-off's steps: the sky, the items' first walk (their elements), the custom data's windows, the items'
    // second walk (their objects), the far trees' quads
    uint64_t stepCycles[5] = {};
    uint64_t customCycles[4] = {};  // the custom data's: the windows, their merge, their copies, the fields
    // verify
    uint64_t vWritten[kKinds] = {}, vWrittenBytes[kKinds] = {}, vChanged[kKinds] = {}, vChangedBytes[kKinds] = {};
    uint64_t vUnreadable = 0;
    Found missed, kept;
};

struct State {
    EMode mode = EMode::Off;
    bool installed = false, ready = false, active = false;
    const ShadowPrepassTarget* sp = nullptr;
    char* base = nullptr;
    size_t size = 0;
    DWORD mainThread = 0;
    char* renderer = nullptr;
    int skyField = 0;            // the renderer's field SetSkyLightRenderParams sets (0: not found, not redirected)
    char* skyParams = nullptr;   // this flush's sky parameters (the original) whose texel data is in place
    char** skyCache = nullptr;   // the shader constants' static holding the sky field (null: not found)
    int skyCacheField = 0;       // the renderer field its code reads (must be skyField)
    std::vector<char*> skyElements;  // this flush's sky elements
    rc::RangeCopies sets[2];  // this flush's copies and the last one's, used in turn
    int cur = 0;
    Texels texels;
    std::vector<char> scratch;   // what a range is read into before it is copied
    std::vector<char*> fogDone;  // oceans whose fog vertices and indices this flush copied
    struct Custom {
        char* field;  // an element's custom data field
        char* data;   // what it points at
    };
    std::vector<Custom> customs;  // this flush's
    std::vector<std::pair<const char*, const char*>> windows;  // their windows, then merged
    char* quads = nullptr;       // this flush's far-tree quads (made before it), and how many
    int quadCount = 0;
    std::vector<char> buffer;    // verify: what a search reads
    int loggedChanged = 0, loggedStale = 0;
    Stats st;
};
State g;

rc::RangeCopies& Cur() { return g.sets[g.cur]; }
rc::RangeCopies& Last() { return g.sets[g.cur ^ 1]; }

bool SafeRead(const char* p, char* out, size_t n) {
    __try {
        std::memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t U64(const char* p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

uint32_t U32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

void Append(char* line, size_t size, size_t& n, const char* fmt, ...) {
    if (n >= size) return;
    va_list a;
    va_start(a, fmt);
    const int k = vsnprintf(line + n, size - n, fmt, a);
    va_end(a);
    if (k > 0) n = (std::min)(size - 1, n + size_t(k));
}

// --- the items the flush will draw (as the light part walks them) ----------------------------------------------------
using ItemFn = void (*)(char* obj, char* elem);

void StashItems(const sp::Stash& s, void* ctx) {
    const ItemFn fn = reinterpret_cast<ItemFn>(ctx);
    const sp::RendItem* it = s.items.data();
    for (int k = 0; k < sp::kLists; ++k) {
        for (uint32_t i = 0; i < s.counts[k]; ++i) fn(it[i].obj, it[i].elem);
        it += s.counts[k];
    }
}

void ForEachItem(ItemFn fn) {
    const auto* lists = reinterpret_cast<const sp::ItemList*>(g.sp->lists);
    const uint32_t* starts = g.sp->starts;
    for (int k = 0; k < sp::kLists; ++k)
        for (uint32_t i = starts[k]; i < lists[k].count; ++i) fn(lists[k].elems[i].obj, lists[k].elems[i].elem);
    ShadowPrepass_ForEachStash(&StashItems, reinterpret_cast<void*>(fn));
    ReflectionPrepass_ForEachCapture(&StashItems, reinterpret_cast<void*>(fn));
}

// --- the copies ------------------------------------------------------------------------------------------------------
// A field that still points at the last flush's copy (something copied the pointer during that flush, or kept it):
// put back to the original it stood for, counted, the first ones logged. One at this flush's copies was pointed there
// already (an item met twice); one elsewhere in this flush's set is the flush before last's, and can't be put back.
void Unstale(char* field, const char* what) {
    char* const v = rc::Get(field);
    if (!v || Cur().InArena(v)) return;
    if (Cur().InRegion(v)) {
        ++g.st.staleOlder;
        return;
    }
    if (!Last().InArena(v)) return;
    char* const o = Last().Unmap(v);
    ++g.st.stale;
    if (g.loggedStale < kLoggedStale) {
        ++g.loggedStale;
        Log("engine data: flush %llu: %s at %p still pointed at the last flush's copy %p (of %p): put back", g.st.flushes,
            what, static_cast<void*>(field), static_cast<void*>(v), static_cast<void*>(o));
    }
    if (o) rc::Set(field, o);
}

// [p, p + n) copied as a range of `kind` (or the copy that holds it already): its copy, or null if it can't be read or
// the arena won't take it; *fresh, if set: copied now
char* CopyRange(const char* p, size_t n, int kind, bool* fresh = nullptr) {
    if (fresh) *fresh = false;
    if (Cur().InArena(p)) return const_cast<char*>(p);  // (a copy already: an item met twice)
    if (char* c = Cur().Find(p, n)) return c;
    if (g.scratch.size() < n) g.scratch.resize(n);
    if (!SafeRead(p, g.scratch.data(), n)) {
        ++g.st.unreadable;
        return nullptr;
    }
    char* const c = Cur().Add(p, n, kind, g.scratch.data());
    if (!c) ++g.st.refused;
    if (fresh) *fresh = c != nullptr;
    return c;
}

// The sky copy's texel data: the copies kept from an earlier flush, or copied again if the original's time stamp,
// size or place changed. Without them (too big, unreadable) the copy keeps the original's pointers.
void CopyTexels(char* c) {
    Texels& t = g.texels;
    const char* const mie = rc::Get(c + kSkyMie);
    const char* const rayleigh = rc::Get(c + kSkyRayleigh);
    const uint32_t width = U32(c + kSkyWidth), height = U32(c + kSkyHeight);
    const int32_t stamp = int32_t(U32(c + kSkyStamp));
    const uint64_t pitch = U64(c + kSkyPitch), bytes = uint64_t(height) * pitch;
    if (!mie || !rayleigh || !bytes) return;  // (no texel data yet)
    if (bytes > kMaxTexels) {
        ++g.st.texelSkipped;
        return;
    }
    if (mie != t.mieFrom || rayleigh != t.rayleighFrom || stamp != t.stamp || width != t.width || height != t.height ||
        pitch != t.pitch || t.mie.size() != bytes) {
        t.mie.resize(size_t(bytes));
        t.rayleigh.resize(size_t(bytes));
        t.mieFrom = t.rayleighFrom = nullptr;
        if (!SafeRead(mie, t.mie.data(), size_t(bytes)) || !SafeRead(rayleigh, t.rayleigh.data(), size_t(bytes))) {
            ++g.st.unreadable;
            return;
        }
        t.mieFrom = mie;
        t.rayleighFrom = rayleigh;
        t.stamp = stamp;
        t.width = width;
        t.height = height;
        t.pitch = pitch;
        ++g.st.texelCopies;
        g.st.texelBytes += 2 * bytes;
    }
    Cur().GivePointer(c + kSkyMie, t.mie.data());
    Cur().GivePointer(c + kSkyRayleigh, t.rayleigh.data());
}

// the sky's parameters copied (once a flush, with their texel data): their copy, or null
char* CopySky(char* p) {
    bool fresh = false;
    char* const c = CopyRange(p, kSkyBytes, kSky, &fresh);
    if (fresh) {
        ++g.st.sky;
        CopyTexels(c);
    }
    return c;
}

// the ocean's fog vertices and indices, copied, the copy of its fog parameters pointed at them
void CopyFogArrays(char* oceanCopy) {
    char* const params = oceanCopy + kOceanFog;
    const char* const verts = rc::Get(params + kWvVerts);
    const char* const inds = rc::Get(params + kWvInds);
    const uint64_t nv = U64(params + kWvNumVerts), ni = U64(params + kWvNumInds);
    if ((nv && !verts) || (ni && !inds) || nv > kMaxFogVerts || ni > kMaxFogInds) {
        ++g.st.fogImplausible;
        return;
    }
    // (the vectors the arrays are, at their begin, too: nothing reads them there, but the copy then holds no pointer
    // into an original)
    if (nv)
        if (char* v = CopyRange(verts, size_t(nv * kVertexBytes), kFogVerts)) {
            Cur().GivePointer(params + kWvVerts, v);
            if (rc::Get(oceanCopy + kOceanVertVector) == verts) Cur().GivePointer(oceanCopy + kOceanVertVector, v);
            g.st.fogVertBytes += nv * kVertexBytes;
        }
    if (ni)
        if (char* i = CopyRange(inds, size_t(ni * kIndexBytes), kFogInds)) {
            Cur().GivePointer(params + kWvInds, i);
            if (rc::Get(oceanCopy + kOceanIndVector) == inds) Cur().GivePointer(oceanCopy + kOceanIndVector, i);
            g.st.fogIndBytes += ni * kIndexBytes;
        }
}

// an ocean copied (once a flush), with its fog's vertices and indices if its fog volume is drawn: its copy, or null
char* CopyOcean(char* ocean, bool fog) {
    bool fresh = false;
    char* const c = CopyRange(ocean, kOceanBytes, kOcean, &fresh);
    if (!c) return nullptr;
    if (fresh) ++g.st.oceans;
    if (fog && std::find(g.fogDone.begin(), g.fogDone.end(), ocean) == g.fogDone.end()) {
        g.fogDone.push_back(ocean);
        CopyFogArrays(c);
    }
    return c;
}

void SkyElement(char* elem) {
    Unstale(elem + kSkyParams, "a sky element's parameters");
    g.skyElements.push_back(elem);
    char* const p = rc::Get(elem + kSkyParams);
    if (!p || Cur().InArena(p)) return;     // (none, or met before in this flush: at the copy already)
    if (p != g.skyParams) ++g.st.skyOther;  // (not the renderer's: copied all the same)
    if (CopySky(p) && Cur().RedirectField(elem + kSkyParams)) ++g.st.skyElements;
}

void WaterElement(char* elem) {
    Unstale(elem + kWvParams, "a water volume element's parameters");
    Unstale(elem + kWvOcean, "the ocean fog element's ocean parameters");
    char* const params = rc::Get(elem + kWvParams);
    if (!params || Cur().InArena(params)) return;  // (met before in this flush: at the copy already)
    char* const ocean = rc::Get(elem + kWvOcean);
    if (ocean) {  // the ocean's fog volume: both inside the COcean
        if (ocean - params != kOceanFogOcean - kOceanFog) {
            ++g.st.oceanImplausible;
            return;
        }
        if (!CopyOcean(params - kOceanFog, true)) return;
        ++g.st.oceanFogs;
        Cur().RedirectField(elem + kWvOcean);
    } else {
        bool fresh = false;
        if (!CopyRange(params, kWvBytes, kWater, &fresh)) return;
        if (fresh) ++g.st.waters;
    }
    if (Cur().RedirectField(elem + kWvParams)) ++g.st.waterElements;
}

// the ocean grid's render object: its m_pID is the COcean, its custom data and +0x128 point into it (an object met
// before points at the copy already, and fails the test)
void OceanObject(char* obj) {
    Unstale(obj + kOCustom, "a render object's custom data");
    Unstale(obj + kOUnion, "a render object's +0x128");
    char* const id = rc::Get(obj + kOID);
    if (!id || rc::Get(obj + kOCustom) != id + kOceanWaves || rc::Get(obj + kOUnion) != id + kOceanGrid) return;
    if (!CopyOcean(id, false)) return;
    Cur().RedirectField(obj + kOCustom);
    Cur().RedirectField(obj + kOUnion);
    ++g.st.oceanObjects;
}

// an element's custom data, to be copied once the flush's are all known (the windows merged)
void CollectCustom(char* elem) {
    Unstale(elem + kMeshCustom, "a mesh element's custom data");
    char* const data = rc::Get(elem + kMeshCustom);
    if (data && !Cur().InArena(data)) g.customs.push_back({elem + kMeshCustom, data});
}

// a mesh element: its custom data, and that of its vertex container's first element (which the terrain's texture
// coordinates come from for a mesh drawn from another's vertices)
void MeshElement(char* elem) {
    CollectCustom(elem);
    char* const mesh = rc::Get(elem + kMeshRenderMesh);  // (its CRenderMesh)
    char *container = nullptr, *chunks = nullptr, *first = nullptr, *data = nullptr;
    if (!mesh || !SafeRead(mesh + kRmContainer, reinterpret_cast<char*>(&container), 8) || !container ||
        !SafeRead(container + kRmChunks, reinterpret_cast<char*>(&chunks), 8) || !chunks ||  // (PodArray<CRenderChunk>*)
        !SafeRead(chunks, reinterpret_cast<char*>(&chunks), 8) || !chunks ||                  // (its elements)
        !SafeRead(chunks + kChunkElement, reinterpret_cast<char*>(&first), 8) || !first || first == elem ||
        !SafeRead(first + kMeshCustom, reinterpret_cast<char*>(&data), 8) || !data || Cur().InArena(data))
        return;
    g.customs.push_back({first + kMeshCustom, data});
    ++g.st.customFromContainers;
}

void CopyItem(char* obj, char* elem) {
    if (elem) {
        int32_t type;
        std::memcpy(&type, elem + kEType, sizeof type);
        if (type == kTypeHDRSky)
            SkyElement(elem);
        else if (type == kTypeWaterVolume)
            WaterElement(elem);
        else if (type == kTypeMesh)
            MeshElement(elem);
    }
    if (obj) OceanObject(obj);
}

// The custom data: a window from each pointer (an index's reach, up to its block's end when the allocator knows it),
// the overlapping ones merged; each merged window copied, and the fields pointed into the copies. The pointers are
// taken in their order, each once, and the allocator asked once per block (a pointer below the last block's end is in
// that block).
void CopyCustoms() {
    const uint64_t t0 = __rdtsc();
    std::sort(g.customs.begin(), g.customs.end(),
              [](const State::Custom& a, const State::Custom& b) { return a.data < b.data; });
    g.windows.clear();
    const char *last = nullptr, *blockStart = nullptr, *blockEnd = nullptr;
    for (const State::Custom& c : g.customs) {
        if (c.data == last) continue;
        last = c.data;
        if (Cur().Find(c.data, 1)) continue;  // (in a range copied already)
        if (!(c.data >= blockStart && c.data < blockEnd)) {
            char* block = nullptr;
            size_t size = 0;
            const bool known = Allocator_BlockOf(c.data, &block, &size) && block + size > c.data;
            blockStart = known ? block : nullptr;
            blockEnd = known ? block + size : nullptr;
        }
        const char* const end = c.data + kCustomWindow;
        g.windows.push_back({c.data, blockEnd ? (std::min)(end, blockEnd) : end});
    }
    const uint64_t t1 = __rdtsc();
    g.st.customCycles[0] += t1 - t0;
    size_t n = 0;  // merged in place
    for (const auto& w : g.windows) {
        if (n && w.first <= g.windows[n - 1].second &&
            size_t((std::max)(w.second, g.windows[n - 1].second) - g.windows[n - 1].first) <= kMaxMerged) {
            g.windows[n - 1].second = (std::max)(g.windows[n - 1].second, w.second);
            continue;
        }
        g.windows[n++] = w;
    }
    g.windows.resize(n);
    g.st.customPointers += g.customs.size();
    const uint64_t t2 = __rdtsc();
    for (const auto& w : g.windows) {
        size_t bytes = size_t(w.second - w.first);
        // (unreadable as a whole: up to the end of its first page, where an unmapped page may follow)
        if (!CopyRange(w.first, bytes, kCustom)) {
            const size_t page = 0x1000 - (reinterpret_cast<uintptr_t>(w.first) & 0xFFF);
            if (page >= bytes || !CopyRange(w.first, page, kCustom)) continue;
            bytes = page;
        }
        ++g.st.customRanges;
        g.st.customBytes += bytes;
    }
    const uint64_t t3 = __rdtsc();
    for (const State::Custom& c : g.customs) {
        if (Cur().RedirectField(c.field))
            ++g.st.customFields;
        else if (!Cur().InArena(rc::Get(c.field)))  // (one met twice points at the copy already)
            ++g.st.customNotRedirected;
    }
    g.st.customCycles[1] += t2 - t1;
    g.st.customCycles[2] += t3 - t2;
    g.st.customCycles[3] += __rdtsc() - t3;
}

// Every drawn object's custom data and +0x128 (a union: a light, a camera, a float...) that point into a copy (the
// ocean's grid and fog objects, a decal's custom data) pointed at it
void RedirectObject(char* obj, char*) {
    if (!obj) return;
    for (int f : {kOCustom, kOUnion})
        Unstale(obj + f, f == kOCustom ? "a render object's custom data" : "a render object's +0x128");
    for (int f : {kOCustom, kOUnion})
        if (Cur().RedirectField(obj + f)) ++g.st.objectFields;
}

// the far-tree sprites' quads, made before the flush: their terrain texture sets
void CopyQuads() {
    g.quads = nullptr;
    g.quadCount = 0;
    char* quads = nullptr;
    int n = 0;
    if (!ReflectionPrepass_FarQuads(&quads, &n)) return;
    g.quads = quads;
    g.quadCount = n;
    ++g.st.quadFlushes;
    g.st.quads += uint64_t(n);
    char *lastSet = nullptr, *lastCopy = nullptr;  // (the quads of a terrain node come together, sharing its set)
    for (int i = 0; i < n; ++i) {
        char* const q = quads + size_t(i) * kQuadBytes;
        Unstale(q + kQuadTexSet, "a far-tree quad's texture set");
        char* const set = rc::Get(q + kQuadTexSet);
        if (!set) continue;
        if (set == lastSet && lastCopy) {
            Cur().RedirectFieldTo(q + kQuadTexSet, set, lastCopy);
            ++g.st.quadFields;
            continue;
        }
        bool fresh = false;
        char* const copy = CopyRange(set, kTexSetBytes, kTexSet, &fresh);
        lastSet = set;
        lastCopy = copy && copy != set ? copy : nullptr;  // (a set already in the arena: nothing to redirect)
        if (!copy) continue;
        if (fresh) ++g.st.texSets;
        if (Cur().RedirectField(q + kQuadTexSet)) ++g.st.quadFields;
    }
}

void HandOff(char* r) {
    const uint64_t t0 = __rdtsc();
    g.cur ^= 1;  // (the set of the flush before last: the last flush's stays, for Unstale)
    Cur().Clear();
    g.fogDone.clear();
    g.customs.clear();
    g.skyElements.clear();
    if (g.skyField) Unstale(r + g.skyField, "the renderer's sky field");
    if (g.skyCache) Unstale(reinterpret_cast<char*>(g.skyCache), "the shader constants' sky static");
    g.skyParams = g.skyField ? rc::Get(r + g.skyField) : nullptr;
    if (g.skyParams && CopySky(g.skyParams)) Cur().RedirectField(r + g.skyField);
    if (g.skyCache && *g.skyCache && CopySky(*g.skyCache) &&
        Cur().RedirectField(reinterpret_cast<char*>(g.skyCache)))
        ++g.st.skyCache;
    ReflectionPrepass_ForEachRecordedSkyLight(  // (used up by this flush: not put back)
        [](const void** p, void*) {
            char* const params = const_cast<char*>(static_cast<const char*>(*p));
            if (!params || Cur().InArena(params) || !CopySky(params)) return;
            *p = Cur().Map(params);
            ++g.st.skySetters;
        },
        nullptr);
    const uint64_t t1 = __rdtsc();
    ForEachItem(&CopyItem);
    const uint64_t t2 = __rdtsc();
    CopyCustoms();
    const uint64_t t3 = __rdtsc();
    ForEachItem(&RedirectObject);
    const uint64_t t4 = __rdtsc();
    CopyQuads();
    const uint64_t t5 = __rdtsc();
    const uint64_t at[6] = {t0, t1, t2, t3, t4, t5};
    for (int s = 0; s < 5; ++s) g.st.stepCycles[s] += at[s + 1] - at[s];
    g.st.arenaMax = (std::max)(g.st.arenaMax, uint64_t(Cur().Used()));
}

// --- verify's searches -----------------------------------------------------------------------------------------------
const char* ClassOf(const char* object) {
    __try {
        const void* const* vt = *reinterpret_cast<const void* const* const*>(object);
        const auto* col = static_cast<const uint32_t*>(vt[-1]);
        HMODULE m;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(col), &m) ||
            col[0] > 1)
            return nullptr;
        const char* name = reinterpret_cast<const char*>(m) + col[3] + 16;  // the type descriptor: vtable, spare, name
        return name[0] == '.' && name[1] == '?' ? name : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// what v points into among the originals (its kind), else -1
int IntoOriginal(const char* v) { return Cur().KindAt(v); }

int IntoCopies(const char* v) {
    if (Cur().InArena(v) || Last().InArena(v)) return kIntoCopy;
    const Texels& t = g.texels;
    if (!t.mie.empty() && v >= t.mie.data() && v < t.mie.data() + t.mie.size()) return kIntoCopy;
    if (!t.rayleigh.empty() && v >= t.rayleigh.data() && v < t.rayleigh.data() + t.rayleigh.size()) return kIntoCopy;
    return -1;
}

// the 8-byte-aligned pointers in [p, p + n) that `match` places, into `found` (a pointer given to a copy is not one);
// the first ones logged, with the class of what holds them (an object's: its render element's)
template <class Match>
void Search(const char* p, size_t n, int src, Match match, Found& found, const char* what) {
    if (!p || !n) return;
    if (g.buffer.size() < n) g.buffer.resize(n);  // (verify only: the quads can outgrow the renderer object)
    if (!SafeRead(p, g.buffer.data(), n)) {
        ++g.st.vUnreadable;
        return;
    }
    for (size_t o = 0; o + 8 <= n; o += 8) {
        const char* v;
        std::memcpy(&v, g.buffer.data() + o, sizeof v);
        if (reinterpret_cast<uintptr_t>(v) & 7) continue;  // (a pointer here is 8-byte aligned: not floats or shorts)
        const int into = match(v);
        if (into < 0) continue;
        if (src == kSrcCopy && Cur().Given(p + o)) continue;
        if (src == kSrcObject && o == size_t(kOID)) continue;  // (m_pID: an identity the flush doesn't follow)
        const int places = found.places;
        found.Add(src, uint32_t(o), into);
        if (found.places > places) {  // (the first of each place)
            const char* holder = src == kSrcObject && n >= 0xB0 ? rc::Get(g.buffer.data() + 0xA8)  // (m_pRE)
                                 : src == kSrcElement           ? p
                                                                : nullptr;
            const char* cls = holder ? ClassOf(holder) : nullptr;
            Log("engine data: VERIFY flush %llu: %s: %s %p +0x%zX (%s) points into %s: %p", g.st.flushes, what,
                kSrcNames[src], p, o, cls ? cls : "-", kKindNames[into], v);
        }
    }
}

// the quads' two pointers (+0, the sprite's texture; +8, the texture set): the rest are floats, bytes and padding
// that holds what the list's memory held before
template <class Match>
void SearchQuads(Match match, Found& found, const char* what) {
    for (int i = 0; g.quads && i < g.quadCount; ++i)
        Search(g.quads + size_t(i) * kQuadBytes, 16, kSrcQuad, match, found, what);
}

const char* const kMissed = "missed";
const char* const kKept = "kept";

// how far verify searches an element: its block, if the allocator knows it (a small one is followed by others)
size_t ElementBytes(const char* e) {
    char* block = nullptr;
    size_t size = 0;
    if (Allocator_BlockOf(e, &block, &size) && block + size > e)
        return (std::min)(kElementBytes, size_t(block + size - e));
    return kElementBytes;
}

// at the hand-off, after the redirect: pointers into the originals that are left
void FindMissed(char* r) {
    if (!Cur().Ranges()) return;
    ForEachItem([](char* obj, char* elem) {
        Search(obj, kObjectBytes, kSrcObject, IntoOriginal, g.st.missed, kMissed);
        if (elem) Search(elem, ElementBytes(elem), kSrcElement, IntoOriginal, g.st.missed, kMissed);
    });
    Search(r, kRendererBytes, kSrcRenderer, IntoOriginal, g.st.missed, kMissed);
    SearchQuads(IntoOriginal, g.st.missed, kMissed);
    for (size_t i = 0; i < Cur().Ranges(); ++i)
        if (Cur().KindOf(i) != kFogVerts && Cur().KindOf(i) != kFogInds)  // (floats and shorts)
            Search(Cur().CopyOf(i), Cur().Bytes(i) & ~size_t(7), kSrcCopy, IntoOriginal, g.st.missed, kMissed);
}

// after the flush, once the pointers are put back: pointers into the copies that are kept
void FindKept(char* r) {
    Search(r, kRendererBytes, kSrcRenderer, IntoCopies, g.st.kept, kKept);
    ForEachItem([](char* obj, char* elem) {
        Search(obj, kObjectBytes, kSrcObject, IntoCopies, g.st.kept, kKept);
        if (elem) Search(elem, ElementBytes(elem), kSrcElement, IntoCopies, g.st.kept, kKept);
    });
    SearchQuads(IntoCopies, g.st.kept, kKept);
}

// after the flush: what it wrote into the copies, and the originals that changed while it ran (the first logged)
void Compare() {
    rc::Diff written, changed;
    Cur().Compare(written, changed, [](size_t i, size_t first, size_t bytes) {
        if (g.loggedChanged >= kLoggedChanged) return;
        ++g.loggedChanged;
        const char* const o = Cur().Start(i);
        Log("engine data: VERIFY flush %llu: %s %p (%zu bytes) changed while the flush ran: %zu bytes, the first at +0x%zX",
            g.st.flushes, kKindNames[Cur().KindOf(i)], static_cast<const void*>(o), Cur().Bytes(i), bytes, first);
    });
    for (int k = 0; k < kKinds; ++k) {
        g.st.vWritten[k] += written.ranges[k];
        g.st.vWrittenBytes[k] += written.bytes[k];
        g.st.vChanged[k] += changed.ranges[k];
        g.st.vChangedBytes[k] += changed.bytes[k];
    }
}

void AfterFlush() {
    if (g.mode == EMode::Verify) Compare();
    size_t translated = 0;
    g.st.writtenBytes += Cur().WriteBack(&translated);
    g.st.translated += translated;
    g.st.restored += Cur().Restore();
    // a copy's pointer the flush left in the sky's places (a replayed setter's, the shader constants' static)
    if (g.skyField) g.st.skyAfter += Cur().UnmapRange(g.renderer + g.skyField, 8);
    if (g.skyCache) g.st.skyAfter += Cur().UnmapRange(reinterpret_cast<char*>(g.skyCache), 8);
    for (char* e : g.skyElements) g.st.skyAfter += Cur().UnmapRange(e + kSkyParams, 8);
    if (g.mode == EMode::Verify) FindKept(g.renderer);
}

void Where(const Found& f, char* line, size_t size) {
    size_t n = 0;
    line[0] = 0;
    for (int i = 0; i < f.places; ++i)
        Append(line, size, n, "%s%s+0x%X into %s:%llu", i ? ", " : "", kSrcNames[f.at[i].src], f.at[i].offset,
               kKindNames[f.at[i].into], f.at[i].count);
    if (!f.places) Append(line, size, n, "none");
}

void PerKind(const uint64_t* ranges, const uint64_t* bytes, char* line, size_t size) {
    size_t n = 0;
    line[0] = 0;
    for (int k = 0; k < kKinds; ++k)
        if (ranges[k]) Append(line, size, n, "%s%s %llu (%llu bytes)", n ? ", " : "", kKindNames[k], ranges[k], bytes[k]);
    if (!n) Append(line, size, n, "none");
}

void Report(const char* when) {
    const Stats& s = g.st;
    const double f = double(s.flushes ? s.flushes : 1);
    Log("engine data [%s]: mode=%s, flushes %llu | the sky: parameters copied in %llu flushes (the renderer's field %s; "
        "elements pointed at the copy %.1f per flush, %llu with other parameters), texel data copied %llu times (%.2f MB "
        "in all; not copied, too big: %llu; the shader constants' static pointed at the copy %llu times, the reflections' "
        "recorded setters %llu, copies' pointers put back after the flush %llu) | the ocean: copied in %llu flushes, its "
        "grid's objects pointed at the copy "
        "%.1f per flush, its fog volume %.1f (vertices %.1f KB, indices %.1f KB per flush; implausible %llu/%llu) | water "
        "volumes' parameters copied %.1f per flush (elements pointed at them %.1f) | far-tree quads made before the "
        "flush in %llu flushes, %.0f per flush: texture sets copied %.1f, quads pointed at them %.1f | the elements' "
        "custom data per flush: %.1f pointers (%.1f of them the vertex containers' elements'), copied in %.1f merged "
        "windows (%.1f KB), elements pointed at them %.1f (not: %llu) | objects' custom data and +0x128 pointed at "
        "copies %.1f per flush | pointers still at the last flush's copies at the hand-off, put back: %llu (older: "
        "%llu)",
        when, ModeName(g.mode), s.flushes, s.sky, g.skyField ? "found" : "not found", double(s.skyElements) / f,
        s.skyOther, s.texelCopies, double(s.texelBytes) / 1048576.0, s.texelSkipped, s.skyCache, s.skySetters,
        s.skyAfter, s.oceans,
        double(s.oceanObjects) / f, double(s.oceanFogs) / f, double(s.fogVertBytes) / 1024.0 / f,
        double(s.fogIndBytes) / 1024.0 / f, s.oceanImplausible, s.fogImplausible, double(s.waters) / f,
        double(s.waterElements) / f, s.quadFlushes, double(s.quads) / double(s.quadFlushes ? s.quadFlushes : 1),
        double(s.texSets) / f, double(s.quadFields) / f, double(s.customPointers) / f,
        double(s.customFromContainers) / f, double(s.customRanges) / f, double(s.customBytes) / 1024.0 / f,
        double(s.customFields) / f, s.customNotRedirected, double(s.objectFields) / f,
        s.stale, s.staleOlder);
    Log("engine data [%s]: unreadable %llu, not copied (the arena full, or a range overlapping one) %llu; the arena: most "
        "used %.1f KB | after the flush: fields put back %.1f per flush; bytes it wrote, written back %.1f per flush "
        "(pointers into the copies among them, written as the originals': %llu) | %.3f ms per flush before it, %.3f ms "
        "after",
        when, s.unreadable, s.refused, double(s.arenaMax) / 1024.0, double(s.restored) / f, double(s.writtenBytes) / f,
        s.translated, double(s.copyCycles) / 3.0e6 / f, double(s.afterCycles) / 3.0e6 / f);
    {
        const auto ms = [f](uint64_t cycles) { return double(cycles) / 3.0e6 / f; };
        Log("engine data [%s]: the hand-off's steps, ms per flush: the sky %.3f, the items' elements %.3f, the custom "
            "data %.3f (the windows %.3f, merged %.3f, copied %.3f, the fields pointed at them %.3f), the items' objects "
            "%.3f, the far trees' quads %.3f",
            when, ms(s.stepCycles[0]), ms(s.stepCycles[1]), ms(s.stepCycles[2]), ms(s.customCycles[0]),
            ms(s.customCycles[1]), ms(s.customCycles[2]), ms(s.customCycles[3]), ms(s.stepCycles[3]),
            ms(s.stepCycles[4]));
    }
    if (g.mode != EMode::Verify) return;
    char missed[900], kept[900], written[600], changed[600];
    Where(s.missed, missed, sizeof missed);
    Where(s.kept, kept, sizeof kept);
    PerKind(s.vWritten, s.vWrittenBytes, written, sizeof written);
    PerKind(s.vChanged, s.vChangedBytes, changed, sizeof changed);
    Log("engine data [%s]: VERIFY pointers into the originals the redirect missed: %llu (%s) | pointers into the copies "
        "kept after the flush: %llu (%s) | copies the flush wrote: %s | originals that changed while it ran: %s | "
        "unreadable memory skipped: %llu",
        when, s.missed.total, missed, s.kept.total, kept, written, changed, s.vUnreadable);
}

// The renderer's sky field, from SetSkyLightRenderParams' code (the renderer's vtable slot 193); 0 if it isn't the
// expected code.
int FindSkyField(char* r) {
    void** vt = *reinterpret_cast<void***>(r);
    void* entry = nullptr;  // (the vtable may be a copy of the renderer's, with the same entries)
    if (!SafeRead(reinterpret_cast<const char*>(vt + kSlotSkyParams), reinterpret_cast<char*>(&entry), sizeof entry))
        return 0;
    char* const fn = static_cast<char*>(CallTrace_Original(entry));
    const Pattern p = ParsePattern(kSkyParamsSetter);
    if (!IsInCode(g.base, fn, p.size()) || !MatchesAt(fn, p)) return 0;
    const int field = int(int32_t(ReadU32(fn + kSetterField)));
    return field > 0 && field < kMaxSkyField && field % 8 == 0 ? field : 0;
}
#endif  // _WIN64

}  // namespace

void EngineData_Install(const wchar_t* moduleName, char* base, size_t size, const ShadowPrepassTarget& sp) {
#ifndef _WIN64
    (void)base;
    (void)size;
    (void)sp;
    if (ReadMode() != EMode::Off) Log("%ls: engine data not copied: 64-bit game only", moduleName);
#else
    if (g.installed) return;
    g.mode = ReadMode();
    if (g.mode == EMode::Off) return;
    // the copies near the top of the address space: no small integer or float is a pointer into them
    const size_t setBytes = rc::RangeCopies::ArenaBytes(kArenaBytes);
    auto* const memory = static_cast<char*>(
        VirtualAlloc(nullptr, 2 * setBytes, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
    if (!memory) {
        Log("%ls: engine data: off: no memory for the copies", moduleName);
        return;
    }
    g.sets[0].Init(kArenaBytes, memory);
    g.sets[1].Init(kArenaBytes, memory + setBytes);
    g.sp = &sp;
    g.base = base;
    g.size = size;
    const std::vector<char*> cache = FindInCode(base, ParsePattern(kSkyCacheCode), 2);
    if (cache.size() == 1) {
        char* const store = cache[0] + kSkyCacheStore;  // (mov [rip + disp32], rcx: 7 bytes)
        g.skyCache = reinterpret_cast<char**>(store + 7 + int32_t(ReadU32(store + 3)));
        g.skyCacheField = int(int32_t(ReadU32(cache[0] + kSkyCacheField)));
    }
    if (g.mode == EMode::Verify) g.buffer.resize(kRendererBytes);
    g.installed = true;
    Log("%ls: engine data: mode=%s (the sky, the ocean, the water volumes, the far trees' terrain texture sets); the "
        "renderer's sky field is looked for at the first flush",
        moduleName, ModeName(g.mode));
#endif
}

void EngineData_BeforeFlush(char* r) {
#ifdef _WIN64
    if (!g.installed) return;
    if (!g.ready) {  // the first flush: the main thread, the renderer's sky field
        g.mainThread = GetCurrentThreadId();
        g.skyField = FindSkyField(r);
        if (g.skyCache && g.skyCacheField != g.skyField) g.skyCache = nullptr;
        if (g.skyField)
            Log("engine data: the renderer's sky parameters at +0x%X (SetSkyLightRenderParams); the shader constants' "
                "static %s",
                unsigned(g.skyField), g.skyCache ? "found" : "not found (left alone)");
        else
            Log("engine data: SetSkyLightRenderParams is not the expected code: the renderer's sky field is left alone "
                "(the sky elements' are still copied)");
        g.ready = true;
        return;
    }
    if (g.active || GetCurrentThreadId() != g.mainThread) return;
    const uint64_t t0 = __rdtsc();
    ++g.st.flushes;
    g.renderer = r;
    HandOff(r);
    if (g.mode == EMode::Verify) FindMissed(r);
    g.active = true;
    g.st.copyCycles += __rdtsc() - t0;
#else
    (void)r;
#endif
}

void EngineData_AfterFlush() {
#ifdef _WIN64
    if (!g.active) return;
    g.active = false;
    const uint64_t t0 = __rdtsc();
    AfterFlush();
    g.st.afterCycles += __rdtsc() - t0;
#endif
}

const char* EngineData_Original(const char* p) {
#ifdef _WIN64
    if (!g.active || !p) return p;
    const char* const o = Cur().Unmap(p);
    return o ? o : p;
#else
    return p;
#endif
}

void EngineData_Report(const char* when) {
#ifdef _WIN64
    if (g.installed && g.ready) Report(when);
#else
    (void)when;
#endif
}

}  // namespace optiperf
