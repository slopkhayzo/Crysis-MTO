#pragma once
// Record and replay of the answers to ISkinnable::GetSkeletonPose (skin_prepass.cpp). No engine calls here, so
// tests/shadow_prepass_test.cpp can drive it with synthetic implementations.
//
// The renderer's skinned draw asks the object's skinnable (a character, a skin attachment, a touch-bendable plant) for
// its pose: GetSkeletonPose(lod, matrix, &quatsL, &quatsS, &mbQuatsL, &mbQuatsS, shapeDeformation, &flags, &remap)
// returns the bone count and writes some of its outputs: pointers to the skinnable's bone arrays (the current pose,
// and the motion-blur pose when the flags ask for it), the shape-deformation weights (8 Vec4), the flags (in: 2, plus
// 4 in the object motion-blur pass; out: 1, plus 2 when morph targets apply) and a bone remap table (skin
// attachments). Which outputs an implementation writes is found by filling them with sentinels before a recorded
// call; a replay writes exactly those, with the recorded values.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "key_index.h"

namespace optiperf::sk {

using PoseFn = uint32_t (*)(void* self, uint32_t lod, const float* matrix, void** quatsL, void** quatsS,
                            void** mbQuatsL, void** mbQuatsS, float* shape, uint32_t* flags, void** remap);

enum PtrOutput { kQuatsL, kQuatsS, kMbQuatsL, kMbQuatsS, kRemap, kPtrOutputs };
constexpr int kShapeFloats = 32;  // what the implementations write: 8 Vec4
constexpr int kShapeBuffer = 34;  // the renderer's buffer
constexpr uint8_t kShapeWritten = 1 << kPtrOutputs;
#ifdef _WIN64
constexpr uintptr_t kSentinel = 0xDEADBEEFDEADBEEFull;  // not a canonical x64 address: never a pointer an output gets
#else
constexpr uintptr_t kSentinel = 0xDEADBEEFu;  // (the part is x64 only; this keeps the header building)
#endif
constexpr uint32_t kShapeSentinel = 0xFFFFFFFFu;  // a NaN no computation produces

struct Outputs {
    void* p[kPtrOutputs];
    float shape[kShapeBuffer];
    uint32_t flags;  // in before the call, out after it
    uint32_t ret;    // the bone count
    uint8_t written;  // bit i: p[i] was written; kShapeWritten: the shape-deformation weights
};

inline void Prepare(Outputs& o, uint32_t flagsIn) {
    for (void*& p : o.p) p = reinterpret_cast<void*>(kSentinel);
    std::memset(o.shape, 0xFF, sizeof o.shape);
    o.flags = flagsIn;
    o.ret = 0;
    o.written = 0;
}

inline uint32_t Call(PoseFn f, void* self, uint32_t lod, const float* matrix, Outputs& o) {
    return f(self, lod, matrix, &o.p[kQuatsL], &o.p[kQuatsS], &o.p[kMbQuatsL], &o.p[kMbQuatsS], o.shape, &o.flags,
             &o.p[kRemap]);
}

// after the call: which outputs it wrote
inline void Finish(Outputs& o, uint32_t ret) {
    o.ret = ret;
    o.written = 0;
    for (int i = 0; i < kPtrOutputs; ++i)
        if (uintptr_t(o.p[i]) != kSentinel) o.written |= uint8_t(1 << i);
    for (int i = 0; i < kShapeFloats; ++i) {
        uint32_t v;
        std::memcpy(&v, &o.shape[i], sizeof v);
        if (v != kShapeSentinel) {
            o.written |= kShapeWritten;
            break;
        }
    }
}

// the replay: what the recorded call wrote, written again
inline uint32_t Apply(const Outputs& o, void** quatsL, void** quatsS, void** mbQuatsL, void** mbQuatsS, float* shape,
                      uint32_t* flags, void** remap) {
    void** const out[kPtrOutputs] = {quatsL, quatsS, mbQuatsL, mbQuatsS, remap};
    for (int i = 0; i < kPtrOutputs; ++i)
        if (o.written & (1 << i)) *out[i] = o.p[i];
    if (o.written & kShapeWritten) std::memcpy(shape, o.shape, kShapeFloats * sizeof(float));
    *flags = o.flags;
    return o.ret;
}

// Whether two calls answered the same; *what says where they first differ.
inline bool Same(const Outputs& a, const Outputs& b, const char** what) {
    static const char* const kNames[kPtrOutputs] = {"quatsL", "quatsS", "mbQuatsL", "mbQuatsS", "remap"};
    *what = nullptr;
    if (a.ret != b.ret) return *what = "bone count", false;
    if (a.flags != b.flags) return *what = "flags", false;
    if (a.written != b.written) return *what = "outputs written", false;
    for (int i = 0; i < kPtrOutputs; ++i)
        if ((a.written & (1 << i)) && a.p[i] != b.p[i]) return *what = kNames[i], false;
    if ((a.written & kShapeWritten) && std::memcmp(a.shape, b.shape, kShapeFloats * sizeof(float)) != 0)
        return *what = "shape deformation", false;
    return true;
}

// --- copies of what the answers point at ---------------------------------------------------------------------------
// The renderer reads the bones right away through the pointers it gets, by the mesh chunk's bone ids (16-bit): bone
// id's 32 bytes of the current pose (quatsL, or quatsS with spherical skinning) and, in the object motion-blur pass, of
// the previous pose (mbQuatsL / mbQuatsS); with a remap table (a skin attachment's: bytes, a joint of the master
// character's per bone id) the bone remap[id]. A render thread would read them while the main thread animates the
// next frame, so the answers point at copies: per record, every bone the chunks drawn with it read, and the remap
// table's bytes they read.
constexpr size_t kBoneBytes = 32;  // a bone as the renderer reads it: quaternion, translation (QuatT, padded)

struct Extents {
    uint32_t bones = 0;  // the bones read: those below this, of each pose pointer
    uint32_t remap = 0;  // the remap table's bytes read: those below this
};

// What one chunk drawn with the record reads: its n bone ids, through the record's remap table if it has one.
inline void AddChunk(Extents& e, const uint16_t* ids, uint32_t n, const uint8_t* remap) {
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t id = ids[i];
        const uint32_t bone = remap ? remap[id] : id;
        if (remap && id + 1 > e.remap) e.remap = id + 1;
        if (bone + 1 > e.bones) e.bones = bone + 1;
    }
}

