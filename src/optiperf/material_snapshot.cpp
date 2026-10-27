// Material snapshot: Phase 2 of research/docs/cpu-multithreading-plan.md, after TraceFogVolumes (details in
// research/docs/material-snapshot.md). x64 only.
//
// Render objects can carry material layers (CRenderObject +0x84: frozen, wet, cloaked). For their batches the flush's
// general shader path calls Renderer:SetupMultiLayers (CryRenderD3D10 +0x20350), which picks the object's sub-material
// for the chunk and the layer to draw it with, and Renderer:DrawShader_MultiLayers (+0x2CEF0), which draws one more
// pass per enabled layer. Both ask the 3D engine's material objects through their interfaces: IMaterial's
// GetSubMtlCount, GetSubMtl, GetFlags, GetLayerCount, GetLayer (by slot, or by mask: that one also notes the layer in
// the material), GetShaderItem; IMaterialLayer's IsEnabled and GetShaderItem; IMaterialManager's
// GetDefaultLayersMaterial. On ice that is about 4,000 calls per frame.
//   - on: right before the flush (with the other parts' pre-runs done), the materials of the objects with layers among
//     the items the flush will draw (level 1's lists, the shadow pre-pass's stashes, the reflections' captures) are
//     copied as those getters read them, with their sub-materials, the default layers material and all their layers
//     (material_record.h). In the flush, the getters' slots in the classes' vtables (CMatInfo, CMaterialLayer, CMatMan)
//     answer the two functions' calls from the copies, the getters transcribed; a layer's shader item is answered
//     with its copy. GetLayer by mask also notes the layer in the material, as the 3D engine's does; nothing in the
//     flush reads that back, so the notes are written right after the flush, on the main thread, in their order: the
//     flush writes no material (what a render thread needs). Calls from elsewhere or from other threads, and for a
//     material not copied, go to the 3D engine.
//   - verify: as on, but each call it would answer also goes to the 3D engine, whose answer is compared and returned.
// The two functions' calls and the getters' code are checked by pattern; with any of it different, the part stays off.
//
// [ShadowPrepass] Materials = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_MATERIALS)

#include "material_snapshot.h"

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
#include "diag.h"
#include "flush_thread.h"
#include "log.h"
#include "material_record.h"
#include "reflection_prepass.h"
#include "scan.h"
#include "shadow_prepass.h"
#include "shadow_prepass_lists.h"

