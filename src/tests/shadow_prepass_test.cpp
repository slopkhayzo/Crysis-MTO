// Tests of the shadow pre-pass outside the game (x64; the x86 build only reports that it has nothing to test).
//
// 1. The list handling (shadow_prepass_lists.h) on synthetic lists: taking a level's items out and putting them back
//    must restore the lists exactly, also when they have to grow; the comparison of verify mode must find exactly the
//    differences that matter.
// 2. Locating the code in the game's x64 CryRenderD3D10.dll, mapped without running it (DONT_RESOLVE_DLL_REFERENCES):
//    every function, global and list must be found where build 6156 has it, and one changed byte must make the
//    patch refuse.
// 3. The counting vtable copy (vtable_counter.h) on an object of the test itself; the particle record and replay
//    (particle_record.h) against a model of the renderer's particle batch.
// 4. The environment snapshot's getters in Cry3DEngine.dll, mapped the same way: each found at its vtable slot, and a
//    changed byte leaves exactly that getter out; CVisArea's vtable and the sun shafts' test in CryRenderD3D10.dll.
// 5. The skinning part: the record and replay of GetSkeletonPose's answers (skin_record.h) against synthetic
//    implementations, and its code in CryRenderD3D10.dll, CryAnimation.dll and Cry3DEngine.dll, mapped the same way.
// 6. The system snapshot's getters in CrySystem.dll (CSystem's and CTimer's vtables, by RTTI), mapped the same way:
//    each found at its slot, and a changed byte leaves exactly that getter out.
// 7. The fog volumes' math (fog_snapshot.h) against Cry3DEngine.dll's own code, mapped the same way and run: the
//    ellipsoid's and the box's contributions and the whole of TraceFogVolumes over random fog volumes, cameras and
//    positions, bit for bit; a changed byte makes it refuse.
// 8. The material snapshot: the layer passes' calls found in CryRenderD3D10.dll, the getters in Cry3DEngine.dll, and
//    the getters' transcriptions (material_record.h) against the getters themselves, run on synthetic materials;
//    changed bytes make it refuse.
// 9. The cvar snapshot's two classes in CrySystem.dll (by RTTI), their getters run on synthetic cvars; a changed byte
//    makes it refuse.
// 10. The ocean's height (ocean_snapshot.h) against Cry3DEngine.dll's own GetWave and GetOceanWaterLevel, mapped the
//    same way and run over fakes of the renderer, the render element and the timer: the FFT waves over random grids,
//    scales and positions and the whole height from the capture's inputs, bit for bit, and the branch the capture
//    picks under each of GetWave's conditions; a changed byte makes it refuse.
// 11. The flush's thread (flush_thread.h): with FlushThread=worker the flush runs on a thread of its own, the same one
//    each time, while the caller waits; it is known as the flush's thread there only, and gets the caller's MXCSR,
//    which comes back as the flush left it.
//
// usage: shadow_prepass_test.exe ["<Crysis>\Bin64"]

#include <windows.h>
#include <intrin.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../optiperf/calltrace.h"
#include "../optiperf/cvar_snapshot.h"
#include "../optiperf/env_snapshot.h"
#include "../optiperf/flush_thread.h"
#include "../optiperf/fog_snapshot.h"
#include "../optiperf/light_record.h"
#include "../optiperf/light_snapshot.h"
#include "../optiperf/material_record.h"
#include "../optiperf/material_snapshot.h"
#include "../optiperf/ocean_snapshot.h"
#include "../optiperf/particle_prepass.h"
#include "../optiperf/particle_record.h"
#include "../optiperf/range_copies.h"
#include "../optiperf/reflection_prepass.h"
#include "../optiperf/render_gate.h"
#include "../optiperf/scan.h"
#include "../optiperf/shadow_prepass.h"
#include "../optiperf/shadow_prepass_lists.h"
#include "../optiperf/skin_prepass.h"
#include "../optiperf/skin_record.h"
#include "../optiperf/sys_snapshot.h"
#include "../optiperf/vtable_counter.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        printf("  FAILED: %s\n", what);
        ++g_failures;
    }
}

#ifdef _WIN64
using namespace optiperf::sp;

int g_reallocs = 0;
bool g_failRealloc = false;
void* TestRealloc(void* p, size_t bytes) {
    if (g_failRealloc) return nullptr;
    ++g_reallocs;
    return std::realloc(p, bytes);
}

struct Lists {
    ItemList l[kLists] = {};
    ~Lists() {
        for (ItemList& x : l) std::free(x.elems);
    }
    void Append(int k, const RendItem& it) {
        if (l[k].count == l[k].capacity) {
            l[k].capacity = l[k].capacity + l[k].capacity / 2 + 4;
            l[k].elems = static_cast<RendItem*>(std::realloc(l[k].elems, l[k].capacity * sizeof(RendItem)));
        }
        l[k].elems[l[k].count++] = it;
    }
};

RendItem RandomItem(std::mt19937& r, std::vector<std::vector<char>>& objects) {
    RendItem it{};
    it.sortVal = r();
    it.elem = reinterpret_cast<char*>(uintptr_t(r() & ~7u));
    it.objSort = r();
    objects.emplace_back(0x130);
    for (char& c : objects.back()) c = char(r());
    it.obj = objects.back().data();
    it.dynLM = r();
    it.batchFlags = r();
    return it;
}

void TestLists() {
    printf("list handling\n");
    std::mt19937 r(7);
    for (int round = 0; round < 200; ++round) {
        std::vector<std::vector<char>> objects;
        objects.reserve(100000);
        Lists a;
        // the frame so far
        for (int k = 0; k < kLists; ++k) {
            const int n = int(r() % 50);
            for (int i = 0; i < n; ++i) a.Append(k, RandomItem(r, objects));
        }
        uint32_t start[kLists];
        for (int k = 0; k < kLists; ++k) start[k] = a.l[k].count;
        std::vector<std::vector<RendItem>> before(kLists);
        for (int k = 0; k < kLists; ++k) before[k].assign(a.l[k].elems, a.l[k].elems + a.l[k].count);

        // a nested level adds items to some lists
        std::vector<std::vector<RendItem>> added(kLists);
        for (int k = 0; k < kLists; ++k) {
            const int n = (r() % 3 == 0) ? int(r() % 200) : 0;
            for (int i = 0; i < n; ++i) {
                const RendItem it = RandomItem(r, objects);
                added[k].push_back(it);
                a.Append(k, it);
            }
        }
        Stash s;
        s.Reset(nullptr);
        TakeLevel(a.l, start, s);
        bool restored = true;
        for (int k = 0; k < kLists; ++k)
            restored &= a.l[k].count == start[k] &&
                        std::memcmp(a.l[k].elems, before[k].data(), before[k].size() * sizeof(RendItem)) == 0;
        Check(restored, "taking a level out leaves the lists as before it");

        // verify mode's view: the same level added again must compare equal to the stash
        for (int k = 0; k < kLists; ++k)
            for (const RendItem& it : added[k]) a.Append(k, it);
        Difference d;
        uint64_t byContents = 0, nearM = 0;
        Check(CompareLevel(a.l, start, s, nullptr, &d, &byContents, &nearM), "the same level compares equal");
        for (int k = 0; k < kLists; ++k) a.l[k].count = start[k];

        // replay: put back. Every third round the lists are full by then (in the game: other items were appended
        // after the pre-pass), so they must grow through the engine's realloc.
        if (round % 3 == 0)
            for (ItemList& x : a.l) x.capacity = x.count;
        g_reallocs = 0;
        Check(PutBack(a.l, s, &TestRealloc), "put back succeeds");
        bool same = true;
        for (int k = 0; k < kLists; ++k) {
            std::vector<RendItem> want = before[k];
            want.insert(want.end(), added[k].begin(), added[k].end());
            same &= a.l[k].count == want.size() && a.l[k].capacity >= a.l[k].count &&
                    std::memcmp(a.l[k].elems, want.data(), want.size() * sizeof(RendItem)) == 0;
        }
        Check(same, "putting a stash back restores the lists exactly");
        if (round % 3 == 0) Check(g_reallocs > 0 || s.items.empty(), "full lists grow through the engine's allocator");

        // a failed allocation leaves the lists as they were
        if (!s.items.empty()) {
            for (int k = 0; k < kLists; ++k) a.l[k].count = start[k];
            for (ItemList& x : a.l) x.capacity = x.count;
            g_failRealloc = true;
            const bool put = PutBack(a.l, s, &TestRealloc);
            g_failRealloc = false;
            bool unchanged = true;
            for (int k = 0; k < kLists; ++k) unchanged &= a.l[k].count == start[k];
            Check(!put && unchanged, "a failed allocation leaves the lists unchanged");
        }
    }
}

bool SameByType(const char* a, const char* b) { return (uintptr_t(a) ^ uintptr_t(b)) == 0x1000; }

void TestCompare() {
    printf("verify comparison\n");
    std::mt19937 r(11);
    std::vector<std::vector<char>> objects;
    objects.reserve(1000);
    Lists a;
    uint32_t start[kLists] = {};
    std::vector<RendItem> level;
    for (int i = 0; i < 20; ++i) {
        level.push_back(RandomItem(r, objects));
        a.Append(4, level.back());
    }
    Stash s;
    s.Reset(nullptr);
    TakeLevel(a.l, start, s);
    // the original run: same items, but its own render objects with the same contents
    std::vector<std::vector<char>> copies;
    for (const RendItem& it : level) copies.emplace_back(it.obj, it.obj + 0x130);
    auto rerun = [&] {
        a.l[4].count = 0;
        for (size_t i = 0; i < level.size(); ++i) {
            RendItem it = level[i];
            it.obj = copies[i].data();
            a.Append(4, it);
        }
    };
    Difference d;
    uint64_t byContents = 0, nearM = 0;
    rerun();
    Check(CompareLevel(a.l, start, s, nullptr, &d, &byContents, &nearM), "objects with equal contents compare equal");
    // identity fields and the wave index don't count
    copies[3][0x0A] ^= 1;
    copies[3][0xD2] ^= 1;
    rerun();
    Check(CompareLevel(a.l, start, s, nullptr, &d, &byContents, &nearM), "identity fields and m_nWaveID are ignored");
    // a real difference in an object
    copies[5][0x44] ^= 1;
    rerun();
    Check(!CompareLevel(a.l, start, s, nullptr, &d, &byContents, &nearM) && d.list == 4 && d.index == 5 &&
              d.field == Field::Object && d.objOffset == 0x44,
          "a changed object byte is found, at the right place");
    copies[5][0x44] ^= 1;
    // the matrix: a small change is a near match (counted), a large one a difference at that float
    {
        const float m[12] = {0.6f, -0.8f, 0.f, 1520.25f, 0.8f, 0.6f, 0.f, 870.5f, 0.f, 0.f, 1.f, 33.75f};
        std::memcpy(level[6].obj + kObjMatrix, m, sizeof m);
        std::memcpy(copies[6].data() + kObjMatrix, m, sizeof m);
        auto setFloat = [&](int i, float v) { std::memcpy(copies[6].data() + kObjMatrix + 4 * i, &v, 4); };
        setFloat(1, -0.8003f);
        setFloat(3, 1520.5f);
        rerun();
        nearM = 0;
        Check(CompareLevel(a.l, start, s, nullptr, &d, &byContents, &nearM) && nearM == 1,
              "a matrix within the tolerance compares equal and is counted");
        setFloat(7, 872.f);
        rerun();
        Check(!CompareLevel(a.l, start, s, nullptr, &d, &byContents, &nearM) && d.index == 6 &&
                  d.objOffset == kObjMatrix + 4 * 7,
              "a matrix outside the tolerance is a difference, at that float");
        std::memcpy(copies[6].data() + kObjMatrix, m, sizeof m);
    }
    // item fields
    rerun();
    a.l[4].elems[7].batchFlags ^= 4;
    Check(!CompareLevel(a.l, start, s, nullptr, &d, &byContents, &nearM) && d.index == 7 && d.field == Field::BatchFlags,
          "a changed item field is found");
    // a count difference
    rerun();
    a.Append(4, level[0]);
    Check(!CompareLevel(a.l, start, s, nullptr, &d, &byContents, &nearM) && d.field == Field::Count && d.list == 4,
          "an extra item is found");
    // pooled elements: different pointers the callback accepts
    rerun();
    a.l[4].elems[2].elem += 0x1000;
    byContents = 0;
    Check(CompareLevel(a.l, start, s, &SameByType, &d, &byContents, &nearM) && byContents == 1,
          "elements the callback accepts compare equal and are counted");
    Check(!CompareLevel(a.l, start, s, nullptr, &d, &byContents, &nearM) && d.field == Field::Elem,
          "without the callback a different element is a difference");
}

