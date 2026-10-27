#pragma once
// Copies of the 3D engine's materials as their getters read them, and those getters transcribed over the copies
// (material_snapshot.cpp). No engine calls here: tests/shadow_prepass_test.cpp runs Cry3DEngine.dll's own getters on
// synthetic materials and compares them with the transcriptions.
//
// x64 Cry3DEngine.dll, build 6156:
//   CMatInfo: flags +0x28 (0x100: has sub-materials; the renderer skips layers with 0x400), its shader item +0x30; the
//     sub-materials +0x60 (a DynArray of CMatInfo*: the count in the 4 bytes before the first element); the layers
//     +0x70 (a pointer to a std::vector of CMaterialLayer*, null without layers); and what GetLayer(mask, usage) noted
//     last: the mask +0x78, the usage +0x79, the layer +0x80 (nothing in the game reads these three back).
//   CMaterialLayer: flags +0x8 (bit 0: disabled), its shader item +0x10 (shader, resources, technique: 24 bytes).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "key_index.h"

namespace optiperf::mt {

constexpr int kMatFlags = 0x28, kMatItem = 0x30, kMatSubs = 0x60, kMatLayers = 0x70;
constexpr int kActiveMask = 0x78, kActiveUsage = 0x79, kActiveLayer = 0x80;
constexpr int kLayerFlags = 0x8, kLayerItem = 0x10;
constexpr uint32_t kMultiSub = 0x100;
constexpr int32_t kMaxSubs = 1024;   // a material with more sub-materials, or more layers, isn't copied: its calls go to
constexpr int32_t kMaxLayers = 64;   // the 3D engine
constexpr size_t kItemBytes = 24;

struct Mat {
    const char* self = nullptr;
    uint32_t flags = 0;
    int32_t subCount = 0;      // as GetSubMtlCount reads it
    uint32_t subs = 0;         // the first of subCount sub-materials in Snapshot::ptrs
    bool hasLayers = false;    // the layer vector exists
    int32_t layerCount = 0;    // its size
    uint32_t layers = 0;       // the first of layerCount layers in Snapshot::ptrs
    bool subsTaken = false;    // its sub-materials copied too
    bool used = false;
};
struct Layer {
    const char* self = nullptr;
    uint64_t item[kItemBytes / 8] = {};  // (the callers read it as three qwords)
    uint8_t flags = 0;
    bool used = false;
};

template <class T>
T Read(const char* p, int off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}

class Snapshot {
public:
    void Clear() {
        mats.clear();
        layers.clear();
        ptrs.clear();
        matIndex_.Clear();
        layerIndex_.Clear();
    }
    // Copies a material, and with withSubs its sub-materials (one level, as the renderer asks for them), and the
    // layers of each; false if its fields are out of range (then it isn't copied).
    bool AddMaterial(const char* m, bool withSubs) {
        if (!m) return false;
        uint32_t i;
        if (const uint32_t* at = matIndex_.Find(Key(m))) {
            i = *at;
        } else {
            Mat x;
            if (!Copy(m, x)) return false;
            i = uint32_t(mats.size());
            mats.push_back(x);
            matIndex_.Insert(Key(m), i);
        }
        if (withSubs && !mats[i].subsTaken) {
            mats[i].subsTaken = true;
            const uint32_t first = mats[i].subs, n = uint32_t(mats[i].subCount);
            for (uint32_t k = 0; k < n; ++k) AddMaterial(ptrs[first + k], false);  // (ptrs only grows: indices hold)
        }
        return true;
    }
    Mat* FindMat(const void* m) {
        const uint32_t* at = matIndex_.Find(Key(m));
        return at ? &mats[*at] : nullptr;
    }
    Layer* FindLayer(const void* l) {
        const uint32_t* at = layerIndex_.Find(Key(l));
        return at ? &layers[*at] : nullptr;
    }

