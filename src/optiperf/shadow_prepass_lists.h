#pragma once
// Render-item lists of the x64 renderer, as the shadow pre-pass (shadow_prepass.cpp) moves them around. No engine
// calls here, so tests/shadow_prepass_test.cpp can drive it with synthetic lists.
//
// The renderer keeps one set of item lists for the whole frame, SRendItem::m_RendItems[2][15] (indexed [nAfterWater]
// [EFSLIST_*]). A recursion level (EF_StartEf .. EF_EndEf3D) remembers where each list stood when it opened, appends
// its items, and processes the part from there to the end; nothing is removed until the next frame. So everything a
// level added is a suffix of each list, which can be taken out and appended again later.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace optiperf::sp {

// SRendItem (x64), as the renderer's EF_AddEf code fills it (SRendItem::mfAdd)
struct RendItem {
    uint32_t sortVal;  // shader, resources and technique ids
    uint32_t pad0;
    char* elem;        // CRendElement*
    uint32_t objSort;  // object flags (high half) | m_nRAEId
    uint32_t pad1;
    char* obj;         // CRenderObject*
    uint32_t dynLM;    // CRenderObject::m_DynLMMask
    uint32_t batchFlags;
};
#ifdef _WIN64  // the layouts are the x64 renderer's (the patch is x64 only)
static_assert(sizeof(RendItem) == 0x28, "SRendItem is 40 bytes on x64");
#endif

// TArray<SRendItem>: elements, count, capacity
struct ItemList {
    RendItem* elems;
    uint32_t count;
    uint32_t capacity;
};
#ifdef _WIN64
static_assert(sizeof(ItemList) == 16, "TArray is 16 bytes on x64");
#endif

constexpr int kLists = 30;  // 2 x 15

using ReallocFn = void* (*)(void* p, size_t bytes);  // the module's allocator entry (CryModuleRealloc)

// Mode=verify: what one caster's Render call got, in the pre-pass and in PrepareDepthMap's loop.
struct CallInputs {
    void* caster;
    uint8_t params[200];  // SRendParams
    uint32_t persFlags, persFlags2;
    int level;
    int frameRec, frame;  // renderer frame ids: with recursive renders, and BeginFrame's
    float camera[3];      // the renderer camera's position (GetCamera)
    float view[3];        // the system view camera's position (the caster loop's distances)
};

// The items one shadow-map pass added, between the pre-pass and its replay: a directional map, or one side of a
// point light's map.
struct Stash {
    char* frustum = nullptr;
    int side = -1;               // omni-directional maps: 0..5; -1 for a directional map
    void* casterList = nullptr;  // the frustum's caster list when it was rendered, to notice a change before the replay
    int casterCount = 0;
    uintptr_t casterHash = 0;
    int casters = 0;    // casters rendered: PrepareDepthMap counts them in its shadow texture
    int state = 0;      // shadow_prepass.cpp: pending, replayed, compared, ...
    uint32_t counts[kLists] = {};
    std::vector<RendItem> items;  // list 0's items, then list 1's, ...
    // compare mode: which caster added which items (after caster i, list k held ends[i * kLists + k] of the level's items)
    std::vector<void*> casterPtrs;
    std::vector<uint32_t> ends;
    // verify mode: the inputs of each Render call the pre-pass made, and an omni side's camera (CCamera) it culled with
    std::vector<CallInputs> calls;
    std::vector<uint8_t> camera;

    void Reset(char* f, int s = -1) {
        frustum = f;
        side = s;
        casterList = nullptr;
        casterCount = 0;
        casterHash = 0;
        casters = 0;
        state = 0;
        std::memset(counts, 0, sizeof counts);
        items.clear();
        casterPtrs.clear();
        ends.clear();
        calls.clear();
        camera.clear();
    }

    // the caster that added item `index` of list k (compare mode), or null
    void* CasterOf(int k, int index) const {
        for (size_t i = 0; i < casterPtrs.size(); ++i)
            if (ends[i * kLists + size_t(k)] > uint32_t(index)) return casterPtrs[i];
        return nullptr;
    }
};