// The pose pointers and the remap table a record answers with, copied to the end of arena (each copy 16-byte aligned:
// arena's data is, as operator new's); at[i] is copy i's offset, or kNotCopied. Null outputs aren't copied, nor the
// motion-blur pose unless the call asked for it (flags in 4: the object motion-blur pass, the only reader).
constexpr uint32_t kNotCopied = 0xFFFFFFFFu;
constexpr uint32_t kFlagMotionBlur = 4;
inline bool Reads(int output, uint32_t flagsIn) {
    return (output != kMbQuatsL && output != kMbQuatsS) || (flagsIn & kFlagMotionBlur) != 0;
}
inline size_t CopyOut(const Outputs& o, const Extents& e, uint32_t flagsIn, std::vector<uint8_t>& arena,
                      uint32_t* at) {
    size_t bytes = 0;
    for (int i = 0; i < kPtrOutputs; ++i) {
        at[i] = kNotCopied;
        const size_t n = i == kRemap ? size_t(e.remap) : size_t(e.bones) * kBoneBytes;
        if (!(o.written & (1 << i)) || !o.p[i] || n == 0 || !Reads(i, flagsIn)) continue;
        const size_t start = (arena.size() + 15) & ~size_t(15);
        arena.resize(start + n);
        std::memcpy(arena.data() + start, o.p[i], n);
        at[i] = uint32_t(start);
        bytes += n;
    }
    return bytes;
}

// The answer once the arena is final: the record's outputs, with the copies in place of what they copy.
inline Outputs WithCopies(const Outputs& o, const uint8_t* arena, const uint32_t* at) {
    Outputs a = o;
    for (int i = 0; i < kPtrOutputs; ++i)
        if (at[i] != kNotCopied) a.p[i] = const_cast<uint8_t*>(arena + at[i]);
    return a;
}

// Whether the copies still hold what the pointers of `live` (a vanilla call's outputs, the same pointers as the
// record's) point at.
inline bool SameCopies(const Outputs& live, const Extents& e, const uint8_t* arena, const uint32_t* at) {
    for (int i = 0; i < kPtrOutputs; ++i) {
        if (at[i] == kNotCopied) continue;
        const size_t n = i == kRemap ? size_t(e.remap) : size_t(e.bones) * kBoneBytes;
        if (!(live.written & (1 << i)) || !live.p[i] || std::memcmp(arena + at[i], live.p[i], n) != 0) return false;
    }
    return true;
}

// A call's key: the skinnable, the LOD and the flags it is asked with (user-space x64 pointers fit in 48 bits).
inline uint64_t Key(const void* skinnable, uint32_t lod, uint32_t flags) {
    return uint64_t(uintptr_t(skinnable)) | uint64_t(lod & 0xFF) << 48 | uint64_t(flags & 0xFF) << 56;
}

// key -> value, cleared per flush by a generation count (no allocations once grown)
using KeyIndex = optiperf::KeyIndex;

}  // namespace optiperf::sk
