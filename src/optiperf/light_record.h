#pragma once
// Copies of the 3D engine's dynamic lights (CDLight) and their shadow frustums (ShadowMapFrustum) for the flush
// (light_snapshot.cpp). No engine calls here, so tests/shadow_prepass_test.cpp can drive it with synthetic data.
//
// The lights. EF_ADDDlight appends a light to its recursion level's list (TArray<CDLight*>, renderer +0xF40 + level x
// 16) and gives it its index there (m_Id, the light's first field). For a light with a shader it also makes render
// objects for its coronas and flares (m_pObject[level][i], +0x118), whose m_pLight points back at it (CRenderObject
// +0x128, a union with m_pLightImage, m_pCustomCamera, m_fBendScale and m_nVisionParams, so only an exact match is a
// light). A "fake" light (flares, beams) gets the objects but no place in the list (m_Id -1). The 3D engine keeps its
// frame's dynamic lights by value in an array it reallocates and shifts (C3DEngine::PrepareLightSourcesForRendering):
// the next frame's traversal moves and rewrites what this frame's flush reads.
//
// The frustums. A light that casts shadows has up to 8 (m_pShadowMapFrustums, +0x1F0: a null-terminated array its
// light entity owns). Every frame the 3D engine points the light at that array and writes the light's index into each
// frustum (C3DEngine::InitShadowFrustums, +0x368), and the shadow update rewrites their cameras and caster lists
// (+0x110, PodArray<IShadowCaster*>*). A render object keeps the list of the frustums that shadow it (+0xE8,
// PodArray<ShadowMapFrustum*>*, lists the 3D engine pools and empties every frame).
//
// Right before the flush every light given to the renderer this frame is copied, and so is every frustum they or the
// drawn objects point at: each twice, the copy the flush works on and a pristine one. The arrays and lists between them
// (a light's frustums, a frustum's casters, an object's frustums) are copied into an arena with their pointers mapped,
// and the copies point at those; so do the pristine copies, so that these pointers don't count as the flush's writes.
// The renderer's pointers are pointed at the copies. After the flush the bytes it wrote into a copy (where it differs
// from the pristine one) are written into the original, but in a word the original changed meanwhile (the next frame,
// on a render thread), and the pointers are put back.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "key_index.h"