    std::vector<Mat> mats;
    std::vector<Layer> layers;
    std::vector<const char*> ptrs;  // the sub-materials' and the layers' pointers, as the material holds them

private:
    static uint64_t Key(const void* p) { return uint64_t(uintptr_t(p)); }
    bool Copy(const char* m, Mat& x) {
        const char* subArr = Read<const char*>(m, kMatSubs);
        if (!subArr) return false;
        const int32_t subCount = Read<int32_t>(subArr, -4);
        if (subCount < 0 || subCount > kMaxSubs) return false;
        const char* vec = Read<const char*>(m, kMatLayers);
        const char* begin = vec ? Read<const char*>(vec, 0) : nullptr;
        const int64_t bytes = vec ? Read<const char*>(vec, 8) - begin : 0;
        if (bytes < 0 || bytes % 8 || bytes / 8 > kMaxLayers || (bytes && !begin)) return false;
        x.self = m;
        x.flags = Read<uint32_t>(m, kMatFlags);
        x.subCount = subCount;
        x.subs = uint32_t(ptrs.size());
        for (int32_t k = 0; k < subCount; ++k) ptrs.push_back(Read<const char*>(subArr, k * 8));
        x.hasLayers = vec != nullptr;
        x.layerCount = int32_t(bytes / 8);
        x.layers = uint32_t(ptrs.size());
        for (int32_t k = 0; k < x.layerCount; ++k) {
            const char* l = Read<const char*>(begin, k * 8);
            ptrs.push_back(l);
            if (l && !layerIndex_.Find(Key(l))) {
                Layer y;
                y.self = l;
                y.flags = Read<uint8_t>(l, kLayerFlags);
                std::memcpy(y.item, l + kLayerItem, kItemBytes);
                layerIndex_.Insert(Key(l), uint32_t(layers.size()));
                layers.push_back(y);
            }
        }
        return true;
    }
    KeyIndex matIndex_, layerIndex_;
};

// --- the getters (Cry3DEngine +0x1520E0 GetFlags, +0x151460 GetSubMtlCount, +0x151D10 GetSubMtl, +0x150DF0
// GetLayerCount, +0x1515E0 GetLayer(slot), +0x151520 GetLayer(mask, usage); +0x150C20 IsEnabled), over a copy -------
inline uint32_t Flags(const Mat& m) { return m.flags; }
inline int32_t SubMtlCount(const Mat& m) { return m.subCount; }
inline const char* SubMtl(const Snapshot& s, const Mat& m, int32_t i) {
    if (m.subCount == 0 || !(m.flags & kMultiSub) || i < 0 || i >= m.subCount) return nullptr;
    return s.ptrs[m.subs + uint32_t(i)];
}
inline uint64_t LayerCount(const Mat& m) { return m.hasLayers ? uint64_t(int64_t(m.layerCount)) : 0; }
inline const char* LayerAt(const Snapshot& s, const Mat& m, uint32_t slot) {
    if (!m.hasLayers || slot >= uint32_t(m.layerCount)) return nullptr;
    return s.ptrs[m.layers + slot];
}
// GetLayer(mask, usage): the first slot whose bit is set in the mask (the bit test takes the slot modulo 32). Found,
// the material notes (mask, usage, layer) (see Note); returns whether a slot was found, and its layer (may be null).
inline bool LayerByMask(const Snapshot& s, const Mat& m, uint8_t mask, const char** layer) {
    *layer = nullptr;
    if (!m.hasLayers || !mask || m.layerCount <= 0) return false;
    for (int32_t k = 0; k < m.layerCount; ++k) {
        if ((uint32_t(mask) >> (uint32_t(k) & 31)) & 1) {
            *layer = s.ptrs[m.layers + uint32_t(k)];
            return true;
        }
    }
    return false;
}
// what GetLayer(mask, usage) writes into the material when it finds a slot
inline void Note(char* material, uint8_t mask, uint8_t usage, const char* layer) {
    material[kActiveMask] = char(mask);
    material[kActiveUsage] = char(usage);
    std::memcpy(material + kActiveLayer, &layer, sizeof layer);
}
inline uint32_t Enabled(const Layer& l) { return uint32_t(uint8_t(~l.flags) & 1); }

}  // namespace optiperf::mt