// Moves what the open level added (each list from start[k] to its end) into s and shortens the lists again.
inline void TakeLevel(ItemList* lists, const uint32_t* start, Stash& s) {
    for (int k = 0; k < kLists; ++k) {
        const uint32_t from = start[k], to = lists[k].count;
        const uint32_t n = to > from ? to - from : 0;
        s.counts[k] = n;
        if (n) {
            s.items.insert(s.items.end(), lists[k].elems + from, lists[k].elems + to);
            lists[k].count = from;
        }
    }
}

// Appends a stash to the end of the lists. Lists grow the way the engine grows them (capacity = n + n/2 + 10) and
// through the engine's own allocator. False if an allocation failed (the lists are then left as they were).
inline bool PutBack(ItemList* lists, const Stash& s, ReallocFn realloc) {
    for (int k = 0; k < kLists; ++k) {
        const uint32_t need = lists[k].count + s.counts[k];
        if (need <= lists[k].capacity) continue;
        const uint32_t cap = need + (need >> 1) + 10;
        void* p = realloc(lists[k].elems, size_t(cap) * sizeof(RendItem));
        if (!p) return false;
        lists[k].elems = static_cast<RendItem*>(p);
        lists[k].capacity = cap;
    }
    const RendItem* src = s.items.data();
    for (int k = 0; k < kLists; ++k) {
        const uint32_t n = s.counts[k];
        if (!n) continue;
        std::memcpy(lists[k].elems + lists[k].count, src, size_t(n) * sizeof(RendItem));
        lists[k].count += n;
        src += n;
    }
    return true;
}

// --- compare mode: compare what the original casters added with the stash ------------------------------------------

// CRenderObject bytes compared between the two runs: everything after the identity fields (vtable, m_Id, m_VisId,
// m_nObjDataId, m_FogVolumeContribIdx) up to the end of the object, except
//   - fields EF_GetObject doesn't initialize and shadow casters don't set: they hold whatever the pool slot's previous
//     user left (vanilla too), and the two runs use different slots;
//   - fields that index per-frame tables filled in call order;
//   - m_vBending: CVegetation::Render advances the node's bending state on every call, and verify mode renders each
//     caster twice (Mode=on doesn't: there each caster is rendered once, in vanilla's order).
// The object matrix is compared with a tolerance, for the same reason: some entities move their render matrix a
// little on every Render call (each shadow cascade already gets a slightly different one in vanilla).
constexpr uint32_t kObjCompareFrom = 0x10, kObjCompareTo = 0x130;
struct ByteRange {
    uint32_t from, to;
};
constexpr ByteRange kObjSkips[] = {
    {0x14, 0x44},    // m_II.m_Matrix: compared with a tolerance (kObjMatrix)
    {0x54, 0x84},    // m_prevMatrix (motion blur): not initialized
    {0x90, 0xA8},    // m_fTempVars[6]: not initialized
    {0xD2, 0xD4},    // m_nWaveID: index into CRenderObject::m_Waves
    {0xFC, 0x104},   // m_vBending
    {0x118, 0x11C},  // m_HMAData: not initialized
};
constexpr uint32_t kObjMatrix = 0x14;  // Matrix34, 12 floats
constexpr float kMatrixTolerance = 1e-3f;  // relative to max(1, |value|)

// Elements are pooled per call for some types (particles: a new CREParticle for every EF_AddParticlesToScene), so two
// runs can hand out different ones for the same thing: the caller decides whether two elements are equivalent.
using SameElementFn = bool (*)(const char* a, const char* b);

enum class Field { Count, SortVal, Elem, ObjSort, DynLM, BatchFlags, Object };

struct Difference {
    int list = -1;   // first list that differs
    int index = -1;  // item in it (-1: the counts differ)
    Field field = Field::Count;
    uint32_t objOffset = 0;  // Field::Object: first differing byte of the object
    uint64_t a = 0, b = 0;   // the differing values (current run, stash), for the log
};