namespace optiperf::lt {

// CDLight (x64)
constexpr size_t kLightBytes = 0x258;  // the renderer's constructor clears 600 bytes
constexpr int kLObjects = 0x118;       // CRenderObject* m_pObject[4][4]: per recursion level, its coronas' objects
constexpr int kLevels = 4, kObjectsPerLevel = 4;
constexpr int kLFrustums = 0x1F0;      // ShadowMapFrustum** m_pShadowMapFrustums: up to 8, null-terminated...
constexpr int kLightFrustums = 8;
constexpr int kLFlags = 0x1D4;           // ... read only with DLF_CASTSHADOW_MAPS in m_Flags
constexpr uint32_t kCastShadowMaps = 0x10;
// ShadowMapFrustum (x64)
constexpr size_t kFrustumBytes = 0x388;  // the 3D engine allocates and clears 0x388 bytes
constexpr int kFCasters = 0x110;         // PodArray<IShadowCaster*>*
// CRenderObject (x64)
constexpr size_t kObjectBytes = 0x130;
constexpr int kOFrustums = 0xE8;  // m_pShadowCasters: PodArray<ShadowMapFrustum*>*, the frustums that shadow it
constexpr int kOLight = 0x128;    // m_pLight (the union above)
// the renderer
constexpr int kRLights = 0xF40;  // TArray<CDLight*> per recursion level, 16 bytes each

constexpr uint32_t kNotCopied = 0xFFFFFFFFu;
constexpr size_t kMaxWords = kFrustumBytes / 4;

struct PtrList {  // TArray<T*> and PodArray<T*>: elements, count, capacity
    char** elems;
    uint32_t count, capacity;
};

inline char* Get(const char* p) {
    char* v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
inline void Set(char* p, char* v) { std::memcpy(p, &v, sizeof v); }

// Per 4-byte word of the objects compared: in how many of them it differed.
struct Diff {
    size_t objects = 0, words = 0;
    uint32_t perWord[kMaxWords] = {};
};

// A bump arena for one flush: allocated once (what the renderer is pointed at must not move), emptied after it. Its
// memory is the caller's (bytes + 16 of it) or its own.
class Arena {
public:
    void Init(size_t bytes, char* memory = nullptr) {
        if (!memory) {
            mem_.assign(bytes + 16, 0);
            memory = reinterpret_cast<char*>(mem_.data());
        }
        char* const a = memory;
        base_ = a + (16 - reinterpret_cast<uintptr_t>(a) % 16) % 16;
        size_ = bytes;
        used_ = 0;
    }
    void Reset() { used_ = 0; }
    char* Alloc(size_t n) {  // 16-aligned; null when full
        const size_t at = (used_ + 15) & ~size_t(15);
        if (at + n > size_) return nullptr;
        used_ = at + n;
        return base_ + at;
    }
    bool Contains(const char* p) const { return p >= base_ && p < base_ + used_; }
    size_t Used() const { return used_; }

private:
    std::vector<uint8_t> mem_;
    char* base_ = nullptr;
    size_t size_ = 0, used_ = 0;
};

// Objects of one size, copied for one flush.
class Copies {
public:
    // The arena holds `capacity` objects of `bytes` each, allocated once: the caller's memory (ArenaBytes of it), or
    // its own.
    static size_t ArenaBytes(size_t capacity, size_t bytes) {
        return 2 * capacity * ((bytes + 15) & ~size_t(15)) + 16;
    }
    void Init(size_t capacity, size_t bytes, char* memory = nullptr) {
        cap_ = capacity;
        bytes_ = bytes;
        stride_ = (bytes + 15) & ~size_t(15);
        if (!memory) {
            arena_.assign(ArenaBytes(capacity, bytes), 0);
            memory = reinterpret_cast<char*>(arena_.data());
        }
        char* const a = memory;
        base_ = a + (16 - reinterpret_cast<uintptr_t>(a) % 16) % 16;
        slots_.assign(capacity, kNotCopied);
        Clear();
    }
    size_t Capacity() const { return cap_; }
    size_t Bytes() const { return bytes_; }
    // (FlushThread=overlap: the originals and the holders of redirected pointers pinned when recorded, and skipped
    // after the flush if their block was freed meanwhile: allocator.h's pins)
    void SetPins(bool (*pin)(const void* p), bool (*dying)(const void* p)) {
        pin_ = pin;
        dying_ = dying;
    }

    // After a flush (and before the first): nothing recorded or copied.
    void Clear() {
        entries_.clear();
        index_.Clear();
        lists_.clear();
        fields_.clear();
        copied_ = 0;
    }

    // An object to copy, with tag bits (a light: the recursion levels it was added at); the same object twice is one.
    void Add(char* object, uint32_t tags = 0) {
        if (uint32_t* i = index_.Find(Key(object))) {
            entries_[*i].tags |= tags;
            return;
        }
        index_.Insert(Key(object), uint32_t(entries_.size()));
        entries_.push_back({object, tags, kNotCopied, kNoField, nullptr});
    }
    size_t Recorded() const { return entries_.size(); }
    bool IsRecorded(const char* object) const { return index_.Find(Key(object)) != nullptr; }
    char* Object(size_t i) const { return entries_[i].object; }
    uint32_t Tags(size_t i) const { return entries_[i].tags; }

    // Copies every recorded object, as many as the arena holds; returns how many it copied.
    size_t CopyAll() {
        copied_ = 0;
        for (size_t i = 0; i < entries_.size(); ++i) {
            Entry& e = entries_[i];
            if (copied_ >= cap_) {
                e.at = kNotCopied;
                continue;
            }
            e.at = uint32_t(copied_++);
            slots_[e.at] = uint32_t(i);
            std::memcpy(CopyOf(e), e.object, bytes_);
            std::memcpy(PristineOf(e), e.object, bytes_);
            if (pin_) pin_(e.object);
        }
        return copied_;
    }
    size_t Copied() const { return copied_; }
    // The copy of entry i, or null if it wasn't copied.
    char* Copy(size_t i) const { return entries_[i].at == kNotCopied ? nullptr : CopyOf(entries_[i]); }
    const char* Pristine(size_t i) const { return entries_[i].at == kNotCopied ? nullptr : PristineOf(entries_[i]); }
    // A pointer field of copy i set in the copy and its pristine copy: a pointer the copies need, not the flush's write.
    // (One per object; the original's value is kept, to tell a change of the original from it.)
    void SetField(size_t i, size_t offset, char* v) {
        Entry& e = entries_[i];
        if (e.at == kNotCopied || offset + sizeof(char*) > bytes_) return;
        if (e.field == kNoField) {
            e.field = uint32_t(offset);
            e.fieldWas = Get(PristineOf(e) + offset);
        }
        Set(CopyOf(e) + offset, v);
        Set(PristineOf(e) + offset, v);
    }

    // The copy of a recorded object (its exact address), else p itself.
    char* Map(char* p) const {
        const uint32_t* i = index_.Find(Key(p));
        return i && entries_[*i].at != kNotCopied ? CopyOf(entries_[*i]) : p;
    }
    // Whether p points into a copy (not a pristine one).
    bool InCopies(const char* p) const { return p >= base_ && p < base_ + copied_ * stride_; }
    // A pointer into a copy: the same place in its object; else p itself.
    char* Unmap(char* p) const {
        if (!InCopies(p)) return p;
        const size_t at = size_t(p - base_);
        const size_t slot = at / stride_, offset = at % stride_;
        return offset < bytes_ ? entries_[slots_[slot]].object + offset : p;
    }

    // A list the flush reads: its entries pointed at the copies (put back by Restore). Returns how many it changed.
    int RedirectList(PtrList* list) {
        int n = 0;
        for (uint32_t i = 0; i < list->count; ++i) {
            char* m = Map(list->elems[i]);
            if (m != list->elems[i]) {
                list->elems[i] = m;
                ++n;
            }
        }
        if (n) {
            lists_.push_back(list);
            if (pin_) {
                pin_(list);
                pin_(list->elems);
            }
        }
        return n;
    }
    // Pointers that are used up by this flush (the reflections' captured lights): pointed at the copies, not put back.
    int RedirectPointers(void** p, size_t count) {
        int n = 0;
        for (size_t i = 0; i < count; ++i) {
            char* m = Map(static_cast<char*>(p[i]));
            if (m != p[i]) {
                p[i] = m;
                ++n;
            }
        }
        return n;
    }
    // A field holding a recorded object (a render object's m_pLight): pointed at the copy (put back by Restore).
    bool RedirectField(char* field) {
        char* const v = Get(field);
        char* const m = Map(v);
        if (m == v) return false;
        Set(field, m);
        fields_.push_back(field);
        if (pin_) pin_(field);
        return true;
    }
    size_t FieldsRedirected() const { return fields_.size(); }

    // After the flush (verify, before WriteBack): the words the flush wrote into the copies, and the words of the
    // originals that changed while it ran (none should: the flush works on the copies).
    void Compare(Diff& written, Diff& changed) const {
        std::vector<char> was(bytes_);
        for (const Entry& e : entries_) {
            if (e.at == kNotCopied || (dying_ && dying_(e.object))) continue;
            AddDiff(written, CopyOf(e), PristineOf(e));
            // the original as it was: the pristine copy, but for the field set for the copies
            std::memcpy(was.data(), PristineOf(e), bytes_);
            if (e.field != kNoField) Set(was.data() + e.field, e.fieldWas);
            AddDiff(changed, e.object, was.data());
        }
    }
    // After the flush: the bytes the flush wrote into the copies, written into the originals, a 4-byte word at a time.
    // A word the original changed since the hand-off keeps the original's value: another thread wrote it meanwhile
    // (on a render thread, the next frame), which in vanilla's order comes after the flush's write too. Returns the
    // bytes written; *kept gets the words kept.
    size_t WriteBack(size_t* kept = nullptr) {
        size_t n = 0, k = 0;
        for (const Entry& e : entries_) {
            if (e.at == kNotCopied) continue;
            if (dying_ && dying_(e.object)) {  // (freed beside the flush: its object is gone)
                ++dyingSkipped_;
                continue;
            }
            const char* c = CopyOf(e);
            const char* p = PristineOf(e);
            for (size_t w = 0; w < bytes_; w += 4) {
                const size_t len = (bytes_ - w < 4) ? bytes_ - w : 4;
                if (std::memcmp(c + w, p + w, len) == 0) continue;
                // the original's word at the hand-off: the pristine copy's, but in the field set for the copies
                char was[4];
                std::memcpy(was, p + w, len);
                if (e.field != kNoField && w + len > e.field && w < size_t(e.field) + sizeof(char*))
                    for (size_t b = 0; b < len; ++b)
                        if (w + b >= e.field && w + b < size_t(e.field) + sizeof(char*))
                            was[b] = reinterpret_cast<const char*>(&e.fieldWas)[w + b - e.field];
                if (std::memcmp(e.object + w, was, len) != 0) {
                    ++k;
                    continue;
                }
                for (size_t b = w; b < w + len; ++b)
                    if (c[b] != p[b]) {
                        e.object[b] = c[b];
                        ++n;
                    }
            }
        }
        if (kept) *kept = k;
        return n;
    }
    // After the flush: the lists' entries and the fields back to the originals.
    void Restore() {
        for (PtrList* l : lists_) {
            if (dying_ && (dying_(l) || dying_(l->elems))) {
                ++dyingSkipped_;
                continue;
            }
            for (uint32_t i = 0; i < l->count; ++i) l->elems[i] = Unmap(l->elems[i]);
        }
        for (char* f : fields_) {
            if (dying_ && dying_(f)) {
                ++dyingSkipped_;
                continue;
            }
            Set(f, Unmap(Get(f)));
        }
        lists_.clear();
        fields_.clear();
    }
    // the originals and holders skipped after the flush, their blocks freed meanwhile (all flushes)
    size_t DyingSkipped() const { return dyingSkipped_; }
    // Every 8-byte-aligned pointer into a copy in [p, p + bytes), put back to its object. Returns how many.
    size_t UnmapRange(char* p, size_t bytes) {
        size_t n = 0;
        for (size_t o = 0; o + 8 <= bytes; o += 8) {
            char* v = Get(p + o);
            if (InCopies(v)) {
                Set(p + o, Unmap(v));
                ++n;
            }
        }
        return n;
    }

private:
    static constexpr uint32_t kNoField = 0xFFFFFFFFu;
    struct Entry {
        char* object;
        uint32_t tags;
        uint32_t at;        // its slot in the arena, or kNotCopied
        uint32_t field;     // the field SetField set, or kNoField
        char* fieldWas;     // its value in the original
    };
    static uint64_t Key(const void* p) { return uint64_t(reinterpret_cast<uintptr_t>(p)); }
    char* CopyOf(const Entry& e) const { return base_ + size_t(e.at) * stride_; }
    char* PristineOf(const Entry& e) const { return base_ + (cap_ + size_t(e.at)) * stride_; }
    void AddDiff(Diff& d, const char* a, const char* b) const {
        bool any = false;
        for (size_t w = 0; w < bytes_ / 4 && w < kMaxWords; ++w)
            if (std::memcmp(a + w * 4, b + w * 4, 4) != 0) {
                ++d.perWord[w];
                ++d.words;
                any = true;
            }
        if (any) ++d.objects;
    }

    std::vector<Entry> entries_;
    KeyIndex index_;  // object -> entry
    std::vector<uint8_t> arena_;
    char* base_ = nullptr;
    size_t cap_ = 0, bytes_ = 0, stride_ = 0, copied_ = 0;
    std::vector<uint32_t> slots_;  // arena slot -> entry
    std::vector<PtrList*> lists_;
    std::vector<char*> fields_;
    bool (*pin_)(const void*) = nullptr;
    bool (*dying_)(const void*) = nullptr;
    size_t dyingSkipped_ = 0;
};

// A copy in the arena of a pointer list (its 16-byte header, then its elements), its pointers mapped through
// `copies`; its capacity is its count. Null if the arena is full, or the list isn't plausible (no elements with a
// count, or more than maxCount).
inline PtrList* CopyList(const PtrList* list, const Copies& copies, Arena& arena, uint32_t maxCount) {
    if (!list || list->count > maxCount || (list->count && !list->elems)) return nullptr;
    char* const at = arena.Alloc(sizeof(PtrList) + size_t(list->count) * sizeof(char*));
    if (!at) return nullptr;
    auto* copy = reinterpret_cast<PtrList*>(at);
    copy->elems = reinterpret_cast<char**>(at + sizeof(PtrList));
    copy->count = copy->capacity = list->count;
    for (uint32_t i = 0; i < list->count; ++i) copy->elems[i] = copies.Map(list->elems[i]);
    return copy;
}

}  // namespace optiperf::lt