void TestLocate(const std::wstring& dir) {
    printf("locating the code in CryRenderD3D10.dll\n");
    const std::wstring path = dir + L"\\CryRenderD3D10.dll";
    HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!mod) {
        printf("  cannot map %ls (error %lu)\n", path.c_str(), GetLastError());
        ++g_failures;
        return;
    }
    char* base = reinterpret_cast<char*>(mod);
    optiperf::ShadowPrepassTarget t;
    const char* why = nullptr;
    if (!optiperf::ShadowPrepass_Locate(base, &t, &why)) {
        printf("  not located: %s\n", why);
        ++g_failures;
        FreeLibrary(mod);
        return;
    }
    struct Expect {
        const char* name;
        const void* got;
        uint32_t rva;
    };
    // build 6156 (research/docs/shadow-prepass.md)
    const Expect expect[] = {
        {"EF_EndEf3D", t.endEf3D, 0xADC40},
        {"PrepareAllDepthMaps", t.prepareAll, 0xBEE30},
        {"PrepareDepthMap", t.prepareDepthMap, 0xBC560},
        {"CryModuleRealloc", t.realloc, 0xDB210},
        {"recursion level", t.level, 0x2EA030},
        {"item lists", t.lists, 0x2EA040},
        {"level starts", t.starts, 0x2E9E50},
        {"polygon lists", t.polyLists, 0x626200},
        {"ISystem*", t.system, 0x2E6A98},
        {"r_UseShadowsPool", t.useShadowsPool, 0x2E9A44},
        {"r_ShadowGenGS", t.shadowGenGS, 0x2E9A04},
        {"r_ShadowsForwardPass", t.forwardPass, 0x2E9A18},
        {"r_ShadowGen", t.shadowGen, 0x2E99FC},
        {"r_NoDrawShaders", t.noDrawShaders, 0x2E97B8},
        {"r_HDRRendering", t.hdrRendering, 0x2E9AA4},
        {"r_MeasureOverdraw", t.measureOverdraw, 0x2E98F8},
        {"r_HDRAllowNonFP", t.hdrAllowNonFP, 0x2E9A98},
        {"r_ShadowTexFormat", t.shadowTexFormat, 0x2E9A30},
        // point-light sides
        {"CCamera::CCamera", t.camInit, 0x83880},
        {"CCamera::UpdateFrustum", t.camUpdate, 0x217B0},
        {"CCamera::SetFrustum", t.camSetFrustum, 0x22F10},
        {"box cull", t.cullBox, 0xAF050},
        {"sinf (import thunk)", t.sinf, 0x1C9F0E},
        {"cosf (import thunk)", t.cosf, 0x1C9F08},
        {"r_ShadowsDeferredMode", t.shadowsDeferred, 0x2E9A24},
        {"side fov (deferred)", t.omniFov, 0x2B0928},
    };
    printf("  point lights: %s\n", t.omniWhy ? t.omniWhy : "found");
    if (t.omniWhy) ++g_failures;
    for (const Expect& e : expect) {
        const uint32_t rva = e.got ? uint32_t(static_cast<const char*>(e.got) - base) : 0;
        printf("  %-22s +0x%06X%s\n", e.name, rva, rva == e.rva ? "" : "   EXPECTED a different place");
        if (rva != e.rva) ++g_failures;
    }
    // soft check: the caster loop's technique (TTYPE_SHADOWGEN = 4) changed to 5 must make it refuse
    char* probe = t.prepareDepthMap + (0xBE90A - 0xBC560) + 7;  // mov dword ptr [rsp+0x2DC], 4
    if (uint8_t(*probe) != 4) {
        printf("  probe byte not found where expected\n");
        ++g_failures;
    } else {
        DWORD old;
        VirtualProtect(probe, 1, PAGE_EXECUTE_READWRITE, &old);
        *probe = 5;
        optiperf::ShadowPrepassTarget dummy;
        const bool located = optiperf::ShadowPrepass_Locate(base, &dummy, &why);
        *probe = 4;
        VirtualProtect(probe, 1, old, &old);
        printf("  altered code: %s\n", located ? "WRONGLY ACCEPTED" : why);
        if (located) ++g_failures;
    }
    // the side camera's code altered (the matrix copied into the camera: 0x30 bytes -> 0x31): the directional maps are
    // still handled, the point lights are left to the game
    char* omniProbe = t.prepareDepthMap + (0xBDFEA - 0xBC560);  // mov r8d, 0x30
    if (uint8_t(*omniProbe) != 0x30) {
        printf("  side-camera probe byte not found where expected\n");
        ++g_failures;
    } else {
        DWORD old;
        VirtualProtect(omniProbe, 1, PAGE_EXECUTE_READWRITE, &old);
        *omniProbe = 0x31;
        optiperf::ShadowPrepassTarget partial;
        const bool located = optiperf::ShadowPrepass_Locate(base, &partial, &why);
        *omniProbe = 0x30;
        VirtualProtect(omniProbe, 1, old, &old);
        const bool ok = located && partial.omniWhy && !partial.camInit && !partial.sinf;
        printf("  altered side-camera code: %s%s\n", located ? "located; point lights: " : "NOT LOCATED: ",
               located ? (partial.omniWhy ? partial.omniWhy : "WRONGLY ACCEPTED") : why);
        if (!ok) ++g_failures;
    }
    // the reflection part: the preprocess stage and FX_DrawToRenderTarget
    optiperf::ReflectionTarget rt;
    if (!optiperf::ReflectionPrepass_Locate(base, t, &rt, &why)) {
        printf("  reflections not located: %s\n", why);
        ++g_failures;
    } else {
        const Expect rexpect[] = {
            {"preprocess handler", rt.preprocess, 0xA6330},
            {"FX_DrawToRenderTarget", rt.drawToRT, 0x29790},
            {"its RenderWorld return", rt.renderWorldReturn, 0x2B923},
            {"cached ocean camera", rt.oceanCamera, 0x2D6160},
            {"panorama update", rt.panorama, 0x83150},
            {"preprocess list sort", rt.sortPreprocess, 0x110A00},
            {"flush render callback", rt.renderCallback, 0x2D6D0},
            {"ISystem* (reflections)", rt.system, 0x2E6A98},
            // rendering them early
            {"per-list setup", rt.startBatch, 0x9F2F0},
            {"per-list teardown", rt.endBatch, 0xA5200},
            {"water update flag", rt.updateFlag, 0x2AF220},
            {"water update counter", rt.updateCounter, 0x2D6378},
            {"r_NoDrawNear", rt.noDrawNear, 0x2E97B4},
            {"cvar below the water", rt.cvar980c, 0x2E980C},
            {"EnableFog", rt.enableFog, 0xDDE30},
            {"SetFog", rt.setFog, 0xDDCC0},
            {"SetSkyLightParams", rt.setSkyLight, 0xE5860},
            {"ITimer*", rt.timer, 0x2E6A90},
            {"flag-0x400 cvar 1", rt.noAsync[0], 0x2E997C},
            {"flag-0x400 cvar 2", rt.noAsync[1], 0x2E9B14},
            {"level polygon items", rt.polyItems, 0x1038E0},
            // far trees
            {"sprite quads", rt.spriteQuads, 0x2E0ED8},
            {"sprite quad count", rt.spriteCount, 0x2E0EE0},
        };
        printf("  rendering them early: %s; far trees: %s\n", rt.fullWhy ? rt.fullWhy : "possible",
               rt.farWhy ? rt.farWhy : "possible");
        if (rt.fullWhy || rt.farWhy) ++g_failures;
        for (const Expect& e : rexpect) {
            const uint32_t rva = e.got ? uint32_t(static_cast<const char*>(e.got) - base) : 0;
            printf("  %-22s +0x%06X%s\n", e.name, rva, rva == e.rva ? "" : "   EXPECTED a different place");
            if (rva != e.rva) ++g_failures;
        }
        // the water reflection's update code altered (its decision flag cleared by a different instruction): the
        // reflections are still located (observe), but not rendered early
        char* stateProbe = rt.drawToRT + (0x2A4F4 - 0x29790);  // mov byte ptr [flag], 0
        if (uint8_t(*stateProbe) != 0xC6) {
            printf("  water-update probe byte not found where expected\n");
            ++g_failures;
        } else {
            DWORD old;
            VirtualProtect(stateProbe, 1, PAGE_EXECUTE_READWRITE, &old);
            *reinterpret_cast<uint8_t*>(stateProbe) = 0xC7;
            optiperf::ReflectionTarget partial;
            const bool located = optiperf::ReflectionPrepass_Locate(base, t, &partial, &why);
            *reinterpret_cast<uint8_t*>(stateProbe) = 0xC6;
            VirtualProtect(stateProbe, 1, old, &old);
            const bool ok = located && partial.fullWhy && !partial.updateFlag && !partial.startBatch;
            printf("  altered water-update code: %s%s\n", located ? "located; early rendering: " : "NOT LOCATED: ",
                   located ? (partial.fullWhy ? partial.fullWhy : "WRONGLY ACCEPTED") : why);
            if (!ok) ++g_failures;
        }
    }
    // the particle part: mfPrepare, the allocator and its reservation, EF_ADDFillLight
    optiperf::ParticleTarget pt;
    if (!optiperf::ParticlePrepass_Locate(base, &pt, &why)) {
        printf("  particles not located: %s\n", why);
        ++g_failures;
    } else {
        const Expect pexpect[] = {
            {"CREParticle::mfPrepare", pt.prepare, 0x127BF0},
            {"CAllocRender::Alloc", pt.alloc, 0x127B50},
            {"CAllocRender vtable", pt.allocVtable, 0x262E28},
            {"particle reservation", pt.reserve, 0xA3430},
            {"EF_ADDFillLight", pt.addFillLight, 0x1041E0},
        };
        for (const Expect& e : pexpect) {
            const uint32_t rva = e.got ? uint32_t(static_cast<const char*>(e.got) - base) : 0;
            printf("  %-22s +0x%06X%s\n", e.name, rva, rva == e.rva ? "" : "   EXPECTED a different place");
            if (rva != e.rva) ++g_failures;
        }
        // the context's feature bit read differently (shr ebx, 0x1D -> 0x1C): refused
        auto* featureProbe = reinterpret_cast<uint8_t*>(pt.prepare + 0xB7);
        if (*featureProbe != 0x1D) {
            printf("  mfPrepare probe byte not found where expected\n");
            ++g_failures;
        } else {
            DWORD old;
            VirtualProtect(featureProbe, 1, PAGE_EXECUTE_READWRITE, &old);
            *featureProbe = 0x1C;
            optiperf::ParticleTarget altered;
            const bool located = optiperf::ParticlePrepass_Locate(base, &altered, &why);
            *featureProbe = 0x1D;
            VirtualProtect(featureProbe, 1, old, &old);
            printf("  altered mfPrepare: %s\n", located ? "WRONGLY ACCEPTED" : why);
            if (located) ++g_failures;
        }
    }
    FreeLibrary(mod);
}

// The counting vtable copy (vtable_counter.h) on an object of this program: calls counted, a slot redirected, a
// marked slot setting the marker, arguments on the stack passed through, nothing counted once it is swapped out.
struct Probe {
    virtual int A(int x) { return x + 1; }
    virtual int B(int x) { return x * 2; }
    virtual float C(float a, float b, float c, float d, float e) { return a + 2 * b + 3 * c + 4 * d + 5 * e; }
    virtual ~Probe() = default;
};
int ProbeReplacement(Probe*, int x) { return x + 100; }
struct Probe2 {  // another class, its slot added to the gate after the install
    virtual int D(int x) { return x * 3; }
    virtual ~Probe2() = default;
};

// a call through the object's vtable pointer as it is now (as the game's calls are)
template <class Fn, class... Args>
auto CallSlot(Probe* p, int slot, Args... args) {
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(p))[slot])(p, args...);
}

// The renderer's gate (render_gate.h), on the probe's vtable: a call on the gated thread calls back first, with its
// register and stack arguments kept through a callback that uses the volatile registers; another thread's calls, and
// every call once nothing is gated, go straight on; both resolvers see through the thunks.
namespace gate {
unsigned calls = 0, last = 0;
char scratch[64];
void OnGated(unsigned index) {
    ++calls;
    last = index;
    snprintf(scratch, sizeof scratch, "%f %d", std::sqrt(double(index) + 2.0) * 3.0, int(index));  // (clobbers)
}
}  // namespace gate

void TestRenderGate() {
    printf("the renderer's gate\n");
    using IntFn = int (*)(Probe*, int);
    using FloatFn = float (*)(Probe*, float, float, float, float, float);  // (the last two on the stack)
    Probe* p = new Probe;
    char** vt = *reinterpret_cast<char***>(p);
    char* const originals[3] = {vt[0], vt[1], vt[2]};
    const std::vector<optiperf::RttiVtable> vts = {{".?AUProbe@@", 0, vt, 3}};
    Check(optiperf::RenderGate_Install(vts, &gate::OnGated) && optiperf::RenderGate_Slots() == 3,
          "the gate installed on the probe's three slots");
    Check(vt[0] != originals[0] && optiperf::RenderGate_Original(vt[0]) == originals[0] &&
              optiperf::CallTrace_Original(vt[2]) == originals[2],
          "the slots go through thunks, which both resolvers see through");
    Check(CallSlot<IntFn>(p, 0, 1) == 2 && gate::calls == 0, "nothing gated: straight on");
    optiperf::RenderGate_Set(GetCurrentThreadId());
    const int b = CallSlot<IntFn>(p, 1, 21);
    const unsigned callsB = gate::calls, lastB = gate::last;
    const float c = CallSlot<FloatFn>(p, 2, 1.f, 2.f, 3.f, 4.f, 5.f);
    int other = 0;
    std::thread([p, &other] { other = CallSlot<IntFn>(p, 0, 41); }).join();
    optiperf::RenderGate_Set(0);
    const int after = CallSlot<IntFn>(p, 0, 1);
    Check(b == 42 && callsB == 1 && lastB == 1, "the gated thread's call calls back first, then goes on with its argument");
    Check(c == 55.f && gate::calls == 2 && gate::last == 2, "register and stack arguments kept through the callback");
    Check(other == 42 && after == 2 && gate::calls == 2,
          "another thread's calls, and all once nothing is gated, go straight on");
    // a slot's thunk sent on to another function: calls through the vtable reach it, the original is still known
    void* const was = optiperf::RenderGate_Redirect(0, reinterpret_cast<void*>(&ProbeReplacement));
    const int redirected = CallSlot<IntFn>(p, 0, 1);
    const bool known = optiperf::RenderGate_Original(vt[0]) == originals[0] && was == originals[0];
    optiperf::RenderGate_Redirect(0, originals[0]);
    Check(redirected == 101 && known && CallSlot<IntFn>(p, 0, 1) == 2,
          "a redirected slot goes to its new function, and back; its own function stays the original");
    char what[64];
    optiperf::RenderGate_Describe(2, what, sizeof what);
    Check(std::strcmp(what, "Probe slot 2") == 0, "a thunk index named by its class and slot");
    // a slot added after the install (another module's vtable, the barrier): gated like the others
    using Int2Fn = int (*)(Probe2*, int);
    Probe2* q = new Probe2;
    char** vt2 = *reinterpret_cast<char***>(q);
    char* const d = vt2[0];
    const int added = optiperf::RenderGate_AddSlot(vt2, 0, ".?AUProbe2@@");
    const auto call2 = [q](int x) { return reinterpret_cast<Int2Fn>((*reinterpret_cast<void***>(q))[0])(q, x); };
    const unsigned before = gate::calls;
    const int plain = call2(2);
    optiperf::RenderGate_Set(GetCurrentThreadId());
    const int gated = call2(5);
    optiperf::RenderGate_Set(0);
    optiperf::RenderGate_Describe(unsigned(added), what, sizeof what);
    Check(added == 3 && optiperf::RenderGate_Slots() == 4 && optiperf::RenderGate_AddSlot(vt2, 0, ".?AUProbe2@@") == 3,
          "an added slot takes the next thunk, once");
    Check(vt2[0] != d && optiperf::RenderGate_Original(vt2[0]) == d && optiperf::RenderGate_IndexOf(vt2, 0) == 3,
          "the added slot goes through its thunk, resolved to its function");
    Check(plain == 6 && gated == 15 && gate::calls == before + 1 && gate::last == 3,
          "the added slot calls back on the gated thread only, then goes on");
    Check(std::strcmp(what, "Probe2 slot 0") == 0, "the added slot named by its class and slot");
    delete q;
    delete p;  // (the destructor's slot isn't gated)
}