// *approx is set when the matrices differ, but within kMatrixTolerance.
inline bool ObjectsEqual(const char* a, const char* b, uint32_t* at, bool* approx) {
    uint32_t o = kObjCompareFrom;
    for (const ByteRange& skip : kObjSkips) {
        for (; o < skip.from; ++o)
            if (a[o] != b[o]) return *at = o, false;
        o = skip.to;
    }
    for (; o < kObjCompareTo; ++o)
        if (a[o] != b[o]) return *at = o, false;
    if (std::memcmp(a + kObjMatrix, b + kObjMatrix, 48) != 0) {
        for (uint32_t i = 0; i < 12; ++i) {
            float fa, fb;
            std::memcpy(&fa, a + kObjMatrix + 4 * i, 4);
            std::memcpy(&fb, b + kObjMatrix + 4 * i, 4);
            const float scale = (fa < 0 ? -fa : fa) > 1.f ? (fa < 0 ? -fa : fa) : 1.f;
            const float diff = fa - fb;
            if (!((diff < 0 ? -diff : diff) <= kMatrixTolerance * scale)) return *at = kObjMatrix + 4 * i, false;
        }
        *approx = true;
    }
    return true;
}

// Items [start[k], end) of each list against the stash. Objects are compared by contents: the original run allocates
// its own render objects. Returns true if identical; *elemsByContents counts elements accepted through sameElem,
// *nearMatrices objects whose matrices were equal only within the tolerance.
inline bool CompareLevel(const ItemList* lists, const uint32_t* start, const Stash& s, SameElementFn sameElem,
                         Difference* d, uint64_t* elemsByContents, uint64_t* nearMatrices) {
    const RendItem* ref = s.items.data();
    for (int k = 0; k < kLists; ++k) {
        const uint32_t from = start[k], to = lists[k].count;
        const uint32_t n = to > from ? to - from : 0;
        if (n != s.counts[k]) {
            *d = Difference{k, -1, Field::Count, 0, n, s.counts[k]};
            return false;
        }
        for (uint32_t i = 0; i < n; ++i, ++ref) {
            const RendItem& a = lists[k].elems[from + i];
            auto differ = [&](Field f, uint64_t va, uint64_t vb, uint32_t at = 0) {
                *d = Difference{k, int(i), f, at, va, vb};
                return false;
            };
            if (a.sortVal != ref->sortVal) return differ(Field::SortVal, a.sortVal, ref->sortVal);
            if (a.objSort != ref->objSort) return differ(Field::ObjSort, a.objSort, ref->objSort);
            if (a.dynLM != ref->dynLM) return differ(Field::DynLM, a.dynLM, ref->dynLM);
            if (a.batchFlags != ref->batchFlags) return differ(Field::BatchFlags, a.batchFlags, ref->batchFlags);
            if (a.elem != ref->elem) {
                if (!a.elem || !ref->elem || !sameElem || !sameElem(a.elem, ref->elem))
                    return differ(Field::Elem, uintptr_t(a.elem), uintptr_t(ref->elem));
                ++*elemsByContents;
            }
            if (a.obj != ref->obj) {
                uint32_t at = 0;
                bool approx = false;
                if (!a.obj || !ref->obj) return differ(Field::Object, uintptr_t(a.obj), uintptr_t(ref->obj));
                if (!ObjectsEqual(a.obj, ref->obj, &at, &approx)) {
                    uint64_t va = 0, vb = 0;
                    std::memcpy(&va, a.obj + (at & ~7u), 8);
                    std::memcpy(&vb, ref->obj + (at & ~7u), 8);
                    return differ(Field::Object, va, vb, at);
                }
                if (approx) ++*nearMatrices;
            }
        }
    }
    return true;
}

inline const char* FieldName(Field f) {
    switch (f) {
        case Field::Count: return "item count";
        case Field::SortVal: return "sort value";
        case Field::Elem: return "render element";
        case Field::ObjSort: return "object sort key";
        case Field::DynLM: return "light mask";
        case Field::BatchFlags: return "batch flags";
        case Field::Object: return "render object";
    }
    return "?";
}

}  // namespace optiperf::sp