namespace optiperf {
namespace {

enum class MMode { Off, On, Verify };

MMode ReadMode() {
    const std::wstring v = ConfigString(L"ShadowPrepass", L"Materials", L"OPTIPERF_SHADOW_PREPASS_MATERIALS", L"off");
    if (_wcsicmp(v.c_str(), L"on") == 0 || v == L"1") return MMode::On;
    if (_wcsicmp(v.c_str(), L"verify") == 0) return MMode::Verify;
    return MMode::Off;
}

#ifdef _WIN64
const char* ModeName(MMode m) { return m == MMode::On ? "on" : m == MMode::Verify ? "verify" : "off"; }

// --- patterns (x64 CryRenderD3D10.dll, build 6156) ------------------------------------------------------------------
// Renderer:SetupMultiLayers(renderer, set): its start (the current object +0xA78, the current element +0xA88, the
// object's layers +0x84 and material +0x108) ...
constexpr const char kSetupStart[] =
    "48 89 5C 24 20 55 41 54 41 55 48 83 EC 40 4C 8B A1 78 0A 00 00 48 8B 99 88 0A 00 00 44 0F B6 EA "
    "4D 85 E4 48 8B E9 0F 84 ?? ?? ?? ?? 41 F7 84 24 84 00 00 00 00 00 00 FF 0F 84 ?? ?? ?? ?? 48 85 "
    "DB 0F 84 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 01 7E 0E 0F BA A1 F4 0A 00 00 18 0F 83 ?? ?? ?? ?? 48 8B "
    "89 B0 0A 00 00 48 89 74 24 60 49 8B B4 24 08 01 00 00 48 85 C9 0F 84 ?? ?? ?? ?? 48 85 F6 0F 84 "
    "?? ?? ?? ?? 48 8B 41 38 48 83 C1 38 FF 50 60";
// ... and its material calls (the element's chunk +0x40, the chunk's material id +0x2C, the object's layer byte
// +0x87), 0x254 bytes in
constexpr const char kSetupCalls[] =
    "45 84 ED 0F 84 ?? ?? ?? ?? 48 89 BD 70 0A 00 00 48 8B 5B 40 48 85 DB 0F 84 ?? ?? ?? ?? 48 8B 06 "
    "48 8B CE FF 90 A0 00 00 00 85 C0 74 12 48 8B 06 8B 53 2C 48 8B CE FF 90 A8 00 00 00 48 8B F0 48 "
    "85 F6 0F 84 ?? ?? ?? ?? 48 39 3D ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 48 8B 06 48 8B CE FF 50 40 0F BA "
    "E0 0A 0F 82 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 41 38 BC 24 87 00 00 00 48 8B 01 0F 95 C3 45 33 C0 "
    "0F B6 D3 FF 90 D8 00 00 00 4C 8B 0E 45 33 C0 0F B6 D3 48 8B CE 4C 8B E0 41 FF 91 D8 00 00 00 48 "
    "85 C0 48 8B D8 75 0F 4D 85 E4 41 B6 01 49 8B DC 0F 84 ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 18 84 "
    "C0 0F 84 ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 28 48 8B 08";
constexpr int kSetupCallsAt = 0x254;
constexpr int kSetupDefaultLayers = 0x4B;  // in kSetupCalls: the default layers material's static (rip-relative)
// Renderer:DrawShader_MultiLayers(renderer): its start, with the material manager's, the sub-material's and the
// default layers material's calls ...
constexpr const char kDrawStart[] =
    "40 53 57 48 81 EC 78 01 00 00 48 8B 81 78 0A 00 00 48 8B B9 88 0A 00 00 48 8B D9 48 85 C0 0F 84 "
    "?? ?? ?? ?? 48 83 B9 68 0A 00 00 00 0F 84 ?? ?? ?? ?? 48 83 B9 B0 0A 00 00 00 0F 84 ?? ?? ?? ?? "
    "83 B8 84 00 00 00 00 0F 84 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 01 4C 89 BC 24 48 01 00 00 4C 8B B8 08 "
    "01 00 00 4C 89 7C 24 50 7E 0E 0F BA A1 F4 0A 00 00 18 0F 83 ?? ?? ?? ?? 4D 85 FF 0F 84 ?? ?? ?? "
    "?? 48 8B 7F 40 48 85 FF 0F 84 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 89 AC 24 70 01 00 00 48 8B 01 "
    "FF 90 E0 00 00 00 48 8B C8 4C 8B 00 41 FF 90 A0 03 00 00 48 8B C8 4C 8B 00 41 FF 50 48 4D 8B 07 "
    "49 8B CF 48 8B E8 48 89 44 24 58 41 FF 90 A0 00 00 00 85 C0 74 18 4D 8B 07 8B 57 2C 49 8B CF 41 "
    "FF 90 A8 00 00 00 4C 8B F8 48 89 44 24 50 4D 85 FF 0F 84 ?? ?? ?? ?? 48 85 ED 0F 84 ?? ?? ?? ?? "
    "49 8B 07 49 8B CF FF 50 40 0F BA E0 0A 0F 82 ?? ?? ?? ?? 48 8B 45 00 48 8B CD FF 90 C0 00 00 00 "
    "85 C0 8B F8 89 44 24 34 0F 84 ?? ?? ?? ??";
// ... its loop over the layer slots (0x2A0 bytes in), and the sub-material's GetShaderItem (0x40A bytes in)
constexpr const char kDrawLoop[] =
    "49 8B 07 8B D6 49 8B CF FF 90 D0 00 00 00 4C 8B 45 00 8B D6 48 8B CD 48 8B F8 41 FF 90 D0 00 00 "
    "00 48 85 FF C6 84 24 90 01 00 00 00 75 14 48 85 C0 48 8B F8 C6 84 24 90 01 00 00 01 0F 84 ?? ?? "
    "?? ?? 48 8B 07 48 8B CF FF 50 18 84 C0 0F 84 ?? ?? ?? ?? 48 8B 07 48 8B CF FF 50 28 4C 8B E8 48 "
    "8B 00 48 85 C0";
constexpr const char kDrawItem[] = "45 85 C0 0F 84 ?? ?? ?? ?? 49 8B 07 49 8B CF FF 90 80 00 00 00 80 BC 24 90 01 00 00 00";
constexpr int kDrawLoopAt = 0x2A0, kDrawItemAt = 0x40A;

enum SitePattern { kInSetupCalls, kInDrawStart, kInDrawLoop, kInDrawItem };
struct SiteDef {
    MtlSlot slot;
    SitePattern in;
    int offset;  // the return address, in that pattern
};
constexpr SiteDef kSiteDefs[] = {
    {kMtlSubCount, kInSetupCalls, 0x29},      {kMtlSub, kInSetupCalls, 0x3C},
    {kMtlFlags, kInSetupCalls, 0x5E},         {kMtlLayerMask, kInSetupCalls, 0x89},  // (the default layers material)
    {kMtlLayerMask, kInSetupCalls, 0x9F},     {kMtlEnabled, kInSetupCalls, 0xBF},
    {kMtlLayerItem, kInSetupCalls, 0xD0},     {kMtlDefaultLayers, kInDrawStart, 0xBD},
    {kMtlSubCount, kInDrawStart, 0xD2},       {kMtlSub, kInDrawStart, 0xE6},
    {kMtlFlags, kInDrawStart, 0x109},         {kMtlLayerCount, kInDrawStart, 0x120},
    {kMtlLayerAt, kInDrawLoop, 0x0E},         {kMtlLayerAt, kInDrawLoop, 0x21},  // (the default layers material)
    {kMtlEnabled, kInDrawLoop, 0x4B},         {kMtlLayerItem, kInDrawLoop, 0x5C},
    {kMtlItem, kInDrawItem, 0x15},
};

// --- the getters (x64 Cry3DEngine.dll, build 6156): the whole of each -------------------------------------------------
enum MtlClass { kCMatInfo, kCMaterialLayer, kCMatMan, kMtlClasses };
constexpr const char* kClassNames[kMtlClasses] = {".?AVCMatInfo@@", ".?AVCMaterialLayer@@", ".?AVCMatMan@@"};
struct GetterDef {
    MtlClass cls;
    int slot;
    const char* name;
    const char* code;
};
constexpr GetterDef kGetters[kMtlSlots] = {
    {kCMatInfo, 8, "IMaterial::GetFlags", "8B 41 28 C3"},
    {kCMatInfo, 16, "IMaterial::GetShaderItem", "48 8D 41 30 C3"},
    {kCMatInfo, 20, "IMaterial::GetSubMtlCount", "48 8B 41 60 8B 40 FC C3"},
    {kCMatInfo, 21, "IMaterial::GetSubMtl",
     "4C 8B 49 60 45 8B 41 FC 45 85 C0 74 18 0F BA 61 28 08 73 11 85 D2 78 0D 41 3B D0 7D 08 48 63 C2 "
     "49 8B 04 C1 C3 33 C0 C3"},
    {kCMatInfo, 24, "IMaterial::GetLayerCount",
     "48 8B 51 70 48 85 D2 74 0C 48 8B 42 08 48 2B 02 48 C1 F8 03 C3 33 C0 C3"},
    {kCMatInfo, 26, "IMaterial::GetLayer(slot)",
     "4C 8B 41 70 4D 85 C0 74 19 49 8B 40 08 49 2B 00 48 C1 F8 03 3B D0 73 0A 49 8B 00 8B CA 48 8B 04 "
     "C8 C3 33 C0 C3"},
    {kCMatInfo, 27, "IMaterial::GetLayer(mask, usage)",
     "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 8B 51 70 41 0F B6 F0 44 0F B6 DA 4D 85 D2 4C 8B "
     "C9 74 7D 84 D2 74 79 49 8B 42 08 33 D2 33 C9 49 2B 02 48 C1 F8 03 85 C0 48 63 D8 7E 63 49 8B 02 "
     "41 0F B6 FB 0F A3 D7 4C 8B 00 72 22 48 83 C1 01 83 C2 01 48 83 C0 08 48 3B CB 7C E8 33 C0 48 8B "
     "5C 24 08 48 8B 74 24 10 48 8B 7C 24 18 C3 4D 85 C0 45 88 59 78 41 88 71 79 74 1A 4D 89 81 80 00 "
     "00 00 49 8B C0 48 8B 5C 24 08 48 8B 74 24 10 48 8B 7C 24 18 C3 49 C7 81 80 00 00 00 00 00 00 00 "
     "48 8B 5C 24 08 48 8B 74 24 10 48 8B 7C 24 18 33 C0 C3"},
    {kCMaterialLayer, 3, "IMaterialLayer::IsEnabled", "0F B6 41 08 F6 D0 24 01 C3"},
    {kCMaterialLayer, 5, "IMaterialLayer::GetShaderItem", "48 8D 41 10 C3"},
    {kCMatMan, 9, "IMaterialManager::GetDefaultLayersMaterial", "48 8B 41 50 C3"},
};
constexpr int kMatManDefaultLayers = 0x50;  // CMatMan: the default layers material, as GetDefaultLayersMaterial reads it
// C3DEngine::GetMaterialManager (I3DEngine slot 116): returns a static, the material manager
constexpr const char kGetMatMan[] = "48 8B 05 ?? ?? ?? ?? C3";
constexpr int kSlotGetMatMan = 116;
// CRenderObject: its material layers (the renderer draws them where any bit is set) and its material
constexpr int kObjLayers = 0x84, kObjMaterial = 0x108;

using FlagsFn = uint32_t (*)(char*);
using ItemFn = void* (*)(char*);
using SubCountFn = int32_t (*)(char*);
using SubFn = char* (*)(char*, int32_t);
using LayerCountFn = uint64_t (*)(char*);
using LayerAtFn = char* (*)(char*, uint32_t);
using LayerMaskFn = char* (*)(char*, uint8_t, uint8_t);
using EnabledFn = uint32_t (*)(char*);
using DefaultLayersFn = char* (*)(char*);

struct MStats {
    uint64_t flushes = 0, gatherCycles = 0, items = 0, layered = 0, notCopied = 0;
    uint64_t mats = 0, layers = 0, unusedMats = 0, unusedLayers = 0;
    size_t maxMats = 0, maxLayers = 0;
    uint64_t answered[kMtlSlots] = {}, missed[kMtlSlots] = {}, same[kMtlSlots] = {}, differ[kMtlSlots] = {};
    uint64_t outside[kMtlSlots] = {};  // from the two functions, but outside the flush
    uint64_t otherThread = 0;
    uint64_t notes = 0;  // GetLayer's notes in the materials, written after the flush
};

struct MState {
    MMode mode = MMode::Off;
    bool installed = false, setupTried = false, ready = false, verify = false, inFlush = false;
    MaterialTarget t;
    MaterialEngine e;
    const ShadowPrepassTarget* shadow = nullptr;
    char* renderer = nullptr;
    DWORD mainThread = 0;
    void* orig[kMtlSlots] = {};  // the vtable entries the hooks replaced (the 3D engine's getters, or the tracer's thunks)
    mt::Snapshot snap;
    const char* matMan = nullptr;         // at the capture: the material manager ...
    const char* defaultLayers = nullptr;  // ... and its default layers material
    struct Noted {  // a note GetLayer by mask makes in the material, due after the flush
        char* material;
        uint8_t mask, usage;
        const char* layer;
    };
    std::vector<Noted> notes;
    MStats st;
    int logged = 0, loggedMiss = 0;
};
MState g;

void Append(char* line, size_t size, size_t& n, const char* fmt, ...) {
    if (n >= size) return;
    va_list a;
    va_start(a, fmt);
    const int k = vsnprintf(line + n, size - n, fmt, a);
    va_end(a);
    if (k > 0) n = (std::min)(size - 1, n + size_t(k));
}

bool Matches(char* base, const char* at, const char* text) {
    const Pattern p = ParsePattern(text);
    return at && IsInCode(base, at, p.size()) && MatchesAt(at, p);
}

// a vtable entry as a mapped module's address (the module may not be relocated where the test maps it)
char* Entry(char* base, void* entry) {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint64_t e = reinterpret_cast<uint64_t>(entry), size = nt->OptionalHeader.SizeOfImage;
    if (e - reinterpret_cast<uint64_t>(base) < size) return static_cast<char*>(entry);
    if (e - nt->OptionalHeader.ImageBase < size) return base + (e - nt->OptionalHeader.ImageBase);
    return nullptr;
}

char* Target4(char* field) { return field + 4 + int32_t(ReadU32(field)); }

// --- right before the flush: the copies -----------------------------------------------------------------------------
void ScanItem(const sp::RendItem& it) {
    ++g.st.items;
    const char* obj = it.obj;
    if (!obj || !mt::Read<uint32_t>(obj, kObjLayers)) return;
    ++g.st.layered;
    const char* m = mt::Read<const char*>(obj, kObjMaterial);
    if (m && !g.snap.AddMaterial(m, true)) ++g.st.notCopied;
}

void ScanStash(const sp::Stash& s, void*) {
    const sp::RendItem* it = s.items.data();
    for (int k = 0; k < sp::kLists; ++k)
        for (uint32_t i = 0; i < s.counts[k]; ++i) ScanItem(*it++);
}

void Gather() {
    const uint64_t t0 = __rdtsc();
    g.snap.Clear();
    // level 1's part of the render lists, the shadow maps the shadow pre-pass rendered early, the reflections the dry
    // run rendered early
    const auto* lists = reinterpret_cast<const sp::ItemList*>(g.shadow->lists);
    const uint32_t* starts = g.shadow->starts;
    for (int k = 0; k < sp::kLists; ++k)
        for (uint32_t i = starts[k]; i < lists[k].count; ++i) ScanItem(lists[k].elems[i]);
    ShadowPrepass_ForEachStash(&ScanStash, nullptr);
    ReflectionPrepass_ForEachCapture(&ScanStash, nullptr);
    // the default layers material: the material manager's, and SetupMultiLayers' own copy of the pointer
    g.matMan = static_cast<const char*>(*g.e.matMan);
    g.defaultLayers = g.matMan ? mt::Read<const char*>(g.matMan, kMatManDefaultLayers) : nullptr;
    if (g.defaultLayers && !g.snap.AddMaterial(g.defaultLayers, false)) ++g.st.notCopied;
    const char* own = static_cast<const char*>(*g.t.defaultLayers);
    if (own && own != g.defaultLayers && !g.snap.AddMaterial(own, false)) ++g.st.notCopied;
    for (const mt::Mat& m : g.snap.mats) Allocator_Pin(m.self);  // (FlushThread=overlap: kept for the notes)
    g.st.mats += g.snap.mats.size();
    g.st.layers += g.snap.layers.size();
    g.st.maxMats = (std::max)(g.st.maxMats, g.snap.mats.size());
    g.st.maxLayers = (std::max)(g.st.maxLayers, g.snap.layers.size());
    g.st.gatherCycles += __rdtsc() - t0;
}

// --- in the flush: the getters' slots -------------------------------------------------------------------------------
// whether a call comes from one of the two functions' calls of this getter, on the flush's thread inside the flush
// (the main thread's outside it are counted, as are other threads')
bool Answering(MtlSlot s, void* ret) {
    if (ret != g.t.sites[s][0] && ret != g.t.sites[s][1]) return false;
    const DWORD t = GetCurrentThreadId();
    if (g.inFlush && t == FlushThread()) return true;
    if (t == g.mainThread)
        ++g.st.outside[s];
    else
        ++g.st.otherThread;
    return false;
}

void Miss(MtlSlot s, const void* self) {
    ++g.st.missed[s];
    if (++g.loggedMiss <= 10)
        Log("materials: flush %llu: %s on %p, which has no copy: asked the 3D engine", g.st.flushes, kGetters[s].name,
            self);
}

void Compare(MtlSlot s, bool same, const void* self, uint64_t live, uint64_t mine) {
    if (same) {
        ++g.st.same[s];
        return;
    }
    ++g.st.differ[s];
    if (++g.logged <= 20)
        Log("materials: VERIFY flush %llu: %s on %p differs: copy 0x%llX, 3D engine 0x%llX", g.st.flushes,
            kGetters[s].name, self, mine, live);
}

uint32_t FlagsHook(char* self) {
    const auto orig = reinterpret_cast<FlagsFn>(g.orig[kMtlFlags]);
    if (!Answering(kMtlFlags, _ReturnAddress())) return orig(self);
    mt::Mat* m = g.snap.FindMat(self);
    if (!m) return Miss(kMtlFlags, self), orig(self);
    m->used = true;
    const uint32_t mine = mt::Flags(*m);
    if (g.verify) {
        const uint32_t live = orig(self);
        Compare(kMtlFlags, live == mine, self, live, mine);
        return live;
    }
    ++g.st.answered[kMtlFlags];
    return mine;
}

// IMaterial::GetShaderItem(): where the material's own shader item is (the caller doesn't read it)
void* MtlItemHook(char* self) {
    const auto orig = reinterpret_cast<ItemFn>(g.orig[kMtlItem]);
    if (!Answering(kMtlItem, _ReturnAddress())) return orig(self);
    mt::Mat* m = g.snap.FindMat(self);
    if (!m) return Miss(kMtlItem, self), orig(self);
    m->used = true;
    void* mine = self + mt::kMatItem;
    if (g.verify) {
        void* live = orig(self);
        Compare(kMtlItem, live == mine, self, uint64_t(live), uint64_t(mine));
        return live;
    }
    ++g.st.answered[kMtlItem];
    return mine;
}

int32_t SubCountHook(char* self) {
    const auto orig = reinterpret_cast<SubCountFn>(g.orig[kMtlSubCount]);
    if (!Answering(kMtlSubCount, _ReturnAddress())) return orig(self);
    mt::Mat* m = g.snap.FindMat(self);
    if (!m) return Miss(kMtlSubCount, self), orig(self);
    m->used = true;
    const int32_t mine = mt::SubMtlCount(*m);
    if (g.verify) {
        const int32_t live = orig(self);
        Compare(kMtlSubCount, live == mine, self, uint32_t(live), uint32_t(mine));
        return live;
    }
    ++g.st.answered[kMtlSubCount];
    return mine;
}

char* SubHook(char* self, int32_t i) {
    const auto orig = reinterpret_cast<SubFn>(g.orig[kMtlSub]);
    if (!Answering(kMtlSub, _ReturnAddress())) return orig(self, i);
    mt::Mat* m = g.snap.FindMat(self);
    if (!m) return Miss(kMtlSub, self), orig(self, i);
    m->used = true;
    char* mine = const_cast<char*>(mt::SubMtl(g.snap, *m, i));
    if (g.verify) {
        char* live = orig(self, i);
        Compare(kMtlSub, live == mine, self, uint64_t(live), uint64_t(mine));
        return live;
    }
    ++g.st.answered[kMtlSub];
    return mine;
}

uint64_t LayerCountHook(char* self) {
    const auto orig = reinterpret_cast<LayerCountFn>(g.orig[kMtlLayerCount]);
    if (!Answering(kMtlLayerCount, _ReturnAddress())) return orig(self);
    mt::Mat* m = g.snap.FindMat(self);
    if (!m) return Miss(kMtlLayerCount, self), orig(self);
    m->used = true;
    const uint64_t mine = mt::LayerCount(*m);
    if (g.verify) {
        const uint64_t live = orig(self);
        Compare(kMtlLayerCount, live == mine, self, live, mine);
        return live;
    }
    ++g.st.answered[kMtlLayerCount];
    return mine;
}

char* LayerAtHook(char* self, uint32_t slot) {
    const auto orig = reinterpret_cast<LayerAtFn>(g.orig[kMtlLayerAt]);
    if (!Answering(kMtlLayerAt, _ReturnAddress())) return orig(self, slot);
    mt::Mat* m = g.snap.FindMat(self);
    if (!m) return Miss(kMtlLayerAt, self), orig(self, slot);
    m->used = true;
    char* mine = const_cast<char*>(mt::LayerAt(g.snap, *m, slot));
    if (g.verify) {
        char* live = orig(self, slot);
        Compare(kMtlLayerAt, live == mine, self, uint64_t(live), uint64_t(mine));
        return live;
    }
    ++g.st.answered[kMtlLayerAt];
    return mine;
}

char* LayerMaskHook(char* self, uint8_t mask, uint8_t usage) {
    const auto orig = reinterpret_cast<LayerMaskFn>(g.orig[kMtlLayerMask]);
    if (!Answering(kMtlLayerMask, _ReturnAddress())) return orig(self, mask, usage);
    mt::Mat* m = g.snap.FindMat(self);
    if (!m) return Miss(kMtlLayerMask, self), orig(self, mask, usage);
    m->used = true;
    const char* layer;
    const bool found = mt::LayerByMask(g.snap, *m, mask, &layer);
    if (g.verify) {
        char* live = orig(self, mask, usage);  // (which notes the layer in the material itself)
        Compare(kMtlLayerMask, live == layer, self, uint64_t(live), uint64_t(layer));
        return live;
    }
    if (found) g.notes.push_back({self, mask, usage, layer});  // (noted after the flush: nothing reads it back)
    ++g.st.answered[kMtlLayerMask];
    return const_cast<char*>(layer);
}

uint32_t EnabledHook(char* self) {
    const auto orig = reinterpret_cast<EnabledFn>(g.orig[kMtlEnabled]);
    if (!Answering(kMtlEnabled, _ReturnAddress())) return orig(self);
    mt::Layer* l = g.snap.FindLayer(self);
    if (!l) return Miss(kMtlEnabled, self), orig(self);
    l->used = true;
    const uint32_t mine = mt::Enabled(*l);
    if (g.verify) {
        const uint32_t live = orig(self);
        Compare(kMtlEnabled, (live & 0xFF) == mine, self, live, mine);
        return live;
    }
    ++g.st.answered[kMtlEnabled];
    return mine;
}

// IMaterialLayer::GetShaderItem: its copy (the callers read the shader, the resources and the technique through it)
void* LayerItemHook(char* self) {
    const auto orig = reinterpret_cast<ItemFn>(g.orig[kMtlLayerItem]);
    if (!Answering(kMtlLayerItem, _ReturnAddress())) return orig(self);
    mt::Layer* l = g.snap.FindLayer(self);
    if (!l) return Miss(kMtlLayerItem, self), orig(self);
    l->used = true;
    if (g.verify) {
        void* live = orig(self);
        uint64_t a, b;
        std::memcpy(&a, live, 8);
        std::memcpy(&b, l->item, 8);
        Compare(kMtlLayerItem, std::memcmp(live, l->item, mt::kItemBytes) == 0, self, a, b);
        return live;
    }
    ++g.st.answered[kMtlLayerItem];
    return l->item;
}

char* DefaultLayersHook(char* self) {
    const auto orig = reinterpret_cast<DefaultLayersFn>(g.orig[kMtlDefaultLayers]);
    if (!Answering(kMtlDefaultLayers, _ReturnAddress())) return orig(self);
    if (self != g.matMan) return Miss(kMtlDefaultLayers, self), orig(self);
    char* mine = const_cast<char*>(g.defaultLayers);
    if (g.verify) {
        char* live = orig(self);
        Compare(kMtlDefaultLayers, live == mine, self, uint64_t(live), uint64_t(mine));
        return live;
    }
    ++g.st.answered[kMtlDefaultLayers];
    return mine;
}

void* const kHooks[kMtlSlots] = {
    reinterpret_cast<void*>(&FlagsHook),      reinterpret_cast<void*>(&MtlItemHook),
    reinterpret_cast<void*>(&SubCountHook),   reinterpret_cast<void*>(&SubHook),
    reinterpret_cast<void*>(&LayerCountHook), reinterpret_cast<void*>(&LayerAtHook),
    reinterpret_cast<void*>(&LayerMaskHook),  reinterpret_cast<void*>(&EnabledHook),
    reinterpret_cast<void*>(&LayerItemHook),  reinterpret_cast<void*>(&DefaultLayersHook),
};

// a vtable slot pointed at fn (one aligned store: other threads calling meanwhile get the old or the new entry)
bool PatchSlot(void** slot, void* fn, void** old) {
    DWORD protect;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
    *old = *slot;
    InterlockedExchangePointer(slot, fn);
    VirtualProtect(slot, sizeof(void*), protect, &protect);
    return true;
}

// At the first flush: the 3D engine's classes and getters, and the slots patched.
void Setup(char* r) {
    g.setupTried = true;
    HMODULE e3d = GetModuleHandleW(L"Cry3DEngine.dll");
    const char* why = "Cry3DEngine.dll is not loaded";
    if (!e3d || !MaterialSnapshot_LocateEngine(reinterpret_cast<char*>(e3d), &g.e, &why)) {
        Log("materials: left to the 3D engine: %s", why);
        return;
    }
    for (int i = 0; i < kMtlSlots; ++i) {
        void** slot = &g.e.vtables[kGetters[i].cls][kGetters[i].slot];
        if (!PatchSlot(slot, kHooks[i], &g.orig[i])) {
            void* ignored;
            for (int j = 0; j < i; ++j) PatchSlot(&g.e.vtables[kGetters[j].cls][kGetters[j].slot], g.orig[j], &ignored);
            Log("materials: left to the 3D engine: could not write %s's vtable entry", kGetters[i].name);
            return;
        }
    }
    g.renderer = r;
    g.mainThread = GetCurrentThreadId();
    g.ready = true;
    char* base = reinterpret_cast<char*>(e3d);
    Log("materials: mode=%s: the getters' slots patched (CMatInfo +0x%X, CMaterialLayer +0x%X, CMatMan +0x%X); the "
        "material manager's static +0x%X",
        ModeName(g.mode), unsigned(reinterpret_cast<char*>(g.e.vtables[kCMatInfo]) - base),
        unsigned(reinterpret_cast<char*>(g.e.vtables[kCMaterialLayer]) - base),
        unsigned(reinterpret_cast<char*>(g.e.vtables[kCMatMan]) - base),
        unsigned(reinterpret_cast<const char*>(g.e.matMan) - base));
}

void Report(const char* when) {
    const MStats& s = g.st;
    const double flushes = double(s.flushes ? s.flushes : 1);
    Log("materials [%s]: mode=%s | flushes=%llu | items scanned=%llu, with material layers %llu | copied per flush: "
        "materials %.1f (at most %zu), layers %.1f (at most %zu); not copied (out of range) %llu | %.4f ms per flush",
        when, ModeName(g.mode), s.flushes, s.items, s.layered, double(s.mats) / flushes, s.maxMats,
        double(s.layers) / flushes, s.maxLayers, s.notCopied, double(s.gatherCycles) / 3.0e6 / flushes);
    char line[2048] = "";
    size_t n = 0;
    uint64_t answered = 0, missed = 0, same = 0, differ = 0, outside = 0;
    for (int i = 0; i < kMtlSlots; ++i) {
        answered += s.answered[i];
        missed += s.missed[i];
        same += s.same[i];
        differ += s.differ[i];
        outside += s.outside[i];
        if (g.verify)
            Append(line, sizeof line, n, " %s %llu/%llu/%llu", kGetters[i].name, s.same[i], s.differ[i], s.missed[i]);
        else
            Append(line, sizeof line, n, " %s %llu/%llu", kGetters[i].name, s.answered[i], s.missed[i]);
    }
    Log("materials [%s]: calls %s=%llu; to the 3D engine: no copy %llu, outside the flush %llu, from other threads "
        "%llu | per getter (%s):%s",
        when, g.verify ? "compared" : "answered", g.verify ? same + differ : answered, missed, outside, s.otherThread,
        g.verify ? "same/differ/no copy" : "answered/no copy", line);
    if (g.verify) Log("materials [%s]: VERIFY calls=%llu: same=%llu differ=%llu", when, same + differ, same, differ);
    Log("materials [%s]: copies not asked for: materials %llu of %llu, layers %llu of %llu | GetLayer's notes in the "
        "materials, written after the flush: %llu",
        when, s.unusedMats, s.mats, s.unusedLayers, s.layers, s.notes);
}
#endif  // _WIN64

}  // namespace

bool MaterialSnapshot_LocateRenderer(char* base, MaterialTarget* out, const char** why) {
#ifndef _WIN64
    (void)base;
    (void)out;
    *why = "64-bit game only";
    return false;
#else
    const auto setups = FindInCode(base, ParsePattern(kSetupStart), 2);
    if (setups.size() != 1) return *why = "Renderer:SetupMultiLayers not found", false;
    char* setup = setups[0];
    if (!Matches(base, setup + kSetupCallsAt, kSetupCalls))
        return *why = "Renderer:SetupMultiLayers' material calls are not the expected code", false;
    const auto draws = FindInCode(base, ParsePattern(kDrawStart), 2);
    if (draws.size() != 1) return *why = "Renderer:DrawShader_MultiLayers not found", false;
    char* draw = draws[0];
    if (!Matches(base, draw + kDrawLoopAt, kDrawLoop) || !Matches(base, draw + kDrawItemAt, kDrawItem))
        return *why = "Renderer:DrawShader_MultiLayers' material calls are not the expected code", false;
    char* defaultLayers = Target4(setup + kSetupCallsAt + kSetupDefaultLayers);
    if (!IsInImage(base, defaultLayers, 8)) return *why = "SetupMultiLayers' default layers material is not in the module", false;
    MaterialTarget t;
    t.setupLayers = setup;
    t.drawLayers = draw;
    t.defaultLayers = reinterpret_cast<void* const*>(defaultLayers);
    char* const at[] = {setup + kSetupCallsAt, draw, draw + kDrawLoopAt, draw + kDrawItemAt};
    for (const SiteDef& d : kSiteDefs) {
        char** s = t.sites[d.slot];
        s[s[0] ? 1 : 0] = at[d.in] + d.offset;
    }
    *out = t;
    return true;
#endif
}

bool MaterialSnapshot_LocateEngine(char* e3d, MaterialEngine* out, const char** why) {
#ifndef _WIN64
    (void)e3d;
    (void)out;
    *why = "64-bit game only";
    return false;
#else
    MaterialEngine e;
    static const char* const kNoClass[kMtlClasses] = {"CMatInfo's vtable not found", "CMaterialLayer's vtable not found",
                                                      "CMatMan's vtable not found"};
    for (int c = 0; c < kMtlClasses; ++c) {
        e.vtables[c] = FindRttiVtable(e3d, kClassNames[c]);
        if (!e.vtables[c]) return *why = kNoClass[c], false;
    }
    for (int i = 0; i < kMtlSlots; ++i) {
        void** vt = e.vtables[kGetters[i].cls];
        if (!IsInImage(e3d, reinterpret_cast<const char*>(vt + kGetters[i].slot), sizeof(void*)))
            return *why = "a material class's vtable is shorter than expected", false;
        e.fns[i] = Entry(e3d, CallTrace_Original(vt[kGetters[i].slot]));
        if (!Matches(e3d, e.fns[i], kGetters[i].code)) {
            static char buf[128];
            snprintf(buf, sizeof buf, "%s is not the expected code", kGetters[i].name);
            return *why = buf, false;
        }
    }
    void** engine = FindRttiVtable(e3d, ".?AVC3DEngine@@");
    char* getMatMan = engine && IsInImage(e3d, reinterpret_cast<const char*>(engine + kSlotGetMatMan), sizeof(void*))
                          ? Entry(e3d, CallTrace_Original(engine[kSlotGetMatMan]))
                          : nullptr;
    if (!Matches(e3d, getMatMan, kGetMatMan)) return *why = "I3DEngine::GetMaterialManager is not the expected code", false;
    char* matMan = Target4(getMatMan + 3);
    if (!IsInImage(e3d, matMan, 8)) return *why = "the material manager's static is not in the module", false;
    e.matMan = reinterpret_cast<void* const*>(matMan);
    *out = e;
    return true;
#endif
}

void MaterialSnapshot_Install(const wchar_t* moduleName, char* base, const ShadowPrepassTarget& sp) {
#ifndef _WIN64
    (void)base;
    (void)sp;
    if (ReadMode() != MMode::Off) Log("%ls: material snapshot not applied: 64-bit game only", moduleName);
#else
    if (g.installed) return;
    g.mode = ReadMode();
    if (g.mode == MMode::Off) return;
    const char* why = nullptr;
    if (!MaterialSnapshot_LocateRenderer(base, &g.t, &why)) {
        Log("%ls: materials left to the 3D engine: %s", moduleName, why);
        return;
    }
    g.shadow = &sp;
    g.verify = g.mode == MMode::Verify;
    g.installed = true;
    Log("%ls: materials: mode=%s, Renderer:SetupMultiLayers +0x%X, Renderer:DrawShader_MultiLayers +0x%X (the 3D "
        "engine's getters are found at the first flush)",
        moduleName, ModeName(g.mode), unsigned(g.t.setupLayers - base), unsigned(g.t.drawLayers - base));
#endif
}

void MaterialSnapshot_BeforeFlush(char* renderer) {
#ifdef _WIN64
    if (!g.installed) return;
    if (!g.setupTried) Setup(renderer);
    if (!g.ready || GetCurrentThreadId() != g.mainThread || renderer != g.renderer) return;
    ++g.st.flushes;
    Gather();
    g.inFlush = true;
#else
    (void)renderer;
#endif
}

void MaterialSnapshot_AfterFlush() {
#ifdef _WIN64
    if (!g.inFlush) return;
    g.inFlush = false;
    for (const MState::Noted& n : g.notes)  // (in the flush's order; a material freed beside the flush skipped)
        if (!Allocator_Dying(n.material)) mt::Note(n.material, n.mask, n.usage, n.layer);
    g.st.notes += g.notes.size();
    g.notes.clear();
    for (const mt::Mat& m : g.snap.mats)
        if (!m.used) ++g.st.unusedMats;
    for (const mt::Layer& l : g.snap.layers)
        if (!l.used) ++g.st.unusedLayers;
#endif
}

void MaterialSnapshot_Report(const char* when) {
#ifdef _WIN64
    if (g.installed && g.ready) Report(when);
#else
    (void)when;
#endif
}

}  // namespace optiperf