void TestVtableCounter() {
    printf("vtable counter\n");
    using IntFn = int (*)(Probe*, int);
    using FloatFn = float (*)(Probe*, float, float, float, float, float);  // (the last two on the stack)
    Probe* p = new Probe;
    char* exe = reinterpret_cast<char*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(exe + reinterpret_cast<IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    optiperf::VtableCounter c;
    Check(c.Init(p, exe, nt->OptionalHeader.SizeOfImage, 3) && c.Slots() == 3, "the copy covers the slots asked for");
    void** before = *reinterpret_cast<void***>(p);
    c.SetTarget(1, reinterpret_cast<void*>(&ProbeReplacement));
    c.Mark(2);
    c.Begin();
    Check(*reinterpret_cast<void***>(p) != before, "Begin swaps the vtable pointer");
    const int a = CallSlot<IntFn>(p, 0, 1), b = CallSlot<IntFn>(p, 1, 1);
    Check(!c.Marked(), "unmarked slots leave the marker alone");
    const float r = CallSlot<FloatFn>(p, 2, 1.f, 2.f, 3.f, 4.f, 5.f);
    c.End();
    Check(*reinterpret_cast<void***>(p) == before, "End puts the vtable pointer back");
    Check(a == 2 && c.Inside(0) == 1 && c.Outside(0) == 0, "a call from inside the module is counted as such");
    Check(b == 101 && c.Inside(1) == 1, "a redirected slot calls its target");
    Check(r == 55.f && c.Inside(2) == 1 && c.Marked(), "a marked slot sets the marker and passes stack arguments on");
    c.ClearMarker();
    Check(!c.Marked(), "the marker clears");
    // a marking thread (the flush's): another thread's call through a marked slot is counted but doesn't mark
    c.SetMarkThread(GetCurrentThreadId());
    c.Begin();
    std::thread([p] { CallSlot<FloatFn>(p, 2, 1.f, 2.f, 3.f, 4.f, 5.f); }).join();
    const bool otherMarked = c.Marked();
    CallSlot<FloatFn>(p, 2, 1.f, 2.f, 3.f, 4.f, 5.f);
    const bool ownMarked = c.Marked();
    c.ClearMarker();
    c.SetMarkThread(0);
    std::thread([p] { CallSlot<FloatFn>(p, 2, 1.f, 2.f, 3.f, 4.f, 5.f); }).join();
    const bool anyMarked = c.Marked();
    c.End();
    c.ClearMarker();
    Check(!otherMarked && ownMarked && anyMarked && c.Inside(2) == 4,
          "with a marking thread only its calls mark, and with none any thread's (all counted)");
    Check(CallSlot<IntFn>(p, 0, 1) == 2 && c.Inside(0) == 1, "nothing is counted once swapped out");
    delete p;
}

// The particle record and replay (particle_record.h) against a model of the renderer's particle batch (CAllocRender
// and its reservation): a producer written like SetVertices, run straight into the batch, and recorded then replayed
// into the same batch, must leave the same draws, the same batch and the same reservations.
struct FakeBatch {
    int maxV = 0, maxI = 0;  // the buffer sizes grants are cut to
    std::vector<uint8_t> vb;
    std::vector<uint16_t> ib;
    int batchV = 0, batchI = 0;
    bool open = false;  // a current element (the renderer's +0xA88): the next reservation draws first
    std::vector<std::vector<uint8_t>> draws;
    std::vector<int> calls;  // per reservation: requested and granted vertices and indices
    void Init(int v, int i) {
        maxV = v;
        maxI = i;
        vb.assign(size_t(v) * optiperf::pp::kVertexSize, 0);
        ib.assign(size_t(i), 0);
    }
    void Draw() {
        std::vector<uint8_t> d(vb.begin(), vb.begin() + ptrdiff_t(size_t(batchV) * optiperf::pp::kVertexSize));
        const auto* p = reinterpret_cast<const uint8_t*>(ib.data());
        d.insert(d.end(), p, p + size_t(batchI) * 2);
        draws.push_back(std::move(d));
        batchV = batchI = 0;
    }
    void Alloc(optiperf::pp::RenderVerts* rv, int nV, int nI) {
        batchV += rv->nVerts;
        batchI += rv->nInds;
        if (nV == 0) return;
        int gv = nV, gi = nI;
        if (open || nV + batchV >= maxV || nI + batchI >= maxI) {
            Draw();
            open = false;
            optiperf::pp::Grant(nV, nI, maxV, maxI, &gv, &gi);
        }
        calls.insert(calls.end(), {nV, nI, gv, gi});
        rv->verts = vb.data() + size_t(batchV) * optiperf::pp::kVertexSize;
        rv->inds = ib.data() + batchI;
        rv->base = batchV;
        rv->nVerts = gv;
        rv->nInds = gi;
    }
};

// quads, as SetVertices' direct path writes them: asks for room for all particles left, fills what was granted
template <class AllocFn>
void ProduceQuads(AllocFn&& alloc, int n, uint32_t seed) {
    optiperf::pp::RenderVerts rv{};
    int capV = 0, capI = 0, wV = 0, wI = 0;
    for (int p = 0; p < n; ++p) {
        if (wV + 4 > capV || wI + 6 > capI) {
            rv.nVerts = wV;
            rv.nInds = wI;
            alloc(&rv, (n - p) * 4, (n - p) * 6);
            capV = rv.nVerts;
            capI = rv.nInds;
            wV = wI = 0;
            if (capV < 4 || capI < 6) return;  // (SetVertices gives up too)
        }
        uint8_t* v = rv.verts + size_t(wV) * optiperf::pp::kVertexSize;
        for (size_t k = 0; k < 4 * optiperf::pp::kVertexSize; ++k) v[k] = uint8_t(seed * 131 + uint32_t(p) * 7 + uint32_t(k));
        const uint16_t b = uint16_t(rv.base + wV);
        const uint16_t q[6] = {0, 1, 2, 3, 2, 1};
        for (int k = 0; k < 6; ++k) rv.inds[wI + k] = uint16_t(b + q[k]);
        wV += 4;
        wI += 6;
    }
    rv.nVerts = wV;
    rv.nInds = wI;
    alloc(&rv, 0, 0);
}

void TestParticleRecord() {
    printf("particle record and replay\n");
    namespace pp = optiperf::pp;
    std::mt19937 r(5);
    bool same = true;
    int cut = 0, drawnBetween = 0;
    pp::Recorder rec;
    for (int round = 0; round < 1000; ++round) {
        const int maxV = 16 + int(r() % 200), maxI = 3 * (16 + int(r() % 200));
        const int n = int(r() % 160), earlier = int(r() % 40);
        const bool open = r() % 2 == 0;
        const uint32_t seed = r();
        FakeBatch a, b;
        a.Init(maxV, maxI);
        b.Init(maxV, maxI);
        for (FakeBatch* f : {&a, &b}) {  // earlier elements' particles in the batch
            ProduceQuads([&](pp::RenderVerts* rv, int v, int i) { f->Alloc(rv, v, i); }, earlier, 99);
            f->open = open;
        }
        ProduceQuads([&](pp::RenderVerts* rv, int v, int i) { a.Alloc(rv, v, i); }, n, seed);  // vanilla
        rec.Reset();
        pp::Record rc;
        rec.Begin(rc, maxV, maxI);
        ProduceQuads([&](pp::RenderVerts* rv, int v, int i) { rec.Alloc(rv, v, i); }, n, seed);
        rec.End(rc, 1);
        const int mismatches = rec.Replay(rc, [&](pp::RenderVerts* rv, int v, int i) { b.Alloc(rv, v, i); });
        a.Draw();
        b.Draw();
        same &= mismatches == 0 && a.draws == b.draws && a.calls == b.calls;
        if (n * 4 >= maxV || n * 6 >= maxI) ++cut;
        if (a.draws.size() > 2) ++drawnBetween;
    }
    Check(same, "a recorded producer replays into the batch exactly as it would have written there");
    // SetVertices updates its per-frame counters only after its final commit (an empty container makes no call)
    {
        pp::Recorder c;
        pp::Record none, request, full;
        c.Begin(none, 100, 300);
        c.End(none, 1);
        c.Begin(request, 100, 300);
        pp::RenderVerts rv{};
        c.Alloc(&rv, 8, 12);
        c.End(request, 0);
        c.Begin(full, 100, 300);
        ProduceQuads([&](pp::RenderVerts* v, int nv, int ni) { c.Alloc(v, nv, ni); }, 5, 1);
        c.End(full, 1);
        Check(!c.Committed(none) && !c.Committed(request) && c.Committed(full),
              "a record says whether its call ended with the final commit");
    }
    Check(cut > 100 && drawnBetween > 100, "the rounds cover grants cut to the buffer and batches drawn in between");
    // comparing records (verify mode)
    pp::Recorder r1, r2;
    pp::Record x, y;
    const char* what = nullptr;
    auto record = [&](pp::Recorder& to, pp::Record& rc, int n, uint32_t seed, int lights) {
        to.Reset();
        to.Begin(rc, 100, 300);
        ProduceQuads([&](pp::RenderVerts* rv, int v, int i) { to.Alloc(rv, v, i); }, n, seed);
        for (int k = 0; k < lights; ++k) {
            const uint8_t light[pp::kLightSize] = {uint8_t(k), 1, 2, 3};
            to.Light(light);
        }
        to.End(rc, 1);
    };
    record(r1, x, 60, 7, 2);
    record(r2, y, 60, 7, 2);
    Check(r1.Same(x, r2, y, &what), "the same output compares equal");
    record(r2, y, 60, 8, 2);
    Check(!r1.Same(x, r2, y, &what) && what && std::strcmp(what, "vertices") == 0, "different vertices are found");
    record(r2, y, 59, 7, 2);
    Check(!r1.Same(x, r2, y, &what) && what && std::strcmp(what, "calls") == 0, "a different particle count is found");
    record(r2, y, 60, 7, 1);
    Check(!r1.Same(x, r2, y, &what) && what && std::strcmp(what, "lights") == 0, "a missing fill light is found");
}

// The environment snapshot's getters in Cry3DEngine.dll: each one's code at its C3DEngine vtable slot. In the game the
// vtable comes from the engine object; here from its place in build 6156.
void TestEnvironment(const std::wstring& dir) {
    printf("locating the environment getters in Cry3DEngine.dll\n");
    const std::wstring path = dir + L"\\Cry3DEngine.dll";
    HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!mod) {
        printf("  cannot map %ls (error %lu)\n", path.c_str(), GetLastError());
        ++g_failures;
        return;
    }
    char* base = reinterpret_cast<char*>(mod);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint64_t preferred = nt->OptionalHeader.ImageBase, size = nt->OptionalHeader.SizeOfImage;
    constexpr uint32_t kVtable = 0x1D37E8;  // C3DEngine (RTTI)
    void* const* vt = reinterpret_cast<void* const*>(base + kVtable);
    std::vector<void*> entries;  // (rebased, in case the image was not relocated)
    for (int i = 0; i < 256; ++i) {
        uint64_t e = reinterpret_cast<uint64_t>(vt[i]);
        if (e - uint64_t(base) >= size && e - preferred < size) e = e - preferred + uint64_t(base);
        if (!optiperf::IsInCode(base, reinterpret_cast<const char*>(e), 1)) break;
        entries.push_back(reinterpret_cast<void*>(e));
    }
    printf("  vtable: %zu slots\n", entries.size());
    optiperf::EnvGetter got[optiperf::kEnvGetters];
    const int n = optiperf::EnvSnapshot_Locate(base, entries.data(), int(entries.size()), got);
    printf("  getters as expected: %d of %d\n", n, optiperf::kEnvGetters);
    if (n != optiperf::kEnvGetters) ++g_failures;
    // build 6156
    const uint32_t expect[optiperf::kEnvGetters] = {0xF1720, 0xF1730, 0xF8AC0, 0xF39D0, 0xF3B70, 0xF3BA0, 0xF6DD0,
                                                    0x1A8380, 0xF1440, 0xF14A0, 0xF14F0, 0xF14D0, 0xF14E0, 0xF35A0,
                                                    0xF15E0, 0xF1600, 0xF1620, 0xF7010, 0xF37D0, 0xF3820,
                                                    0xF1590, 0xF15B0, 0xF17C0, 0xF18D0, 0xF4990, 0x110BE0};
    for (int i = 0; i < optiperf::kEnvGetters && n >= 0; ++i) {
        const uint32_t rva = got[i].fn ? uint32_t(got[i].fn - base) : 0;
        printf("  %3d %-30s +0x%06X%s\n", got[i].slot, got[i].name, rva, rva == expect[i] ? "" : "   EXPECTED a different place");
        if (rva != expect[i]) ++g_failures;
    }
    // GetSunColor's field altered (the object manager's +0xC8 -> +0xC9): only that getter is left to the 3D engine
    char* probe = base + 0xF3820 + 0x13;  // lea rcx, [rax+0xC8]
    if (uint8_t(*probe) != 0xC8) {
        printf("  GetSunColor probe byte not found where expected\n");
        ++g_failures;
    } else {
        DWORD old;
        VirtualProtect(probe, 1, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<uint8_t*>(probe) = 0xC9;
        optiperf::EnvGetter partial[optiperf::kEnvGetters];
        const int m = optiperf::EnvSnapshot_Locate(base, entries.data(), int(entries.size()), partial);
        *reinterpret_cast<uint8_t*>(probe) = 0xC8;
        VirtualProtect(probe, 1, old, &old);
        int refused = -1;
        for (int i = 0; i < optiperf::kEnvGetters; ++i)
            if (!partial[i].fn) refused = partial[i].slot;
        const bool ok = m == optiperf::kEnvGetters - 1 && refused == 92;
        printf("  altered GetSunColor: %d getters as expected, slot %d left out%s\n", m, refused, ok ? "" : "   WRONG");
        if (!ok) ++g_failures;
    }
    // a vtable too short for the setters' slots: refused as a whole
    const bool shortRefused = optiperf::EnvSnapshot_Locate(base, entries.data(), 150, got) < 0;
    printf("  short vtable: %s\n", shortRefused ? "refused" : "WRONGLY ACCEPTED");
    if (!shortRefused) ++g_failures;
    // the particle part: the vertex creators' SetVertices, slot 0 of CParticleContainer's IParticleVertexCreator
    // vtable and of CParticleDirectEmitter's
    for (const uint32_t vtRva : {0x1CE748u, 0x1CE9B0u}) {
        uint64_t e = *reinterpret_cast<const uint64_t*>(base + vtRva);
        if (e - uint64_t(base) >= size && e - preferred < size) e = e - preferred + uint64_t(base);
        char* fn = reinterpret_cast<char*>(e);
        const bool ok = optiperf::ParticlePrepass_IsSetVertices(base, fn) && uint64_t(fn - base) == 0x84AE0;
        printf("  vertex creator vtable +0x%06X: SetVertices %s\n", vtRva, ok ? "found" : "NOT FOUND");
        if (!ok) ++g_failures;
    }
    // ... and the per-particle vertex function it calls when it computes the vertices itself
    const char* pvWhy = "";
    char* pv = optiperf::ParticlePrepass_LocateParticleVerts(base, &pvWhy);
    const bool pvOk = pv && uint64_t(pv - base) == 0x83750;
    printf("  the per-particle vertex function: %s\n", pvOk ? "found at +0x83750" : pvWhy);
    if (!pvOk) ++g_failures;
    // the positional calls: CVisArea's vtable (by RTTI, a struct's name) with the two slots the sun shafts call, and
    // the sun shafts' test in CryRenderD3D10.dll
    void** visVt = optiperf::EnvSnapshot_LocateVisArea(base);
    const bool visOk = visVt && uint64_t(reinterpret_cast<char*>(visVt) - base) == 0x1D5A98;
    printf("  CVisArea's vtable: %s\n", visOk ? "found at +0x1D5A98, its two slots as expected" : "NOT FOUND");
    if (!visOk) ++g_failures;
    HMODULE render = LoadLibraryExW((dir + L"\\CryRenderD3D10.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    char* shafts = render ? optiperf::EnvSnapshot_LocateSunShafts(reinterpret_cast<char*>(render)) : nullptr;
    const bool shaftsOk = shafts && uint64_t(shafts - reinterpret_cast<char*>(render)) == 0x1AD8E4;
    printf("  the sun shafts' test: %s\n", shaftsOk ? "found at CryRenderD3D10 +0x1AD8E4" : "NOT FOUND");
    if (!shaftsOk) ++g_failures;
    if (render) FreeLibrary(render);
    FreeLibrary(mod);
}

// --- skinning ----------------------------------------------------------------------------------------------------------
// Synthetic GetSkeletonPose implementations, writing what the game's write: a character its bones, the motion-blur
// bones when asked (flag 4), the shape weights and the flags, not the remap; a skin attachment the remap too; a plant
// all four bone pointers and the remap, no shape weights.
namespace fake {
uint8_t bones[64 * 32], mbBones[64 * 32], remapTable[64];

uint32_t CharPose(void*, uint32_t lod, const float*, void** l, void** s, void** ml, void** ms, float* shape,
                  uint32_t* flags, void**) {
    const uint32_t in = *flags;
    *l = *s = *ml = *ms = nullptr;
    *l = bones + lod * 32;
    if (in & 4) *ml = mbBones;
    for (int i = 0; i < 32; ++i) shape[i] = float(i) * 0.25f + float(lod);
    *flags = 1u | (lod == 1 ? 2u : 0u);
    return 40 + lod;
}
uint32_t SkinPose(void*, uint32_t lod, const float*, void** l, void** s, void** ml, void** ms, float* shape,
                  uint32_t* flags, void** remap) {
    const uint32_t in = *flags;
    *l = *s = *ml = *ms = nullptr;
    *remap = remapTable;
    *s = bones;
    if (in & 4) *ms = mbBones + lod * 32;
    for (int i = 0; i < 32; ++i) shape[i] = 1.0f - float(i) / 32.0f;
    *flags = 1;
    return 20;
}
uint32_t FoliagePose(void*, uint32_t, const float*, void** l, void** s, void** ml, void** ms, float*, uint32_t* flags,
                     void** remap) {
    *remap = nullptr;
    *l = *s = bones;
    *ml = *ms = mbBones;
    *flags = 0;
    return 12;
}
}  // namespace fake

struct RendererCall {  // what the renderer's call leaves in its variables
    void* p[5];
    float shape[34];
    uint32_t flags, ret;
};
static_assert(sizeof(RendererCall) == 184, "no padding: compared with memcmp");

RendererCall Fresh(uint32_t flagsIn) {
    RendererCall c;
    std::memset(&c, 0, sizeof c);
    for (float& f : c.shape) f = 1234.5f;
    c.flags = flagsIn;
    return c;
}
RendererCall Direct(optiperf::sk::PoseFn f, uint32_t lod, uint32_t flagsIn) {
    RendererCall c = Fresh(flagsIn);
    c.ret = f(nullptr, lod, nullptr, &c.p[0], &c.p[1], &c.p[2], &c.p[3], c.shape, &c.flags, &c.p[4]);
    return c;
}
RendererCall Replayed(const optiperf::sk::Outputs& o, uint32_t flagsIn) {
    RendererCall c = Fresh(flagsIn);
    c.ret = optiperf::sk::Apply(o, &c.p[0], &c.p[1], &c.p[2], &c.p[3], c.shape, &c.flags, &c.p[4]);
    return c;
}

void TestSkinRecord() {
    namespace sk = optiperf::sk;
    printf("skinning: GetSkeletonPose answers recorded and replayed\n");
    const sk::PoseFn fns[] = {&fake::CharPose, &fake::SkinPose, &fake::FoliagePose};
    const uint8_t expectWritten[] = {0x2F, 0x3F, 0x1F};  // pointers L, S, mbL, mbS (, remap), shape weights (0x20)
    int same = 0, cases = 0;
    for (int k = 0; k < 3; ++k) {
        for (uint32_t lod = 0; lod < 3; ++lod) {
            for (uint32_t flags : {2u, 6u}) {
                sk::Outputs o;
                sk::Prepare(o, flags);
                sk::Finish(o, sk::Call(fns[k], nullptr, lod, nullptr, o));
                const RendererCall a = Direct(fns[k], lod, flags), b = Replayed(o, flags);
                ++cases;
                if (std::memcmp(&a, &b, sizeof a) == 0) ++same;
                Check(o.written == expectWritten[k], "the outputs a call writes are found");
                sk::Outputs again;
                sk::Prepare(again, flags);
                sk::Finish(again, sk::Call(fns[k], nullptr, lod, nullptr, again));
                const char* what = nullptr;
                Check(sk::Same(o, again, &what), "two calls with the same answer are the same");
            }
        }
    }
    printf("  replays leaving what the call leaves: %d of %d\n", same, cases);
    Check(same == cases, "every replay leaves the renderer's variables as the call does");
    // differences are found, and where
    sk::Outputs a, b;
    sk::Prepare(a, 6);
    sk::Finish(a, sk::Call(&fake::CharPose, nullptr, 0, nullptr, a));
    const char* what = nullptr;
    b = a;
    b.p[sk::kMbQuatsL] = fake::bones;
    Check(!sk::Same(a, b, &what) && what && std::strcmp(what, "mbQuatsL") == 0, "another motion-blur pointer is found");
    b = a;
    b.shape[31] += 1.0f;
    Check(!sk::Same(a, b, &what) && what && std::strcmp(what, "shape deformation") == 0, "other shape weights are found");
    b = a;
    b.ret = 39;
    Check(!sk::Same(a, b, &what) && what && std::strcmp(what, "bone count") == 0, "another bone count is found");
    b = a;
    b.written &= uint8_t(~(1 << sk::kQuatsS));
    Check(!sk::Same(a, b, &what) && what && std::strcmp(what, "outputs written") == 0, "other outputs written are found");
    // copies of what the answers point at: the bones two chunks' ids read (an attachment's through its remap table)
    // and the table's bytes they read; the answer points at the copies, which read as the call's pointers do
    for (size_t i = 0; i < sizeof fake::bones; ++i) {
        fake::bones[i] = uint8_t(i * 7 + 1);
        fake::mbBones[i] = uint8_t(i * 13 + 5);
    }
    for (int i = 0; i < 64; ++i) fake::remapTable[i] = uint8_t((i * 5) % 48);
    const uint16_t chunkA[] = {0, 3, 17, 5}, chunkB[] = {12, 30, 2};
    const uint32_t expectBones[] = {31, 38, 31}, expectRemap[] = {0, 31, 0};  // (remap[17] = 37 is the largest)
    int copiesOk = 0, copyCases = 0;
    std::vector<uint8_t> arena;
    for (int k = 0; k < 3; ++k) {
        for (uint32_t flags : {2u, 6u}) {
            sk::Outputs o;
            sk::Prepare(o, flags);
            sk::Finish(o, sk::Call(fns[k], nullptr, 1, nullptr, o));
            const auto* remap =
                (o.written & (1 << sk::kRemap)) ? static_cast<const uint8_t*>(o.p[sk::kRemap]) : nullptr;
            sk::Extents e;
            sk::AddChunk(e, chunkA, 4, remap);
            sk::AddChunk(e, chunkB, 3, remap);
            uint32_t at[sk::kPtrOutputs];
            arena.assign(3, 0);  // (an odd start: the copies are aligned anyway)
            sk::CopyOut(o, e, flags, arena, at);
            const sk::Outputs answer = sk::WithCopies(o, arena.data(), at);
            ++copyCases;
            bool ok = e.bones == expectBones[k] && e.remap == expectRemap[k] && sk::SameCopies(o, e, arena.data(), at);
            for (int i = 0; i < sk::kPtrOutputs; ++i) {
                if (!(o.written & (1 << i)) || !o.p[i] || !sk::Reads(i, flags)) {  // (the motion-blur pose: flags 6)
                    ok = ok && at[i] == sk::kNotCopied && answer.p[i] == o.p[i];
                    continue;
                }
                ok = ok && at[i] != sk::kNotCopied && at[i] % 16 == 0 && answer.p[i] == arena.data() + at[i];
            }
            for (const uint16_t* ids : {chunkA, chunkB}) {  // the renderer's reads, through either
                for (int j = 0; j < (ids == chunkA ? 4 : 3); ++j) {
                    const uint32_t bone = remap ? remap[ids[j]] : ids[j];
                    for (int i = 0; i < sk::kPtrOutputs; ++i) {
                        if (at[i] == sk::kNotCopied) continue;
                        const auto* mine = static_cast<const uint8_t*>(answer.p[i]);
                        const auto* theirs = static_cast<const uint8_t*>(o.p[i]);
                        ok = ok && (i == sk::kRemap ? mine[ids[j]] == theirs[ids[j]]
                                                    : std::memcmp(mine + bone * sk::kBoneBytes,
                                                                  theirs + bone * sk::kBoneBytes, sk::kBoneBytes) == 0);
                    }
                }
            }
            // the skinnable's array changed after the copy: seen
            uint8_t* changed = k == 1 ? fake::bones : static_cast<uint8_t*>(o.p[sk::kQuatsL]);
            changed[sk::kBoneBytes * 2 + 3] ^= 1;
            ok = ok && !sk::SameCopies(o, e, arena.data(), at);
            changed[sk::kBoneBytes * 2 + 3] ^= 1;
            ok = ok && sk::SameCopies(o, e, arena.data(), at);
            if (ok) ++copiesOk;
        }
    }
    printf("  bone copies read as the call's pointers do: %d of %d\n", copiesOk, copyCases);
    Check(copiesOk == copyCases, "the copies hold what the chunks read, aligned, and the answers point at them");
    // the key index: LOD and flags apart, cleared per flush
    sk::KeyIndex index;
    std::vector<uint8_t> objects(4096);
    for (int round = 0; round < 3; ++round) {
        index.Clear();
        int found = 0;
        for (uint32_t i = 0; i < 1500; ++i) {
            const uint64_t key = sk::Key(objects.data() + (i % 500) * 8, i / 500, 2 + 4 * (round & 1));
            if (!index.Find(key)) index.Insert(key, i);
        }
        for (uint32_t i = 0; i < 1500; ++i) {
            const uint32_t* v = index.Find(sk::Key(objects.data() + (i % 500) * 8, i / 500, 2 + 4 * (round & 1)));
            if (v && *v == i) ++found;
        }
        Check(found == 1500 && index.size() == 1500, "every key is found again, LODs apart");
        Check(!index.Find(sk::Key(objects.data(), 0, 6 - 4 * (round & 1))), "the other flags are another key");
    }
    index.Clear();
    Check(!index.Find(sk::Key(objects.data(), 0, 2)), "a cleared index finds nothing");
}

// The skinning part's code: the renderer's call site, and the three skinnables' vtables and GetSkeletonPose (RTTI).
void TestSkinLocate(const std::wstring& dir) {
    printf("locating the skinning code in CryRenderD3D10.dll, CryAnimation.dll and Cry3DEngine.dll\n");
    HMODULE render = LoadLibraryExW((dir + L"\\CryRenderD3D10.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    HMODULE anim = LoadLibraryExW((dir + L"\\CryAnimation.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    HMODULE e3d = LoadLibraryExW((dir + L"\\Cry3DEngine.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!render || !anim || !e3d) {
        printf("  cannot map them (error %lu)\n", GetLastError());
        ++g_failures;
    } else {
        char* const rb = reinterpret_cast<char*>(render);
        char* const ab = reinterpret_cast<char*>(anim);
        char* const eb = reinterpret_cast<char*>(e3d);
        optiperf::SkinTarget t;
        const char* why = nullptr;
        const bool r = optiperf::SkinPrepass_LocateRenderer(rb, &t, &why);
        printf("  the skinned draw's call returns at: %s +0x%X, CREMesh::mfIsHWSkinned +0x%X\n", r ? "" : why,
               r ? unsigned(t.callReturn - rb) : 0u, r ? unsigned(t.isHWSkinned - rb) : 0u);
        Check(r && t.callReturn - rb == 0x26C8E, "the renderer's GetSkeletonPose call (build 6156: returns at +0x26C8E)");
        Check(r && t.isHWSkinned - rb == 0x126670, "CREMesh::mfIsHWSkinned (build 6156: +0x126670)");
        // the elements' slot 8: CREMesh's reads its chunk, CREParticle's (like most) returns false
        void** meshVt = optiperf::FindRttiVtable(rb, ".?AVCREMesh@@");
        void** particleVt = optiperf::FindRttiVtable(rb, ".?AVCREParticle@@");
        const int km = r && meshVt ? optiperf::SkinPrepass_ElementKind(rb, t, meshVt) : -1;
        const int kp = r && particleVt ? optiperf::SkinPrepass_ElementKind(rb, t, particleVt) : -1;
        printf("  element slot 8: CREMesh %d, CREParticle %d (1 = the chunk's bones, 0 = false)\n", km, kp);
        Check(km == 1 && kp == 0, "the elements' skinned test is told apart");
        const bool s = optiperf::SkinPrepass_LocateSkinnables(ab, eb, &t, &why);
        if (!s) printf("  skinnables: %s\n", why);
        Check(s, "the three skinnables are found");
        const char* const names[] = {"CCharInstance", "CSkinInstance", "CStatObjFoliage"};
        char* const bases[] = {ab, ab, eb};
        const uint32_t vtables[] = {0x1692A8, 0x169B58, 0x1CBFC8}, poses[] = {0x8CA0, 0xC2D0, 0x1C430};
        for (int i = 0; s && i < optiperf::kSkinImpls; ++i) {
            const uint32_t vt = uint32_t(reinterpret_cast<char*>(t.vtables[i]) - bases[i]);
            const uint32_t fn = uint32_t(t.poses[i] - bases[i]);
            const bool ok = vt == vtables[i] && fn == poses[i];
            printf("  %-16s vtable +0x%06X, GetSkeletonPose +0x%06X%s\n", names[i], vt, fn, ok ? "" : "   EXPECTED elsewhere");
            if (!ok) ++g_failures;
        }
        // the morph targets' copy (MorphTargetCopy's two paths) and the renderer pointer it uses
        const bool mc = optiperf::SkinPrepass_LocateMorphCopy(ab, &t, &why);
        const unsigned copyRva = mc ? unsigned(t.morphCopy - ab) : 0u, seqRva = mc ? unsigned(t.morphSequence - ab) : 0u;
        const unsigned rendRva = mc ? unsigned(reinterpret_cast<char*>(t.animRenderer) - ab) : 0u;
        printf("  morph targets' copy: %s +0x%X, sequence +0x%X, renderer pointer +0x%X\n", mc ? "" : why, copyRva, seqRva,
               rendRva);
        Check(mc && copyRva == 0x8458 && seqRva == 0x8659 && rendRva == 0x19F5B0,
              "MorphTargetCopy's copy (build 6156: +0x8458, +0x8659, renderer +0x19F5B0)");
        // CCharInstance's attachment type check altered (cmp [rax+0x130], 2 -> 3): refused
        char* probe = ab + 0x8CA0 + 0x31;
        if (uint8_t(*probe) != 0x02) {
            printf("  CCharInstance probe byte not found where expected\n");
            ++g_failures;
        } else {
            DWORD old;
            VirtualProtect(probe, 1, PAGE_EXECUTE_READWRITE, &old);
            *reinterpret_cast<uint8_t*>(probe) = 0x03;
            optiperf::SkinTarget u;
            const char* why2 = nullptr;
            const bool refused = !optiperf::SkinPrepass_LocateSkinnables(ab, eb, &u, &why2) && why2 &&
                                 std::strstr(why2, "CCharInstance") != nullptr;
            *reinterpret_cast<uint8_t*>(probe) = 0x02;
            VirtualProtect(probe, 1, old, &old);
            printf("  altered CCharInstance::GetSkeletonPose: %s\n", refused ? "refused" : "WRONGLY ACCEPTED");
            if (!refused) ++g_failures;
        }
    }
    if (render) FreeLibrary(render);
    if (anim) FreeLibrary(anim);
    if (e3d) FreeLibrary(e3d);
}

// The system snapshot's getters: CSystem's and CTimer's vtables (found by RTTI), each getter at its slot.
std::vector<void*> VtableEntries(char* base, void* const* vt) {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint64_t preferred = nt->OptionalHeader.ImageBase, size = nt->OptionalHeader.SizeOfImage;
    std::vector<void*> out;  // (rebased, in case the image was not relocated)
    for (int i = 0; vt && i < 256; ++i) {
        uint64_t e = reinterpret_cast<uint64_t>(vt[i]);
        if (e - uint64_t(base) >= size && e - preferred < size) e = e - preferred + uint64_t(base);
        if (!optiperf::IsInCode(base, reinterpret_cast<const char*>(e), 1)) break;
        out.push_back(reinterpret_cast<void*>(e));
    }
    return out;
}

void TestSystem(const std::wstring& dir) {
    printf("locating the system snapshot's getters in CrySystem.dll\n");
    const std::wstring path = dir + L"\\CrySystem.dll";
    HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!mod) {
        printf("  cannot map %ls (error %lu)\n", path.c_str(), GetLastError());
        ++g_failures;
        return;
    }
    char* base = reinterpret_cast<char*>(mod);
    void** svt = optiperf::FindRttiVtable(base, ".?AVCSystem@@");
    void** tvt = optiperf::FindRttiVtable(base, ".?AVCTimer@@");
    const unsigned srva = svt ? unsigned(reinterpret_cast<char*>(svt) - base) : 0u;
    const unsigned trva = tvt ? unsigned(reinterpret_cast<char*>(tvt) - base) : 0u;
    std::vector<void*> se = VtableEntries(base, svt), te = VtableEntries(base, tvt);
    printf("  vtables: CSystem +0x%X (%zu slots), CTimer +0x%X (%zu slots)\n", srva, se.size(), trva, te.size());
    Check(srva == 0x26ACF8 && trva == 0x26AC28, "CSystem's and CTimer's vtables (build 6156: +0x26ACF8, +0x26AC28)");
    optiperf::SysGetter got[optiperf::kSysGetters];
    const int n = svt && tvt ? optiperf::SysSnapshot_Locate(base, se.data(), int(se.size()), te.data(), int(te.size()), got)
                             : -1;
    printf("  getters as expected: %d of %d\n", n, optiperf::kSysGetters);
    if (n != optiperf::kSysGetters) ++g_failures;
    const uint32_t expect[optiperf::kSysGetters] = {0x33570, 0x4C1B0, 0x336D0, 0x33680, 0x338E0, 0x57F90,
                                                    0x57FD0, 0x58010, 0x57FB0, 0x57FE0, 0x57FF0};
    for (int i = 0; i < optiperf::kSysGetters && n >= 0; ++i) {
        const uint32_t rva = got[i].fn ? uint32_t(got[i].fn - base) : 0;
        printf("  %s %3d %-20s +0x%06X%s\n", got[i].object ? "ITimer " : "ISystem", got[i].slot, got[i].name, rva,
               rva == expect[i] ? "" : "   EXPECTED a different place");
        if (rva != expect[i]) ++g_failures;
    }
    // GetTimeScale's field altered (+0x24C -> +0x24D): only that getter is left to CrySystem
    char* probe = base + 0x57FF0 + 4;  // movss xmm0, [rcx+0x24C]
    if (n >= 0 && uint8_t(*probe) == 0x4C) {
        DWORD old;
        VirtualProtect(probe, 1, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<uint8_t*>(probe) = 0x4D;
        optiperf::SysGetter partial[optiperf::kSysGetters];
        const int m = optiperf::SysSnapshot_Locate(base, se.data(), int(se.size()), te.data(), int(te.size()), partial);
        *reinterpret_cast<uint8_t*>(probe) = 0x4C;
        VirtualProtect(probe, 1, old, &old);
        int refused = -1;
        for (int i = 0; i < optiperf::kSysGetters; ++i)
            if (!partial[i].fn) refused = i;
        const bool ok = m == optiperf::kSysGetters - 1 && refused == optiperf::kSysGetters - 1;
        printf("  altered GetTimeScale: %d getters as expected, %s left out%s\n", m,
               refused >= 0 ? partial[refused].name : "none", ok ? "" : "   WRONG");
        if (!ok) ++g_failures;
    } else {
        printf("  GetTimeScale probe byte not found where expected\n");
        ++g_failures;
    }
    // vtables too short for the setters' slots: refused as a whole
    const bool shortRefused =
        n >= 0 && optiperf::SysSnapshot_Locate(base, se.data(), 60, te.data(), int(te.size()), got) < 0;
    printf("  short vtable: %s\n", shortRefused ? "refused" : "WRONGLY ACCEPTED");
    if (!shortRefused) ++g_failures;
    FreeLibrary(mod);
}

// --- fog volumes ------------------------------------------------------------------------------------------------------
// The test's sqrtf and expf, put in the import slots the 3D engine's thunks jump through: the game's code and the
// transcription then call the same functions (in the game, MSVCR80's), so everything else has to match bit for bit.
float FogSqrt(float x) { return std::sqrt(x); }
float FogExp(float x) { return std::exp(x); }

bool PatchImport(char* thunk, const void* fn) {
    if (uint8_t(thunk[0]) != 0xFF || uint8_t(thunk[1]) != 0x25) return false;
    char* slot = thunk + 6 + int32_t(optiperf::ReadU32(thunk + 2));
    DWORD old;
    if (!VirtualProtect(slot, sizeof fn, PAGE_READWRITE, &old)) return false;
    std::memcpy(slot, &fn, sizeof fn);
    VirtualProtect(slot, sizeof fn, old, &old);
    return true;
}

template <class T>
void Put(uint8_t* p, int off, T v) {
    std::memcpy(p + off, &v, sizeof v);
}

// A fog volume as CFogVolumeRenderNode holds what the math reads: the unit sphere (type 0) or cube (1) in its own
// space, placed by a rotation, a scale and a position; with its world box, flags, density, height falloff, soft edge
// and colour.
struct FogVolume {
    alignas(16) uint8_t n[optiperf::fog::kNodeBytes];
    double rot[3][3], scale[3], pos[3];
};

void MakeFogVolume(std::mt19937& r, const float* around, FogVolume& v) {
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    std::memset(v.n, 0, sizeof v.n);
    const bool aligned = r() % 4 == 0;  // axis-aligned: a line along an axis is parallel to two pairs of faces
    const double a = aligned ? 0.0 : 3.14159 * u(r), b = aligned ? 0.0 : 3.14159 * u(r), c = aligned ? 0.0 : 3.14159 * u(r);
    const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
    const double R[3][3] = {{ca * cb, ca * sb * sc - sa * cc, ca * sb * cc + sa * sc},
                            {sa * cb, sa * sb * sc + ca * cc, sa * sb * cc - ca * sc},
                            {-sb, cb * sc, cb * cc}};
    for (int i = 0; i < 3; ++i) {
        v.scale[i] = 1.0 + 30.0 * std::fabs(u(r));
        v.pos[i] = around[i] + 60.0 * u(r);
        for (int j = 0; j < 3; ++j) v.rot[i][j] = R[i][j];
    }
    // world to local (+0x98, 3x4): the rows of S^-1 R^T and their translation
    for (int i = 0; i < 3; ++i) {
        double t = 0.0;
        for (int j = 0; j < 3; ++j) {
            Put(v.n, 0x98 + (i * 4 + j) * 4, float(R[j][i] / v.scale[i]));
            t += R[j][i] * v.pos[j];
        }
        Put(v.n, 0x98 + (i * 4 + 3) * 4, float(-t / v.scale[i]));
    }
    // the world box (+0x1A0: the cube's), sometimes moved away so that the ray misses it
    const double away = r() % 8 == 0 ? 500.0 : 0.0;
    for (int i = 0; i < 3; ++i) {
        double e = 0.0;
        for (int j = 0; j < 3; ++j) e += std::fabs(R[i][j]) * v.scale[j];
        Put(v.n, 0x1A0 + i * 4, float(v.pos[i] - e + away));
        Put(v.n, 0x1AC + i * 4, float(v.pos[i] + e + away));
    }
    const uint32_t hidden = r() % 10 == 0 ? 0x100u : 0u;  // sometimes hidden (0x100), with other flags
    Put(v.n, 0xC, hidden | uint32_t(r() & 0x6));
    Put(v.n, 0xC8, uint32_t(r() % 2));
    Put(v.n, 0x108, float(2.0 * std::fabs(u(r))));  // density
    // height falloff (+0x134 direction, +0x14C base): none (no integration step), mild, or steep (expf's argument
    // clamped at -80 and 80)
    const int fall = int(r() % 3);
    const double k = fall == 0 ? 0.0 : fall == 1 ? 0.05 : 3.0;
    for (int i = 0; i < 3; ++i) {
        Put(v.n, 0x134 + i * 4, float(k * u(r)));
        Put(v.n, 0x14C + i * 4, float(v.pos[i] + 20.0 * u(r)));
    }
    Put(v.n, 0x1B8, float(4.0 * u(r)));  // the soft edge: below 0, within [0, 1] and above
    Put(v.n, 0x1BC, float(u(r)));
    for (int i = 0; i < 3; ++i) Put(v.n, 0x1C0 + i * 4, float(std::fabs(u(r))));
    if (r() % 50 == 0) {  // a NaN in a field the math reads
        static const int kFields[] = {0x98, 0xA4, 0xB8, 0x108, 0x134, 0x14C, 0x1B8, 0x1BC};
        Put(v.n, kFields[r() % 8], std::numeric_limits<float>::quiet_NaN());
    }
}

// A position: the camera itself, along an axis from it, inside the volume, just beyond it, or anywhere near
void FogPosition(std::mt19937& r, const float* cam, const FogVolume& v, float* pos) {
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    const int kind = int(r() % 6);
    if (kind == 0) {
        for (int i = 0; i < 3; ++i) pos[i] = cam[i];
    } else if (kind == 1) {
        const int axis = int(r() % 3);
        for (int i = 0; i < 3; ++i) pos[i] = cam[i] + (i == axis ? float(200.0 * u(r)) : 0.0f);
    } else if (kind <= 3) {
        const double s = kind == 2 ? 0.8 : 1.5;
        const double l[3] = {s * u(r), s * u(r), s * u(r)};
        for (int i = 0; i < 3; ++i) {
            double w = v.pos[i];
            for (int j = 0; j < 3; ++j) w += v.rot[i][j] * v.scale[j] * l[j];
            pos[i] = float(w);
        }
    } else {
        for (int i = 0; i < 3; ++i) pos[i] = cam[i] + float(150.0 * u(r));
    }
}

bool AllOnes(const float* c) { return c[0] == 1.0f && c[1] == 1.0f && c[2] == 1.0f && c[3] == 1.0f; }

void TestFog(const std::wstring& dir) {
    namespace fog = optiperf::fog;
    printf("the fog volumes' math against Cry3DEngine.dll's own code\n");
    const std::wstring path = dir + L"\\Cry3DEngine.dll";
    HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!mod) {
        printf("  cannot map %ls (error %lu)\n", path.c_str(), GetLastError());
        ++g_failures;
        return;
    }
    char* base = reinterpret_cast<char*>(mod);
    char* trace = base + 0x9F630;  // TraceFogVolumes, where I3DEngine's slot 59 jumps (build 6156)
    fog::Target t;
    const char* why = "";
    const bool found = fog::Locate(base, trace, &t, &why);
    printf("  located: %s\n", found ? "yes" : why);
    Check(found, "fog::Locate on build 6156");
    if (!found) {
        FreeLibrary(mod);
        return;
    }
    auto rva = [base](const void* p) { return unsigned(static_cast<const char*>(p) - base); };
    const unsigned rays = rva(reinterpret_cast<const void*>(t.math.rayBox));
    const unsigned sq = rva(reinterpret_cast<const void*>(t.math.sqrt)), ex = rva(reinterpret_cast<const void*>(t.math.exp));
    printf("  ray-box +0x%X, ellipsoid +0x%X, box +0x%X, sqrtf +0x%X, expf +0x%X | cvars +0x%X, list +0x%X..+0x%X, "
           "camera box +0x%X, camera +0x%X +0x%X +0x%X\n",
           rays, rva(t.ellipsoid), rva(t.boxFog), sq, ex, rva(t.cvars), rva(t.begin), rva(t.end), rva(t.box),
           rva(t.cam[0]), rva(t.cam[1]), rva(t.cam[2]));
    Check(rays == 0x7DA0 && rva(t.ellipsoid) == 0x9DD80 && rva(t.boxFog) == 0x9E3C0 && sq == 0x1C7BA6 && ex == 0x1C7CB0,
          "the fog's code (build 6156: +0x7DA0, +0x9DD80, +0x9E3C0, thunks +0x1C7BA6, +0x1C7CB0)");
    Check(rva(t.cvars) == 0x22B8E0 && rva(t.begin) == 0x213BB0 && rva(t.end) == 0x213BB8 && rva(t.box) == 0x213B98 &&
              rva(t.cam[0]) == 0x2008FC && rva(t.cam[1]) == 0x20090C && rva(t.cam[2]) == 0x20091C,
          "the fog's data (build 6156)");
    // To run the whole of TraceFogVolumes: the imports; its profiler (the static it sets up once: done; gEnv's
    // bProfilerEnabled, +0xD3: off); the 3D engine's cvars, fog and fog volumes on
    const bool imports = PatchImport(reinterpret_cast<char*>(t.math.sqrt), reinterpret_cast<const void*>(&FogSqrt)) &&
                         PatchImport(reinterpret_cast<char*>(t.math.exp), reinterpret_cast<const void*>(&FogExp));
    Check(imports, "patching the sqrtf and expf imports");
    const bool prologue = std::memcmp(trace + 0x25, "\x8B\x05", 2) == 0 && std::memcmp(trace + 0x112, "\x48\x8B\x05", 3) == 0;
    char* guard = trace + 0x2B + int32_t(optiperf::ReadU32(trace + 0x27));
    char* genv = trace + 0x119 + int32_t(optiperf::ReadU32(trace + 0x115));
    Check(prologue && rva(guard) == 0x213F98 && rva(genv) == 0x244AB0, "TraceFogVolumes' profiler (build 6156)");
    if (!imports || !prologue) {
        FreeLibrary(mod);
        return;
    }
    alignas(16) static uint8_t env[0x100] = {};
    alignas(16) static uint8_t cvars[0x40] = {};
    *reinterpret_cast<uint32_t*>(guard) |= 1;
    *reinterpret_cast<void**>(genv) = env;
    *const_cast<char**>(t.cvars) = reinterpret_cast<char*>(cvars);
    Put(cvars, fog::kCvarFog, 1);
    Put(cvars, fog::kCvarFogVolumes, 1);
    float* liveCam[3] = {const_cast<float*>(t.cam[0]), const_cast<float*>(t.cam[1]), const_cast<float*>(t.cam[2])};

    // one fog volume's contribution: the game's (node, out, pos; the camera from its globals) and the transcription's
    using GameFogFn = float* (*)(const uint8_t* node, float* out, const float* pos);
    const auto gameEllipsoid = reinterpret_cast<GameFogFn>(t.ellipsoid);
    const auto gameBox = reinterpret_cast<GameFogFn>(t.boxFog);
    std::mt19937 r(20261018);
    std::uniform_real_distribution<float> where(0.0f, 4096.0f);
    FogVolume v;
    int differ = 0, missed[2] = {}, hit[2] = {};
    constexpr int kCases = 200000;
    for (int i = 0; i < kCases; ++i) {
        const float cam[3] = {where(r), where(r), where(r) * 0.05f};
        for (int k = 0; k < 3; ++k) *liveCam[k] = cam[k];
        MakeFogVolume(r, cam, v);
        float pos[3];
        FogPosition(r, cam, v, pos);
        uint32_t type;
        std::memcpy(&type, v.n + 0xC8, 4);
        float game[4], mine[4];
        (type ? gameBox : gameEllipsoid)(v.n, game, pos);
        (type ? fog::Box : fog::Ellipsoid)(v.n, cam, pos, mine, t.math);
        if (std::memcmp(game, mine, sizeof game) != 0) {
            if (++differ <= 5)
                printf("  DIFFERS: %s, camera (%.9g %.9g %.9g), position (%.9g %.9g %.9g): game (%.9g %.9g %.9g %.9g), "
                       "transcription (%.9g %.9g %.9g %.9g)\n",
                       type ? "box" : "ellipsoid", cam[0], cam[1], cam[2], pos[0], pos[1], pos[2], game[0], game[1],
                       game[2], game[3], mine[0], mine[1], mine[2], mine[3]);
            continue;
        }
        ++(AllOnes(game) ? missed : hit)[type ? 1 : 0];
    }
    printf("  one fog volume, %d cases: %d differ | ellipsoid %d through, %d missed or none | box %d through, %d missed "
           "or none\n",
           kCases, differ, hit[0], missed[0], hit[1], missed[1]);
    Check(differ == 0 && hit[0] > kCases / 10 && hit[1] > kCases / 10 && missed[0] > 0 && missed[1] > 0,
          "the contributions, bit for bit (and both kinds of outcome)");

    // the whole of TraceFogVolumes: up to 8 fog volumes in the list, the camera box around the camera
    using GameTraceFn = void (*)(const float* pos, float* out);
    const auto gameTrace = reinterpret_cast<GameTraceFn>(trace);
    std::vector<FogVolume> volumes(8);
    std::vector<uint8_t> entries(8 * fog::kEntryBytes);
    int traceDiffer = 0, none = 0, faint = 0, fogged = 0;
    constexpr int kScenes = 50000;
    for (int i = 0; i < kScenes; ++i) {
        const float cam[3] = {where(r), where(r), where(r) * 0.05f};
        for (int k = 0; k < 3; ++k) *liveCam[k] = cam[k];
        const float shift = r() % 4 == 0 ? 37.5f : 0.0f;  // the box set at another camera position
        const float box[6] = {cam[0] - 1024.0f + shift, cam[1] - 1024.0f, cam[2] - 1024.0f,
                              cam[0] + 1024.0f + shift, cam[1] + 1024.0f, cam[2] + 1024.0f};
        std::memcpy(const_cast<float*>(t.box), box, sizeof box);
        const size_t count = r() % 9;
        fog::Snapshot s;
        std::memcpy(s.box, box, sizeof box);
        std::memcpy(s.cam, cam, sizeof cam);
        s.count = count;
        s.nodes.resize(count * fog::kNodeBytes);
        std::memset(entries.data(), 0, entries.size());
        for (size_t k = 0; k < count; ++k) {
            MakeFogVolume(r, cam, volumes[k]);
            const uint8_t* node = volumes[k].n;
            std::memcpy(entries.data() + k * fog::kEntryBytes, &node, sizeof node);
            std::memcpy(s.nodes.data() + k * fog::kNodeBytes, node, fog::kNodeBytes);
        }
        *const_cast<char**>(t.begin) = reinterpret_cast<char*>(entries.data());
        *const_cast<char**>(t.end) = reinterpret_cast<char*>(entries.data() + count * fog::kEntryBytes);
        float pos[3];
        FogPosition(r, cam, count ? volumes[r() % count] : v, pos);
        float game[4], mine[4];
        gameTrace(pos, game);
        fog::Trace(s, cam, pos, mine, t.math);
        if (std::memcmp(game, mine, sizeof game) != 0) {
            if (++traceDiffer <= 5)
                printf("  DIFFERS: %zu fog volumes, camera (%.9g %.9g %.9g), position (%.9g %.9g %.9g): game (%.9g %.9g "
                       "%.9g %.9g), transcription (%.9g %.9g %.9g %.9g)\n",
                       count, cam[0], cam[1], cam[2], pos[0], pos[1], pos[2], game[0], game[1], game[2], game[3],
                       mine[0], mine[1], mine[2], mine[3]);
            continue;
        }
        if (game[3] == 1.0f && game[0] == 0.0f && game[1] == 0.0f && game[2] == 0.0f)
            ++none;
        else if (game[0] == 0.0f && game[1] == 0.0f && game[2] == 0.0f)
            ++faint;
        else
            ++fogged;
    }
    printf("  TraceFogVolumes, %d scenes: %d differ | no fog %d, too little to colour %d, fogged %d\n", kScenes,
           traceDiffer, none, faint, fogged);
    Check(traceDiffer == 0 && fogged > kScenes / 10 && none > 0, "TraceFogVolumes, bit for bit");
    // a fog cvar off: none of their colour, all of the light through
    Put(cvars, fog::kCvarFogVolumes, 0);
    float off[4] = {5.0f, 5.0f, 5.0f, 5.0f};
    const float at[3] = {1.0f, 2.0f, 3.0f};
    gameTrace(at, off);
    Put(cvars, fog::kCvarFogVolumes, 1);
    const bool offOk = off[0] == 0.0f && off[1] == 0.0f && off[2] == 0.0f && off[3] == 1.0f;
    printf("  fog volumes off: (%g %g %g %g)%s\n", off[0], off[1], off[2], off[3], offOk ? "" : "   WRONG");
    Check(offOk, "TraceFogVolumes with e_FogVolumes off");

    // the ellipsoid's density field altered (+0x108 -> +0x10C): refused
    const auto density = optiperf::FindInRange(t.ellipsoid, 1595, optiperf::ParsePattern("F3 44 0F 59 AF 08 01 00 00"), 2);
    if (density.size() == 1) {
        char* probe = density[0] + 5;
        DWORD old;
        VirtualProtect(probe, 1, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<uint8_t*>(probe) = 0x0C;
        fog::Target t2;
        const char* why2 = "";
        const bool refused = !fog::Locate(base, trace, &t2, &why2);
        *reinterpret_cast<uint8_t*>(probe) = 0x08;
        VirtualProtect(probe, 1, old, &old);
        printf("  altered ellipsoid: %s%s\n", refused ? why2 : "accepted", refused ? "" : "   WRONG");
        Check(refused, "fog::Locate refusing an altered ellipsoid");
    } else {
        printf("  the ellipsoid's density field not found where expected\n");
        ++g_failures;
    }
    FreeLibrary(mod);
}

// --- the ocean ----------------------------------------------------------------------------------------------------------
// GetWave asks its renderer for the water shaders' quality and its features, its render element for the grid, and its
// analytic waves the timer for the time: fakes of those three.
namespace fakeocean {
int quality = 3;
uint32_t features = 0x80000000u;
const float* grid = nullptr;
int Quality(void*, int which) { return which == 7 ? quality : -1; }
uint32_t Features(void*) { return features; }
const float* Grid(void*) { return grid; }
float Time(void*, int) { return 1234.5f; }
void* rendererVt[0x320 / 8] = {};
void* elementVt[0xC8 / 8] = {};
void* timerVt[4] = {};
void* renderer[1] = {rendererVt};
void* element[1] = {elementVt};
void* timer[1] = {timerVt};
}  // namespace fakeocean

float OceanFloor(float x) { return std::floor(x); }
float OceanSin(float x) { return std::sin(x); }
float OceanCos(float x) { return std::cos(x); }
bool SameBits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

void TestOcean(const std::wstring& dir) {
    namespace ocean = optiperf::ocean;
    printf("the ocean's height against Cry3DEngine.dll's own code\n");
    const std::wstring path = dir + L"\\Cry3DEngine.dll";
    HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!mod) {
        printf("  cannot map %ls (error %lu)\n", path.c_str(), GetLastError());
        ++g_failures;
        return;
    }
    char* base = reinterpret_cast<char*>(mod);
    char* fn = base + 0xF39D0;  // GetOceanWaterLevel, I3DEngine's slot 28 (build 6156)
    ocean::Target t;
    const char* why = "";
    const bool found = ocean::Locate(base, fn, &t, &why);
    printf("  located: %s\n", found ? "yes" : why);
    Check(found, "ocean::Locate on build 6156");
    if (!found) {
        FreeLibrary(mod);
        return;
    }
    auto rva = [base](const void* p) { return unsigned(static_cast<const char*>(p) - base); };
    printf("  GetWave +0x%X, floorf +0x%X | renderer +0x%X, timer +0x%X, 3D engine +0x%X, cvars +0x%X, render element "
           "+0x%X, water level +0x%X\n",
           rva(t.wave), rva(reinterpret_cast<const void*>(t.floor)), rva(t.renderer), rva(t.timer), rva(t.engine),
           rva(t.cvars), rva(t.element), rva(t.level));
    Check(rva(t.wave) == 0xC3F40 && rva(reinterpret_cast<const void*>(t.floor)) == 0x1C7CDA,
          "the ocean's code (build 6156: +0xC3F40, thunk +0x1C7CDA)");
    Check(rva(t.renderer) == 0x22B8A0 && rva(t.timer) == 0x227D70 && rva(t.engine) == 0x22B8D8 &&
              rva(t.cvars) == 0x22B8E0 && rva(t.element) == 0x227D78 && rva(t.level) == 0x22BA0C,
          "the ocean's data (build 6156)");
    // the imports the waves call (the analytic ones too); GetOceanWaterLevel's profiler (its static set up, gEnv's
    // bProfilerEnabled off), and the terrain it takes the water level from when the 3D engine's frame changes
    const bool imports =
        PatchImport(reinterpret_cast<char*>(t.floor), reinterpret_cast<const void*>(&OceanFloor)) &&
        PatchImport(base + 0x1C7BA6, reinterpret_cast<const void*>(&FogSqrt)) &&
        PatchImport(base + 0x1C7C98, reinterpret_cast<const void*>(&OceanSin)) &&
        PatchImport(base + 0x1C7C9E, reinterpret_cast<const void*>(&OceanCos));
    Check(imports, "patching the floorf, sqrtf, sinf and cosf imports");
    auto rip = [](char* field) { return field + 4 + int32_t(optiperf::ReadU32(field)); };
    char* cached = fn + 0x11F;  // (ocean_snapshot.cpp's kLevelCode)
    const bool prologue = std::memcmp(fn + 0xF, "\x8B\x05", 2) == 0 && std::memcmp(fn + 0xF8, "\x48\x8B\x05", 3) == 0 &&
                          std::memcmp(cached, "\x8B\x05", 2) == 0 && std::memcmp(cached + 0xE, "\x48\x8B\x0D", 3) == 0;
    char* guard = rip(fn + 0x11);
    char* genv = rip(fn + 0xFB);
    char* frame = rip(cached + 2);
    char* terrainAt = rip(cached + 0x11);
    Check(prologue && rva(guard) == 0x22BDA8 && rva(genv) == 0x244AB0 && rva(frame) == 0x22B930 &&
              rva(terrainAt) == 0x22B8C0,
          "GetOceanWaterLevel's profiler, frame and terrain (build 6156)");
    if (!imports || !prologue) {
        FreeLibrary(mod);
        return;
    }
    fakeocean::rendererVt[ocean::kVfShaderQuality / 8] = reinterpret_cast<void*>(&fakeocean::Quality);
    fakeocean::rendererVt[ocean::kVfFeatures / 8] = reinterpret_cast<void*>(&fakeocean::Features);
    fakeocean::elementVt[ocean::kVfDisplaceGrid / 8] = reinterpret_cast<void*>(&fakeocean::Grid);
    fakeocean::timerVt[2] = reinterpret_cast<void*>(&fakeocean::Time);
    alignas(16) static uint8_t env[0x100] = {}, engine[0x200] = {}, cvars[0x100] = {}, terrain[0x200] = {};
    *reinterpret_cast<uint32_t*>(guard) |= 1;
    *reinterpret_cast<void**>(genv) = env;
    *reinterpret_cast<void**>(terrainAt) = terrain;
    *const_cast<void**>(t.renderer) = fakeocean::renderer;
    *const_cast<void**>(t.timer) = fakeocean::timer;
    *const_cast<char**>(t.engine) = reinterpret_cast<char*>(engine);
    *const_cast<char**>(t.cvars) = reinterpret_cast<char*>(cvars);
    *const_cast<void**>(t.element) = fakeocean::element;
    Put(cvars, ocean::kCvarFft, int32_t(1));

    // the FFT branch: random grids, scales, sizes and positions (some far beyond the grid's int range)
    using WaveFn = float (*)(const float* pos);
    using LevelFn = float (*)(void* self, const float* pos);
    const auto gameWave = reinterpret_cast<WaveFn>(t.wave);
    const auto gameLevel = reinterpret_cast<LevelFn>(fn);
    std::mt19937 r(20261019);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::vector<float> grid(size_t(ocean::kGrid) * ocean::kGrid * 4);
    fakeocean::grid = grid.data();
    auto randomise = [&](int i) {
        const float scale = i % 7 == 0 ? unit(r) * 8.0f : 1.75f + unit(r) * 1.5f;
        const float size = i % 11 == 0 ? unit(r) * 20.0f : 2.5f + unit(r) * 2.0f;
        Put(engine, ocean::kEngineScale, scale);
        Put(engine, ocean::kEngineSize, size);
    };
    auto position = [&](int i, float* pos) {
        pos[0] = unit(r) * 8192.0f;
        pos[1] = unit(r) * 8192.0f;
        pos[2] = unit(r) * 200.0f;
        if (i % 13 == 0) {
            pos[0] *= 1.0e6f;
            pos[1] *= 1.0e5f;
        }
    };
    int waveDiffer = 0;
    constexpr int kCases = 200000;
    for (int i = 0; i < kCases; ++i) {
        if (i % 1000 == 0)
            for (float& h : grid) h = unit(r) * 3.0f;
        randomise(i);
        float pos[3];
        position(i, pos);
        float scale, size;
        std::memcpy(&scale, engine + ocean::kEngineScale, 4);
        std::memcpy(&size, engine + ocean::kEngineSize, 4);
        const float game = gameWave(pos);
        const float mine = ocean::FftWave(grid.data(), pos, scale, size, t.floor);
        if (!SameBits(game, mine) && ++waveDiffer <= 5)
            printf("  DIFFERS: position (%.9g %.9g), scale %.9g, size %.9g: game %.9g, transcription %.9g\n", pos[0],
                   pos[1], scale, size, game, mine);
    }
    printf("  GetWave's FFT branch, %d cases: %d differ\n", kCases, waveDiffer);
    Check(waveDiffer == 0, "the FFT waves, bit for bit");

    // the whole of GetOceanWaterLevel as the capture takes its inputs: the terrain's water level, taken when the 3D
    // engine's frame changes, and the waves (a tenth of the cases without the render element, a tenth without a grid)
    int levelDiffer = 0, notFft = 0;
    constexpr int kLevels = 20000;
    for (int i = 0; i < kLevels; ++i) {
        Put(terrain, 0x1E0, 50.0f + unit(r) * 60.0f);
        ++*reinterpret_cast<int32_t*>(frame);
        randomise(i);
        *const_cast<void**>(t.element) = i % 10 == 0 ? nullptr : fakeocean::element;
        fakeocean::grid = i % 10 == 1 ? nullptr : grid.data();
        float pos[3];
        position(i, pos);
        const float game = gameLevel(nullptr, pos);
        const ocean::Inputs in = ocean::Capture(t);
        if (in.branch != ocean::Branch::Fft) {
            ++notFft;
            continue;
        }
        const float mine = ocean::Height(in, pos, t.floor);
        if (!SameBits(game, mine) && ++levelDiffer <= 5)
            printf("  DIFFERS: GetOceanWaterLevel at (%.9g %.9g): game %.9g, from the capture %.9g\n", pos[0], pos[1],
                   game, mine);
    }
    *const_cast<void**>(t.element) = fakeocean::element;
    fakeocean::grid = grid.data();
    printf("  GetOceanWaterLevel, %d cases: %d differ, %d not on the FFT branch\n", kLevels, levelDiffer, notFft);
    Check(levelDiffer == 0 && notFft == 0, "GetOceanWaterLevel from the capture's inputs, bit for bit");

    // the branches: GetWave's own answer tells the FFT one (a grid of 1000s: 60 at a size of 1) from the analytic
    // waves and from none (0)
    for (float& h : grid) h = 1000.0f;
    Put(engine, ocean::kEngineScale, 1.0f);
    Put(engine, ocean::kEngineSize, 1.0f);
    Put(engine, 0x158, 0.5f);  // the analytic waves' wind direction, speed and amount
    Put(engine, 0x15C, 1.0f);
    Put(engine, 0x160, 1.0f);
    struct Case {
        const char* what;
        int quality;
        uint32_t features;
        int32_t fft;
        bool timer;
        ocean::Branch branch;
    };
    const Case cases[] = {
        {"water shaders off", 0, 0x80000000u, 1, true, ocean::Branch::None},
        {"no timer", 3, 0x80000000u, 1, false, ocean::Branch::None},
        {"no vertex textures", 3, 0x7FFFFFFFu, 1, true, ocean::Branch::Analytic},
        {"the FFT cvar off", 3, 0x80000000u, 0, true, ocean::Branch::Analytic},
        {"low quality water", 1, 0x80000000u, 1, true, ocean::Branch::Analytic},
        {"medium quality water", 2, 0x80000000u, 1, true, ocean::Branch::Fft},
        {"very high quality water", 3, 0xFFFFFFFFu, 1, true, ocean::Branch::Fft},
    };
    const float at[3] = {100.0f, 200.0f, 0.0f};
    const float fftHeight = ocean::FftWave(grid.data(), at, 1.0f, 1.0f, t.floor);
    const char* names[] = {"none", "analytic", "FFT"};
    for (const Case& c : cases) {
        fakeocean::quality = c.quality;
        fakeocean::features = c.features;
        Put(cvars, ocean::kCvarFft, c.fft);
        *const_cast<void**>(t.timer) = c.timer ? fakeocean::timer : nullptr;
        ++*reinterpret_cast<int32_t*>(frame);
        const float game = gameWave(at);
        const ocean::Branch mine = ocean::Capture(t).branch;
        const ocean::Branch seen = game == 0.0f       ? ocean::Branch::None
                                   : game == fftHeight ? ocean::Branch::Fft
                                                       : ocean::Branch::Analytic;
        const bool ok = mine == c.branch && seen == c.branch;
        printf("  %-24s GetWave %.9g (%s), the capture's branch %s%s\n", c.what, game, names[int(seen)],
               names[int(mine)], ok ? "" : "   WRONG");
        Check(ok, "the capture's branch, as GetWave takes it");
    }
    fakeocean::quality = 3;
    fakeocean::features = 0x80000000u;
    Put(cvars, ocean::kCvarFft, int32_t(1));
    *const_cast<void**>(t.timer) = fakeocean::timer;

    // the waves' size field altered (+0x168 -> +0x16C): refused
    char* probe = t.wave + 0x3CB;  // mulss xmm6, [r12+0x168]
    if (uint8_t(*probe) != 0x68) {
        printf("  the waves' size field not found where expected\n");
        ++g_failures;
    } else {
        DWORD old;
        VirtualProtect(probe, 1, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<uint8_t*>(probe) = 0x6C;
        ocean::Target t2;
        const char* why2 = "";
        const bool refused = !ocean::Locate(base, fn, &t2, &why2);
        *reinterpret_cast<uint8_t*>(probe) = 0x68;
        VirtualProtect(probe, 1, old, &old);
        printf("  altered waves: %s%s\n", refused ? why2 : "accepted", refused ? "" : "   WRONG");
        Check(refused, "ocean::Locate refusing altered waves");
    }
    FreeLibrary(mod);
}

// --- materials ----------------------------------------------------------------------------------------------------------
// Synthetic CMatInfo and CMaterialLayer objects, laid out as material_record.h describes them.
struct FakeMats {
    std::vector<std::vector<uint8_t>> layers, mats, subArrays, vectors, layerArrays;
    std::vector<char*> layerPtrs, matPtrs;
};

void MakeFakeMats(std::mt19937& r, FakeMats& f, int count) {
    namespace mt = optiperf::mt;
    for (int i = 0; i < 40; ++i) {
        f.layers.emplace_back(0x30);
        for (uint8_t& b : f.layers.back()) b = uint8_t(r());
        f.layerPtrs.push_back(reinterpret_cast<char*>(f.layers.back().data()));
    }
    f.mats.reserve(size_t(count));
    for (int i = 0; i < count; ++i) {
        f.mats.emplace_back(0x90);
        std::vector<uint8_t>& m = f.mats.back();
        for (uint8_t& b : m) b = uint8_t(r());
        f.matPtrs.push_back(reinterpret_cast<char*>(m.data()));
    }
    for (int i = 0; i < count; ++i) {
        uint8_t* m = f.mats[size_t(i)].data();
        uint32_t flags = uint32_t(r());
        if (r() % 2) flags |= mt::kMultiSub; else flags &= ~mt::kMultiSub;
        std::memcpy(m + mt::kMatFlags, &flags, 4);
        // sub-materials: a DynArray, its count in the 4 bytes before the elements
        const int32_t subs = int32_t(r() % 5);
        f.subArrays.emplace_back(size_t(8 + 8 * subs));
        uint8_t* arr = f.subArrays.back().data();
        std::memcpy(arr + 4, &subs, 4);
        for (int k = 0; k < subs; ++k) {
            char* s = r() % 6 ? f.matPtrs[r() % f.matPtrs.size()] : nullptr;
            std::memcpy(arr + 8 + 8 * k, &s, 8);
        }
        char* elems = reinterpret_cast<char*>(arr + 8);
        std::memcpy(m + mt::kMatSubs, &elems, 8);
        // layers: no vector, an empty one, or up to 12 slots (some empty)
        char* vec = nullptr;
        if (r() % 5) {
            const int n = r() % 4 ? int(r() % 5) : int(r() % 13);
            f.layerArrays.emplace_back(size_t(8 * (n ? n : 1)));
            uint8_t* la = f.layerArrays.back().data();
            for (int k = 0; k < n; ++k) {
                char* l = r() % 5 ? f.layerPtrs[r() % f.layerPtrs.size()] : nullptr;
                std::memcpy(la + 8 * k, &l, 8);
            }
            f.vectors.emplace_back(24);
            uint8_t* v = f.vectors.back().data();
            char* begin = n ? reinterpret_cast<char*>(la) : nullptr;
            char* end = begin ? begin + 8 * n : nullptr;
            std::memcpy(v, &begin, 8);
            std::memcpy(v + 8, &end, 8);
            std::memcpy(v + 16, &end, 8);
            vec = reinterpret_cast<char*>(v);
        }
        std::memcpy(m + mt::kMatLayers, &vec, 8);
    }
}

void TestMaterials(const std::wstring& dir) {
    namespace mt = optiperf::mt;
    printf("the material snapshot: the layer passes' calls in CryRenderD3D10.dll, the getters in Cry3DEngine.dll\n");
    HMODULE render = LoadLibraryExW((dir + L"\\CryRenderD3D10.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    HMODULE e3d = LoadLibraryExW((dir + L"\\Cry3DEngine.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!render || !e3d) {
        printf("  cannot map the renderer or the 3D engine (error %lu)\n", GetLastError());
        ++g_failures;
        if (render) FreeLibrary(render);
        if (e3d) FreeLibrary(e3d);
        return;
    }
    char* rb = reinterpret_cast<char*>(render);
    char* eb = reinterpret_cast<char*>(e3d);
    optiperf::MaterialTarget t;
    optiperf::MaterialEngine e;
    const char* why = "";
    const bool rOk = optiperf::MaterialSnapshot_LocateRenderer(rb, &t, &why);
    if (!rOk) printf("  renderer: %s\n", why);
    const bool eOk = optiperf::MaterialSnapshot_LocateEngine(eb, &e, &why);
    if (!eOk) printf("  3D engine: %s\n", why);
    Check(rOk && eOk, "locating the material calls and getters");
    if (!rOk || !eOk) {
        FreeLibrary(render);
        FreeLibrary(e3d);
        return;
    }
    auto rr = [rb](const void* p) { return p ? unsigned(static_cast<const char*>(p) - rb) : 0u; };
    auto er = [eb](const void* p) { return p ? unsigned(static_cast<const char*>(p) - eb) : 0u; };
    printf("  SetupMultiLayers +0x%X, DrawShader_MultiLayers +0x%X, default layers static +0x%X\n", rr(t.setupLayers),
           rr(t.drawLayers), rr(t.defaultLayers));
    Check(rr(t.setupLayers) == 0x20350 && rr(t.drawLayers) == 0x2CEF0 && rr(t.defaultLayers) == 0x2D43D8,
          "the layer passes (build 6156)");
    // build 6156: the getters, and the return addresses of the two functions' calls of each
    const unsigned fns[optiperf::kMtlSlots] = {0x1520E0, 0x152100, 0x151460, 0x151D10, 0x150DF0,
                                               0x1515E0, 0x151520, 0x150C20, 0x150C30, 0xD1FF0};
    const unsigned sites[optiperf::kMtlSlots][2] = {{0x20602, 0x2CFF9}, {0x2D30F, 0},       {0x205CD, 0x2CFC2},
                                                    {0x205E0, 0x2CFD6}, {0x2D010, 0},       {0x2D19E, 0x2D1B1},
                                                    {0x2062D, 0x20643}, {0x20663, 0x2D1DB}, {0x20674, 0x2D1EC},
                                                    {0x2CFAD, 0}};
    static const char* const kNames[optiperf::kMtlSlots] = {"GetFlags", "GetShaderItem", "GetSubMtlCount", "GetSubMtl",
                                                            "GetLayerCount", "GetLayer(slot)", "GetLayer(mask)",
                                                            "IsEnabled", "Layer::GetShaderItem", "GetDefaultLayers"};
    int wrongPlaces = 0;
    for (int i = 0; i < optiperf::kMtlSlots; ++i) {
        const bool ok = er(e.fns[i]) == fns[i] && rr(t.sites[i][0]) == sites[i][0] && rr(t.sites[i][1]) == sites[i][1];
        // each return address follows an indirect call: FF /2 with a disp32 or a disp8, maybe with a REX prefix
        for (int k = 0; k < 2; ++k) {
            const auto* s = reinterpret_cast<const uint8_t*>(t.sites[i][k]);
            if (!s) continue;
            const bool call = (s[-6] == 0xFF && (s[-5] & 0xF8) == 0x90) || (s[-3] == 0xFF && (s[-2] & 0xF8) == 0x50);
            if (!call) ++wrongPlaces;
        }
        printf("  %-21s +0x%06X, called from +0x%X, +0x%X%s\n", kNames[i], er(e.fns[i]), rr(t.sites[i][0]),
               rr(t.sites[i][1]), ok ? "" : "   EXPECTED other places");
        if (!ok) ++wrongPlaces;
    }
    printf("  vtables: CMatInfo +0x%X, CMaterialLayer +0x%X, CMatMan +0x%X; the material manager's static +0x%X\n",
           er(e.vtables[0]), er(e.vtables[1]), er(e.vtables[2]), er(e.matMan));
    Check(wrongPlaces == 0 && er(e.vtables[0]) == 0x1DFAE8 && er(e.vtables[1]) == 0x1DFA48 &&
              er(e.vtables[2]) == 0x1E01C8 && er(e.matMan) == 0x22B928,
          "the getters and their calls (build 6156)");

    // the transcriptions against the getters themselves, on synthetic materials
    using U32Fn = uint32_t (*)(char*);
    using I32Fn = int32_t (*)(char*);
    using PtrFn = char* (*)(char*);
    using SubFn = char* (*)(char*, int32_t);
    using U64Fn = uint64_t (*)(char*);
    using AtFn = char* (*)(char*, uint32_t);
    using MaskFn = char* (*)(char*, uint8_t, uint8_t);
    std::mt19937 r(181018);
    FakeMats f;
    MakeFakeMats(r, f, 400);
    mt::Snapshot snap;
    for (char* m : f.matPtrs) snap.AddMaterial(m, true);
    int calls = 0, differ = 0, notes = 0, found = 0;
    auto check = [&](bool same, const char* what, const void* m) {
        ++calls;
        if (same) return;
        if (++differ <= 5) printf("  DIFFERS: %s on %p\n", what, m);
    };
    for (char* m : f.matPtrs) {
        const mt::Mat* c = snap.FindMat(m);
        if (!c) {
            check(false, "no copy", m);
            continue;
        }
        check(reinterpret_cast<U32Fn>(e.fns[optiperf::kMtlFlags])(m) == mt::Flags(*c), "GetFlags", m);
        check(reinterpret_cast<PtrFn>(e.fns[optiperf::kMtlItem])(m) == m + mt::kMatItem, "GetShaderItem", m);
        check(reinterpret_cast<I32Fn>(e.fns[optiperf::kMtlSubCount])(m) == mt::SubMtlCount(*c), "GetSubMtlCount", m);
        for (int32_t i = -2; i < c->subCount + 2; ++i)
            check(reinterpret_cast<SubFn>(e.fns[optiperf::kMtlSub])(m, i) == mt::SubMtl(snap, *c, i), "GetSubMtl", m);
        check(reinterpret_cast<U64Fn>(e.fns[optiperf::kMtlLayerCount])(m) == mt::LayerCount(*c), "GetLayerCount", m);
        for (uint32_t slot : {0u, 1u, 2u, 3u, 5u, 8u, 12u, 13u, 40u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu})
            check(reinterpret_cast<AtFn>(e.fns[optiperf::kMtlLayerAt])(m, slot) == mt::LayerAt(snap, *c, slot),
                  "GetLayer(slot)", m);
        // GetLayer(mask, usage): the answer, and what it leaves in the material (the game's on the material itself,
        // the transcription's on a copy of it)
        for (int k = 0; k < 12; ++k) {
            const uint8_t mask = k == 0 ? 0 : k == 1 ? 1 : uint8_t(r()), usage = uint8_t(r());
            std::vector<uint8_t> twin(m, m + 0x90);
            char* live = reinterpret_cast<MaskFn>(e.fns[optiperf::kMtlLayerMask])(m, mask, usage);
            const char* mine;
            const bool hit = mt::LayerByMask(snap, *c, mask, &mine);
            if (hit) mt::Note(reinterpret_cast<char*>(twin.data()), mask, usage, mine);
            check(live == mine, "GetLayer(mask)", m);
            check(std::memcmp(m, twin.data(), 0x90) == 0, "GetLayer(mask)'s note", m);
            found += hit ? 1 : 0;
            ++notes;
        }
    }
    for (char* l : f.layerPtrs) {
        mt::Layer* c = snap.FindLayer(l);
        if (!c) continue;  // (a layer no material holds)
        check((reinterpret_cast<U32Fn>(e.fns[optiperf::kMtlEnabled])(l) & 0xFF) == mt::Enabled(*c), "IsEnabled", l);
        char* item = reinterpret_cast<PtrFn>(e.fns[optiperf::kMtlLayerItem])(l);
        check(item == l + mt::kLayerItem && std::memcmp(item, c->item, mt::kItemBytes) == 0, "Layer::GetShaderItem", l);
    }
    std::vector<uint8_t> man(0x60, 0);
    char* someMat = f.matPtrs[3];
    std::memcpy(man.data() + 0x50, &someMat, 8);
    check(reinterpret_cast<PtrFn>(e.fns[optiperf::kMtlDefaultLayers])(reinterpret_cast<char*>(man.data())) == someMat,
          "GetDefaultLayersMaterial", man.data());
    printf("  %zu materials, %zu layers copied; %d calls compared, %d differ (GetLayer by mask: %d of %d found a slot)\n",
           snap.mats.size(), snap.layers.size(), calls, differ, found, notes);
    Check(differ == 0 && found > 0 && found < notes, "the getters' transcriptions, against the getters");

    // one changed byte: GetSubMtl's flag test (bit 8 -> 9), and SetupMultiLayers' layer byte (+0x87 -> +0x86)
    char* probe = e.fns[optiperf::kMtlSub] + 0x11;
    char* probe2 = t.setupLayers + 0x254 + 0x73;  // cmp byte ptr [r12 + 0x87], dil
    if (uint8_t(*probe) == 0x08 && uint8_t(*probe2) == 0x87) {
        DWORD old, old2;
        VirtualProtect(probe, 1, PAGE_EXECUTE_READWRITE, &old);
        VirtualProtect(probe2, 1, PAGE_EXECUTE_READWRITE, &old2);
        *probe = 0x09;
        *reinterpret_cast<uint8_t*>(probe2) = 0x86;
        optiperf::MaterialTarget t2;
        optiperf::MaterialEngine e2;
        const char* w1 = "accepted";
        const char* w2 = "accepted";
        const bool engineRefused = !optiperf::MaterialSnapshot_LocateEngine(eb, &e2, &w1);
        const bool rendererRefused = !optiperf::MaterialSnapshot_LocateRenderer(rb, &t2, &w2);
        *probe = 0x08;
        *reinterpret_cast<uint8_t*>(probe2) = 0x87;
        VirtualProtect(probe, 1, old, &old);
        VirtualProtect(probe2, 1, old2, &old2);
        printf("  altered GetSubMtl: %s; altered SetupMultiLayers: %s\n", w1, w2);
        Check(engineRefused && rendererRefused, "refusing altered code");
    } else {
        printf("  probe bytes not found where expected\n");
        ++g_failures;
    }
    FreeLibrary(render);
    FreeLibrary(e3d);
}

// --- cvars ------------------------------------------------------------------------------------------------------------
void TestCvars(const std::wstring& dir) {
    printf("the cvar snapshot's classes in CrySystem.dll\n");
    const std::wstring path = dir + L"\\CrySystem.dll";
    HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!mod) {
        printf("  cannot map %ls (error %lu)\n", path.c_str(), GetLastError());
        ++g_failures;
        return;
    }
    char* base = reinterpret_cast<char*>(mod);
    optiperf::CvarTarget t;
    const char* why = "";
    const bool found = optiperf::CvarSnapshot_Locate(base, &t, &why);
    if (!found) printf("  %s\n", why);
    Check(found, "locating the cvar classes");
    if (!found) {
        FreeLibrary(mod);
        return;
    }
    auto rva = [base](const void* p) { return unsigned(static_cast<const char*>(p) - base); };
    printf("  CXConsoleVariableIntRef +0x%X: GetIVal +0x%X, GetFVal +0x%X, GetString +0x%X\n", rva(t.vtables[0]),
           rva(t.getters[0][0]), rva(t.getters[0][1]), rva(t.getters[0][2]));
    printf("  CXConsoleVariableString +0x%X: GetIVal +0x%X, GetFVal +0x%X, GetString +0x%X\n", rva(t.vtables[1]),
           rva(t.getters[1][0]), rva(t.getters[1][1]), rva(t.getters[1][2]));
    Check(rva(t.vtables[0]) == 0x273618 && rva(t.vtables[1]) == 0x273438 && rva(t.getters[0][0]) == 0x60BD0 &&
              rva(t.getters[0][1]) == 0x60BE0 && rva(t.getters[0][2]) == 0x60BF0 && rva(t.getters[1][0]) == 0x60320 &&
              rva(t.getters[1][1]) == 0x60330 && rva(t.getters[1][2]) == 0x60350,
          "the cvar classes and getters (build 6156)");
    // the getters that only read memory, on synthetic cvars: an IntRef points at its int (+0x30), a String holds its
    // text at +0x30
    int value = -12345;
    alignas(16) uint8_t intRef[0x40] = {};
    int* pv = &value;
    std::memcpy(intRef + 0x30, &pv, sizeof pv);
    alignas(16) uint8_t str[0x140] = {};
    std::memcpy(str + 0x30, "e_some_value", 13);
    const int iv = reinterpret_cast<int (*)(void*)>(t.getters[0][0])(intRef);
    const float fv = reinterpret_cast<float (*)(void*)>(t.getters[0][1])(intRef);
    const char* sv = reinterpret_cast<const char* (*)(void*)>(t.getters[1][2])(str);
    const bool readsOk = iv == value && fv == float(value) && sv == reinterpret_cast<const char*>(str + 0x30);
    printf("  IntRef::GetIVal %d, GetFVal %g, String::GetString \"%s\"%s\n", iv, double(fv), sv, readsOk ? "" : "   WRONG");
    Check(readsOk, "the cvar getters read what the snapshot captures");
    // one changed byte: IntRef::GetIVal's field (+0x30 -> +0x31)
    char* probe = t.getters[0][0] + 3;
    if (uint8_t(*probe) == 0x30) {
        DWORD old;
        VirtualProtect(probe, 1, PAGE_EXECUTE_READWRITE, &old);
        *probe = 0x31;
        optiperf::CvarTarget t2;
        const char* why2 = "accepted";
        const bool refused = !optiperf::CvarSnapshot_Locate(base, &t2, &why2);
        *probe = 0x30;
        VirtualProtect(probe, 1, old, &old);
        printf("  altered IntRef::GetIVal: %s\n", why2);
        Check(refused, "refusing an altered getter");
    } else {
        printf("  IntRef::GetIVal's probe byte not found where expected\n");
        ++g_failures;
    }
    FreeLibrary(mod);
}

// The light and frustum copies' bookkeeping (light_record.h) on synthetic lights, frustums, lists and render objects
void TestLightRecord() {
    printf("light and frustum copies\n");
    namespace lt = optiperf::lt;
    // five lights: 0-2 in level 1's list, 1 also in level 2's (the reflections' dry run added it again), 3 only in a
    // reflection's capture, 4 fake (in no list: only its corona's object points at it)
    alignas(16) static char lights[5][lt::kLightBytes];
    for (int i = 0; i < 5; ++i)
        for (size_t b = 0; b < lt::kLightBytes; ++b) lights[i][b] = char(i * 31 + int(b) * 7);
    // objects 0 and 1: the coronas of lights 2 and 4; object 2's m_pLight holds something else (a bend scale)
    alignas(16) static char objects[3][lt::kObjectBytes] = {};
    lt::Set(objects[0] + lt::kOLight, lights[2]);
    lt::Set(objects[1] + lt::kOLight, lights[4]);
    const float bend = 1.5f;
    std::memcpy(objects[2] + lt::kOLight, &bend, sizeof bend);
    char* level1[3] = {lights[0], lights[1], lights[2]};
    char* level2[1] = {lights[1]};
    lt::PtrList l1{level1, 3, 3}, l2{level2, 1, 1};
    void* captured[2] = {lights[3], lights[1]};

    lt::Copies L;
    L.Init(8, lt::kLightBytes);
    L.Add(lights[0], 2);
    L.Add(lights[1], 2);
    L.Add(lights[2], 2);
    L.Add(lights[1], 4);
    L.Add(lights[3], 4);
    L.Add(lights[4], 2);
    Check(L.Recorded() == 5 && L.Tags(1) == 6, "a light added at two levels is one record, with both levels");
    Check(L.CopyAll() == 5, "every recorded light copied");
    bool same = true;
    for (size_t i = 0; i < 5; ++i)
        same = same && L.Copy(i) && std::memcmp(L.Copy(i), L.Object(i), lt::kLightBytes) == 0 &&
               std::memcmp(L.Pristine(i), L.Object(i), lt::kLightBytes) == 0 &&
               reinterpret_cast<uintptr_t>(L.Copy(i)) % 16 == 0;
    Check(same, "the copies hold the lights' bytes, 16-aligned");
    const int inL1 = L.RedirectList(&l1), inL2 = L.RedirectList(&l2);
    const int inCaptured = L.RedirectPointers(captured, 2);
    int objs = 0;
    for (auto& o : objects) objs += L.RedirectField(o + lt::kOLight) ? 1 : 0;
    float bendNow;
    std::memcpy(&bendNow, objects[2] + lt::kOLight, sizeof bendNow);
    const bool pointed = inL1 == 3 && inL2 == 1 && inCaptured == 2 && objs == 2 && L.Map(lights[0]) != lights[0] &&
                         l1.elems[0] == L.Map(lights[0]) && l2.elems[0] == L.Map(lights[1]) &&
                         captured[0] == L.Map(lights[3]) && lt::Get(objects[0] + lt::kOLight) == L.Map(lights[2]) &&
                         lt::Get(objects[1] + lt::kOLight) == L.Map(lights[4]) && bendNow == bend;
    printf("  pointed at the copies: %d + %d list entries, %d captured lights, %d objects (of 3)%s\n", inL1, inL2,
           inCaptured, objs, pointed ? "" : "   WRONG");
    Check(pointed, "lists, captured lights and coronas' objects pointed at the copies, nothing else");

    // the frustums: light 0 has two (its array null-terminated), an object's list shares the second and adds a third
    alignas(16) static char frustums[3][lt::kFrustumBytes];
    for (int i = 0; i < 3; ++i)
        for (size_t b = 0; b < lt::kFrustumBytes; ++b) frustums[i][b] = char(i * 13 + int(b) * 3);
    void* casterElems[2] = {objects[0], objects[1]};  // (any pointers: they are kept as they are)
    lt::PtrList casters{reinterpret_cast<char**>(casterElems), 2, 4};
    lt::Set(frustums[0] + lt::kFCasters, reinterpret_cast<char*>(&casters));
    char* lightArray[lt::kLightFrustums] = {frustums[0], frustums[1]};
    char* objectElems[2] = {frustums[1], frustums[2]};
    lt::PtrList objectList{objectElems, 2, 2};
    lt::Copies F;
    F.Init(8, lt::kFrustumBytes);
    for (int j = 0; j < lt::kLightFrustums && lightArray[j]; ++j) F.Add(lightArray[j]);
    for (char* f : objectElems) F.Add(f);
    Check(F.Recorded() == 3 && F.CopyAll() == 3, "the lights' and objects' frustums copied once each");
    lt::Arena arena;
    arena.Init(4096);
    static const lt::Copies none;
    lt::PtrList* castersCopy = lt::CopyList(&casters, none, arena, 1000);
    lt::PtrList* objectCopy = lt::CopyList(&objectList, F, arena, 64);
    F.SetField(0, lt::kFCasters, reinterpret_cast<char*>(castersCopy));
    const bool lists = castersCopy && objectCopy && castersCopy->count == 2 && castersCopy->capacity == 2 &&
                       castersCopy->elems[1] == casters.elems[1] && objectCopy->elems[0] == F.Map(frustums[1]) &&
                       objectCopy->elems[1] == F.Map(frustums[2]) && arena.Contains(reinterpret_cast<char*>(objectCopy)) &&
                       lt::Get(F.Copy(0) + lt::kFCasters) == reinterpret_cast<char*>(castersCopy) &&
                       lt::Get(frustums[0] + lt::kFCasters) == reinterpret_cast<char*>(&casters);
    Check(lists, "lists copied into the arena, their frustums at the copies; a copy pointed at its copied list");
    lt::Arena tiny;
    tiny.Init(16);
    Check(!lt::CopyList(&objectList, F, tiny, 64), "a list the arena can't take is not copied");

    // the flush: writes light 1's shadow channel mask (+0x6C) through level 1's list, and a frustum's field of view
    const uint32_t mask = 0x3F800000u;
    std::memcpy(l1.elems[1] + 0x6C, &mask, sizeof mask);
    const uint32_t fov = 0x42B40000u;
    std::memcpy(F.Map(frustums[1]) + 0xF0, &fov, sizeof fov);
    lt::Diff written, changed, fWritten, fChanged;
    L.Compare(written, changed);
    F.Compare(fWritten, fChanged);
    Check(written.objects == 1 && written.words == 1 && written.perWord[0x6C / 4] == 1 && changed.objects == 0,
          "the flush's write found in its light's copy, the lights unchanged");
    Check(fWritten.objects == 1 && fWritten.words == 1 && fWritten.perWord[0xF0 / 4] == 1 && fChanged.objects == 0,
          "a frustum's write found, and a copied list's pointer not taken for one");
    // a light changed while the flush ran (through a pointer the redirect missed): found
    lights[3][0x10] = char(lights[3][0x10] ^ 1);
    lt::Diff w2, c2;
    L.Compare(w2, c2);
    Check(c2.objects == 1 && c2.words == 1 && c2.perWord[0x10 / 4] == 1, "a light changed during the flush is found");
    lights[3][0x10] = char(lights[3][0x10] ^ 1);
    // a pointer into a copy kept elsewhere: put back to the same place in its light
    char* kept[2] = {L.Map(lights[2]) + 0x44, lights[0]};
    const size_t unmapped = L.UnmapRange(reinterpret_cast<char*>(kept), sizeof kept);
    Check(unmapped == 1 && kept[0] == lights[2] + 0x44 && kept[1] == lights[0], "a pointer into a copy, put back");

    // meanwhile another thread (on a render thread, the next frame) wrote a word of light 4 that the flush wrote too, and
    // the word after one the flush wrote in light 0: the original's word is kept, the flush's next to it goes back
    const uint32_t flushWord = 0x11111111u, nextWord = 0x22222222u;
    std::memcpy(L.Map(lights[4]) + 0x70, &flushWord, 4);
    std::memcpy(lights[4] + 0x70, &nextWord, 4);
    std::memcpy(L.Map(lights[0]) + 0x80, &flushWord, 4);
    std::memcpy(lights[0] + 0x84, &nextWord, 4);

    // after the flush: the writes go into the originals (not the copied list's pointer); the lists and objects go back
    size_t keptWords = 0, fKept = 0;
    const size_t back = L.WriteBack(&keptWords), fBack = F.WriteBack(&fKept);
    uint32_t now, fovNow, w4, w0, w0next;
    std::memcpy(&now, lights[1] + 0x6C, sizeof now);
    std::memcpy(&fovNow, frustums[1] + 0xF0, sizeof fovNow);
    std::memcpy(&w4, lights[4] + 0x70, 4);
    std::memcpy(&w0, lights[0] + 0x80, 4);
    std::memcpy(&w0next, lights[0] + 0x84, 4);
    Check(keptWords == 1 && fKept == 0 && w4 == nextWord && w0 == flushWord && w0next == nextWord,
          "a word the original changed meanwhile keeps the original's value; the flush's next to it is written back");
    L.Restore();
    F.Restore();
    const bool restored = l1.elems[0] == lights[0] && l1.elems[1] == lights[1] && l1.elems[2] == lights[2] &&
                          l2.elems[0] == lights[1] && lt::Get(objects[0] + lt::kOLight) == lights[2] &&
                          lt::Get(objects[1] + lt::kOLight) == lights[4] &&
                          lt::Get(frustums[0] + lt::kFCasters) == reinterpret_cast<char*>(&casters);
    printf("  written back: %zu bytes of lights, %zu of frustums%s\n", back, fBack,
           now == mask && fovNow == fov ? "" : "   WRONG");
    Check(back >= 2 && back <= 8 && fBack >= 1 && fBack <= 4 && now == mask && fovNow == fov && restored,
          "written back, lists and objects put back");
    L.Clear();
    Check(L.Recorded() == 0 && L.Map(lights[0]) == lights[0], "nothing recorded once cleared");

    // an arena too small: the light past it keeps its pointer
    lt::Copies small;
    small.Init(2, lt::kLightBytes);
    small.Add(lights[0]);
    small.Add(lights[1]);
    small.Add(lights[2]);
    char* three[3] = {lights[0], lights[1], lights[2]};
    lt::PtrList pl{three, 3, 3};
    const size_t copiedSmall = small.CopyAll();
    const int redirectedSmall = small.RedirectList(&pl);
    Check(copiedSmall == 2 && redirectedSmall == 2 && pl.elems[2] == lights[2] && !small.Copy(2),
          "past the arena, a light keeps its pointer");
    small.Restore();
    Check(pl.elems[0] == lights[0] && pl.elems[1] == lights[1], "and the others are put back");
}

void TestRangeCopies() {
    printf("range copies\n");
    namespace rc = optiperf::rc;
    // the engine's memory: a manager with parameters inside it (as the sky's), an object (as the ocean) whose fog
    // parameters point at an array (its vertices), and memory nothing copies
    alignas(16) static char engine[0x2000];
    for (size_t b = 0; b < sizeof engine; ++b) engine[b] = char(b * 7 + 3);
    char* const manager = engine;
    char* const params = manager + 0x1A8;  // 0xD8 bytes
    char* const ocean = engine + 0x400;    // 0x3B0 bytes
    char* const verts = engine + 0x800;    // 0x100 bytes
    char* const other = engine + 0x1F00;
    rc::Set(ocean + 0x2E8, verts);

    rc::RangeCopies R;
    R.Init(0x4000);
    char* const pc = R.Add(params, 0xD8, 0, params);
    Check(pc && std::memcmp(pc, params, 0xD8) == 0 && reinterpret_cast<uintptr_t>(pc) % 16 == 0,
          "a range copied, 16-aligned");
    Check(R.Add(params + 0x10, 0x20, 0, params + 0x10) == pc + 0x10, "a range inside a copied one is that copy");
    Check(!R.Add(params + 0xD0, 0x20, 0, params + 0xD0) && !R.Add(manager + 0x190, 0x20, 0, manager + 0x190),
          "a range overlapping a copied one without lying inside it is refused");
    char* const oc = R.Add(ocean, 0x3B0, 1, ocean);
    char* const vc = R.Add(verts, 0x100, 2, verts);
    Check(oc && vc && R.Ranges() == 3, "three ranges");
    Check(R.Map(ocean + 0x2A8) == oc + 0x2A8 && R.Map(params) == pc && !R.Map(other) && !R.Map(ocean + 0x3B0) &&
              !R.Map(params - 1),
          "a pointer into a range maps to the same byte of its copy, others to nothing");
    Check(R.Unmap(oc + 0x10) == ocean + 0x10 && !R.Unmap(oc + 0x3C0 + 0x10) && !R.Unmap(other),
          "a pointer into a copy maps back, one into a twin doesn't");
    Check(R.Find(ocean + 0x10, 0x20) == oc + 0x10 && !R.Find(ocean + 0x3A0, 0x20), "Find: only inside a range");
    Check(R.KindAt(verts + 5) == 2 && R.KindAt(other) == -1, "the kind of a range");

    // the ocean copy's fog parameters given the vertices' copy
    Check(R.GivePointer(oc + 0x2E8, vc) && rc::Get(oc + 0x2E8) == vc && rc::Get(ocean + 0x2E8) == verts,
          "a pointer given to a copy, not to its original");
    Check(R.Given(oc + 0x2E8) && !R.Given(oc + 0x2F0), "the given pointer known as such");

    // the renderer's fields
    char* skyField = params;
    char* customField = ocean + 0x2A8;
    char* otherField = other;
    char* rewritten = verts + 8;
    const bool redirected = R.RedirectField(reinterpret_cast<char*>(&skyField)) &&
                            R.RedirectField(reinterpret_cast<char*>(&customField)) &&
                            !R.RedirectField(reinterpret_cast<char*>(&otherField)) &&
                            R.RedirectField(reinterpret_cast<char*>(&rewritten));
    Check(redirected && skyField == pc && customField == oc + 0x2A8 && otherField == other && R.FieldsRedirected() == 3,
          "the renderer's fields pointed at the copies, and only those");

    // the flush: two bytes into the ocean's copy, a pointer into a copy into the parameters' copy, one redirected field
    // set to something else; meanwhile one original byte changes (none should)
    oc[0x10] = char(oc[0x10] ^ 0x5A);
    oc[0x11] = char(oc[0x11] ^ 0x21);
    rc::Set(pc + 8, vc + 0x20);
    rewritten = other;
    verts[0x40] = char(verts[0x40] ^ 1);
    rc::Diff written, changed;
    size_t changedRanges = 0, changedFirst = 0, changedBytes = 0;
    R.Compare(written, changed, [&](size_t, size_t first, size_t bytes) {
        ++changedRanges;
        changedFirst = first;
        changedBytes = bytes;
    });
    printf("  written: sky %llu (%llu bytes), ocean %llu (%llu bytes); changed: vertices %llu (%llu bytes, at +0x%zX)\n",
           written.ranges[0], written.bytes[0], written.ranges[1], written.bytes[1], changed.ranges[2],
           changed.bytes[2], changedFirst);
    Check(written.ranges[0] == 1 && written.bytes[0] >= 1 && written.ranges[1] == 1 && written.bytes[1] == 2 &&
              written.ranges[2] == 0,
          "what the flush wrote into the copies");
    Check(changed.ranges[2] == 1 && changed.bytes[2] == 1 && changedRanges == 1 && changedFirst == 0x40 &&
              changedBytes == 1 && changed.ranges[1] == 0,
          "the original that changed while the flush ran (not the one given a pointer)");
    verts[0x40] = char(verts[0x40] ^ 1);

    size_t translated = 0;
    const size_t back = R.WriteBack(&translated);
    Check(back >= 3 && ocean[0x10] == oc[0x10] && ocean[0x11] == oc[0x11], "the flush's bytes written back");
    Check(rc::Get(params + 8) == verts + 0x20 && translated == 1,
          "a pointer into a copy among them written as the original it stands for");
    Check(rc::Get(ocean + 0x2E8) == verts, "a given pointer is not written back");
    Check(R.Restore() == 2 && skyField == params && customField == ocean + 0x2A8 && rewritten == other,
          "the fields put back, but the one the flush set to something else");
    char* kept[2] = {pc + 0x10, other};
    Check(R.UnmapRange(reinterpret_cast<char*>(kept), sizeof kept) == 1 && kept[0] == params + 0x10 && kept[1] == other,
          "pointers into the copies found in memory put back");
    R.Clear();
    Check(R.Ranges() == 0 && !R.Map(params) && R.Used() == 0, "cleared");

    rc::RangeCopies S;  // a small arena: room for one range of 0x80 (and its twin)
    S.Init(0x1F0);
    Check(S.Add(engine, 0x80, 0, engine) && !S.Add(engine + 0x100, 0x80, 0, engine + 0x100),
          "past the arena, a range isn't copied");
}

void TestFlushThread() {
    printf("the flush's thread\n");
    SetEnvironmentVariableW(L"OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD", L"worker");
    optiperf::FlushThread_Install(L"shadow_prepass_test.exe", reinterpret_cast<char*>(GetModuleHandleW(nullptr)),
                                  nullptr);
    SetEnvironmentVariableW(L"OPTIPERF_SHADOW_PREPASS_FLUSH_THREAD", nullptr);
    struct Seen {
        DWORD tid;
        bool onFlush;
        unsigned mxcsr;
        int runs;
    } seen = {};
    constexpr unsigned kFlags = 0x3F, kRounding = 0x6000;
    const DWORD caller = GetCurrentThreadId();
    const unsigned was = _mm_getcsr();
    _mm_setcsr((was & ~kRounding) | 0x8040u | 0x2000u);  // flush to zero, denormals are zero, rounding down
    const unsigned mine = _mm_getcsr();
    optiperf::FlushThread_Run(
        [](void* p) {
            auto* s = static_cast<Seen*>(p);
            s->tid = GetCurrentThreadId();
            s->onFlush = optiperf::OnFlushThread();
            s->mxcsr = _mm_getcsr();
            _mm_setcsr(s->mxcsr ^ kRounding);  // the flush changes the rounding (to up)
            ++s->runs;
        },
        &seen, nullptr, nullptr);
    const unsigned back = _mm_getcsr();
    _mm_setcsr(was);
    printf("  caller %lu, flush on %lu; MXCSR given 0x%04X, seen 0x%04X, back 0x%04X\n", caller, seen.tid, mine,
           seen.mxcsr, back);
    Check(seen.runs == 1 && seen.tid != caller && seen.tid == optiperf::FlushThread(), "the flush ran on the worker");
    Check(seen.onFlush && !optiperf::OnFlushThread(), "the flush's thread known as such there only");
    Check((seen.mxcsr & ~kFlags) == (mine & ~kFlags), "the caller's MXCSR given to the flush");
    Check((back & ~kFlags) == ((mine ^ kRounding) & ~kFlags), "and the flush's back to the caller");
    const DWORD first = seen.tid;
    optiperf::FlushThread_Run(
        [](void* p) {
            auto* s = static_cast<Seen*>(p);
            s->tid = GetCurrentThreadId();
            ++s->runs;
        },
        &seen, nullptr, nullptr);
    Check(seen.runs == 2 && seen.tid == first, "the next flush on the same worker");
}

void TestLightLocate(const std::wstring& dir) {
    printf("locating the light code in CryRenderD3D10.dll\n");
    const std::wstring path = dir + L"\\CryRenderD3D10.dll";
    HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!mod) {
        printf("  cannot map %ls (error %lu)\n", path.c_str(), GetLastError());
        ++g_failures;
        return;
    }
    char* base = reinterpret_cast<char*>(mod);
    optiperf::LightTarget t;
    const char* why = "";
    const bool found = optiperf::LightSnapshot_Locate(base, &t, &why);
    if (!found) printf("  %s\n", why);
    Check(found, "locating EF_ADDDlight and the CDLight constructor");
    if (!found) {
        FreeLibrary(mod);
        return;
    }
    auto rva = [base](const void* p) { return unsigned(static_cast<const char*>(p) - base); };
    printf("  EF_ADDDlight +0x%X, CDLight constructor +0x%X\n", rva(t.addLight), rva(t.lightCtor));
    Check(rva(t.addLight) == 0x103E10 && rva(t.lightCtor) == 0xED20, "EF_ADDDlight and the constructor (build 6156)");
    // one changed byte each: the corona object's m_pLight store (+0x128 -> +0x130), the light's size (0x258 -> 0x260),
    // the renderer's sun field (+0x141B0 -> +0x141B8)
    struct Probe {
        char* at;
        uint8_t was, now;
        const char* what;
    } probes[] = {{t.addLight + 0x2FB + 3, 0x28, 0x30, "the coronas' m_pLight"},
                  {t.lightCtor + 0x10, 0x58, 0x60, "the light's size"},
                  {base + 0x9F394 + 3, 0xB0, 0xB8, "the per-list setup's sun"}};
    for (const Probe& p : probes) {
        if (uint8_t(*p.at) != p.was) {
            printf("  %s: probe byte not found where expected\n", p.what);
            ++g_failures;
            continue;
        }
        DWORD old;
        VirtualProtect(p.at, 1, PAGE_EXECUTE_READWRITE, &old);
        *p.at = char(p.now);
        optiperf::LightTarget t2;
        const char* why2 = "accepted";
        const bool refused = !optiperf::LightSnapshot_Locate(base, &t2, &why2);
        *p.at = char(p.was);
        VirtualProtect(p.at, 1, old, &old);
        printf("  altered %s: %s\n", p.what, why2);
        Check(refused, "refusing altered light code");
    }
    FreeLibrary(mod);
}
#endif

}  // namespace

int wmain(int argc, wchar_t** argv) {
#ifdef _WIN64
    const std::wstring dir = argc > 1 ? argv[1] : L"D:\\Program Files\\Games\\GOG\\Crysis\\Bin64";
    TestLists();
    TestCompare();
    TestLocate(dir);
    TestVtableCounter();
    TestRenderGate();
    TestParticleRecord();
    TestEnvironment(dir);
    TestSkinRecord();
    TestSkinLocate(dir);
    TestSystem(dir);
    TestFog(dir);
    TestOcean(dir);
    TestMaterials(dir);
    TestCvars(dir);
    TestLightRecord();
    TestLightLocate(dir);
    TestRangeCopies();
    TestFlushThread();
#else
    (void)argc;
    (void)argv;
    printf("the shadow pre-pass is x64 only: nothing to test in the 32-bit build\n");
#endif
    printf(g_failures ? "FAILED (%d)\n" : "OK\n", g_failures);
    return g_failures ? 1 : 0;
}
