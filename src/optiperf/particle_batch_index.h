#pragma once
// Lookup logic of the particle batch fix, kept free of game/Windows dependencies so tests can drive it directly.
// See particle_batch.cpp for what the patched renderer function does.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace optiperf::pb {

// CRenderObject fields the original merge loop compares, per build. The two builds also differ in how a NaN compares:
// the x86 code checks the parity flag after ucomiss (NaN never equal), the x64 code doesn't (NaN always "equal").
struct Layout {
    int w0, w1;   // two 16-bit fields
    int d0, d1;   // two 32-bit fields
    int f0;       // one float
    int flags;    // 32-bit flag word, only kFlagsMask bits compared
    int w2;       // 16-bit field
    int color;    // ColorF (4 floats)
    bool nanEqual;
    int comparedBytes;  // all compared fields lie below this offset
};
constexpr Layout kLayoutX86{0xBE, 0xC0, 0x88, 0xB0, 0xB4, 0x0C, 0x0A, 0x40, false, 0xC2};
constexpr Layout kLayoutX64{0xCE, 0xD0, 0x8C, 0xC0, 0xC4, 0x10, 0x0E, 0x44, true, 0xD2};
#ifdef _WIN64
constexpr const Layout& kLayout = kLayoutX64;
#else
constexpr const Layout& kLayout = kLayoutX86;
#endif
constexpr uint32_t kFlagsMask = 0xC020;

template <class T>
inline T Rd(const char* p, int off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}

inline bool FloatEq(float a, float b, const Layout& L) { return L.nanEqual ? (a == b || a != a || b != b) : a == b; }

// The original's comparison, field for field.
inline bool SameExceptColor(const char* a, const char* b, const Layout& L) {
    return Rd<uint16_t>(a, L.w0) == Rd<uint16_t>(b, L.w0) && Rd<uint16_t>(a, L.w1) == Rd<uint16_t>(b, L.w1) &&
           Rd<uint32_t>(a, L.d0) == Rd<uint32_t>(b, L.d0) && Rd<uint32_t>(a, L.d1) == Rd<uint32_t>(b, L.d1) &&
           FloatEq(Rd<float>(a, L.f0), Rd<float>(b, L.f0), L) &&
           ((Rd<uint32_t>(a, L.flags) ^ Rd<uint32_t>(b, L.flags)) & kFlagsMask) == 0 &&
           Rd<uint16_t>(a, L.w2) == Rd<uint16_t>(b, L.w2);
}
inline bool SameColor(const char* a, const char* b, const Layout& L) {
    for (int i = 0; i < 4; ++i)
        if (!FloatEq(Rd<float>(a, L.color + 4 * i), Rd<float>(b, L.color + 4 * i), L)) return false;
    return true;
}
inline bool Same(const char* a, const char* b, const Layout& L) { return SameExceptColor(a, b, L) && SameColor(a, b, L); }

struct Key {
    uint32_t w[10];
    bool operator==(const Key& o) const { return std::memcmp(w, o.w, sizeof w) == 0; }
};

// How an object takes part in matching:
//   Exact      - equal objects have equal keys (floats normalised: +0 == -0, denormals == 0 under DAZ)
//   Never      - x86: a NaN in a compared float, so it can never be equal to anything
//   Wildcard   - x64: a NaN in a compared float, which compares equal to any value; needs a linear check
enum class KeyKind { Exact, Never, Wildcard };

inline KeyKind MakeKey(const char* o, bool daz, const Layout& L, Key* k) {
    bool nan = false;
    auto f = [&](int off) {
        uint32_t b = Rd<uint32_t>(o, off);
        const uint32_t e = b & 0x7F800000u, m = b & 0x007FFFFFu;
        if (e == 0x7F800000u && m) nan = true;
        if (e == 0 && (m == 0 || daz)) b = 0;
        return b;
    };
    k->w[0] = Rd<uint16_t>(o, L.w0) | uint32_t(Rd<uint16_t>(o, L.w1)) << 16;
    k->w[1] = Rd<uint32_t>(o, L.d0);
    k->w[2] = Rd<uint32_t>(o, L.d1);
    k->w[3] = f(L.f0);
    k->w[4] = Rd<uint32_t>(o, L.flags) & kFlagsMask;
    k->w[5] = Rd<uint16_t>(o, L.w2);
    for (int i = 0; i < 4; ++i) k->w[6 + i] = f(L.color + 4 * i);
    if (!nan) return KeyKind::Exact;
    return L.nanEqual ? KeyKind::Wildcard : KeyKind::Never;
}

inline uint32_t Mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}
inline uint32_t HashKey(const Key& k) {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t w : k.w) h = Mix(h ^ w) + 0x9E3779B9u;
    return h;
}
inline uint32_t HashPtr(const char* p) {
    const auto v = uint64_t(reinterpret_cast<uintptr_t>(p));
    return Mix(uint32_t(v) * 0x9E3779B1u ^ uint32_t(v >> 32));
}

// Mirrors the renderer's list: positions chained per key in insertion order, wildcard positions in a separate list,
// plus a pointer set. Reset is O(1) through generation stamps.
class Index {
public:
    Index() : buckets_(1024), ptrs_(2048) {}

    void Reset() {
        if (++gen_ == 0) {
            for (auto& b : buckets_) b.gen = 0;
            for (auto& p : ptrs_) p.gen = 0;
            gen_ = 1;
        }
        count_ = keys_ = ptrCount_ = 0;
        wildcards_.clear();
    }
    int Count() const { return count_; }
    const std::vector<int>& Wildcards() const { return wildcards_; }

    // Indexes `obj` as list position Count().
    void Add(const char* obj, const Key& key, KeyKind kind) {
        const int pos = count_++;
        if (pos >= int(next_.size())) next_.resize(size_t(pos) * 2 + 256);
        next_[pos] = -1;
        InsertPtr(obj);
        if (kind == KeyKind::Wildcard) wildcards_.push_back(pos);
        if (kind != KeyKind::Exact) return;
        Bucket* b = FindOrCreateBucket(key, HashKey(key));
        if (b->head < 0) {
            b->head = b->tail = pos;
        } else {
            next_[b->tail] = pos;
            b->tail = pos;
        }
    }

    bool HasPtr(const char* obj) const {
        const uint32_t mask = uint32_t(ptrs_.size()) - 1;
        for (uint32_t i = HashPtr(obj) & mask;; i = (i + 1) & mask) {
            const PtrSlot& s = ptrs_[i];
            if (s.gen != gen_) return false;
            if (s.p == obj) return true;
        }
    }
    int First(const Key& key) const {
        const uint32_t h = HashKey(key), mask = uint32_t(buckets_.size()) - 1;
        for (uint32_t i = h & mask;; i = (i + 1) & mask) {
            const Bucket& b = buckets_[i];
            if (b.gen != gen_) return -1;
            if (b.hash == h && b.key == key) return b.head;
        }
    }
    int Next(int pos) const { return next_[pos]; }

private:
    struct Bucket {
        uint32_t gen = 0, hash = 0;
        Key key{};
        int head = -1, tail = -1;
    };
    struct PtrSlot {
        uint32_t gen = 0;
        const char* p = nullptr;
    };

    Bucket* FindOrCreateBucket(const Key& key, uint32_t h) {
        if ((keys_ + 1) * 2 > int(buckets_.size())) GrowBuckets();
        const uint32_t mask = uint32_t(buckets_.size()) - 1;
        for (uint32_t i = h & mask;; i = (i + 1) & mask) {
            Bucket& b = buckets_[i];
            if (b.gen != gen_) {
                b.gen = gen_;
                b.hash = h;
                b.key = key;
                b.head = b.tail = -1;
                ++keys_;
                return &b;
            }
            if (b.hash == h && b.key == key) return &b;
        }
    }
    void GrowBuckets() {
        std::vector<Bucket> old(buckets_.size() * 2);
        old.swap(buckets_);
        const uint32_t mask = uint32_t(buckets_.size()) - 1;
        for (const Bucket& b : old) {
            if (b.gen != gen_) continue;
            uint32_t i = b.hash & mask;
            while (buckets_[i].gen == gen_) i = (i + 1) & mask;
            buckets_[i] = b;
        }
    }
    void InsertPtr(const char* obj) {
        if ((ptrCount_ + 1) * 2 > int(ptrs_.size())) {
            std::vector<PtrSlot> old(ptrs_.size() * 2);
            old.swap(ptrs_);
            const uint32_t mask = uint32_t(ptrs_.size()) - 1;
            for (const PtrSlot& s : old) {
                if (s.gen != gen_) continue;
                uint32_t i = HashPtr(s.p) & mask;
                while (ptrs_[i].gen == gen_) i = (i + 1) & mask;
                ptrs_[i] = s;
            }
        }
        const uint32_t mask = uint32_t(ptrs_.size()) - 1;
        uint32_t i = HashPtr(obj) & mask;
        while (ptrs_[i].gen == gen_) {
            if (ptrs_[i].p == obj) return;
            i = (i + 1) & mask;
        }
        ptrs_[i].gen = gen_;
        ptrs_[i].p = obj;
        ++ptrCount_;
    }

    std::vector<Bucket> buckets_;
    std::vector<PtrSlot> ptrs_;
    std::vector<int> next_;
    std::vector<int> wildcards_;
    uint32_t gen_ = 1;
    int count_ = 0, keys_ = 0, ptrCount_ = 0;
};

enum class Kind { Present, Match, Append };
struct Result {
    Kind kind;
    int index;  // list position for Match (and for Present in the linear reference); -1 otherwise
};

struct Counters {
    uint64_t resets = 0, catchups = 0, chainSteps = 0, sumListAtReset = 0, wildcardLookups = 0;
};

// Brings the index in line with the list. The renderer only grows the list through the patched function or resets it
// to zero, so a smaller count means "reset"; entries we did not see are indexed as catch-ups (not expected).
inline void Sync(Index& ix, char* const* elems, int count, bool daz, const Layout& L, Counters& c) {
    if (count < ix.Count()) {
        c.sumListAtReset += uint64_t(ix.Count());
        ++c.resets;
        ix.Reset();
    }
    while (ix.Count() < count) {
        const char* o = elems[ix.Count()];
        Key k;
        const KeyKind kind = MakeKey(o, daz, L, &k);
        ix.Add(o, k, kind);
        ++c.catchups;
    }
}

// Hashed version of the original decision (call Sync first). On Append, `key`/`kind` are what Index::Add needs.
inline Result HashedLookup(const Index& ix, char* const* elems, int count, const char* obj, bool daz, const Layout& L,
                           Key* key, KeyKind* kind, Counters& c) {
    *kind = MakeKey(obj, daz, L, key);
    if (ix.HasPtr(obj)) return {Kind::Present, -1};
    if (*kind == KeyKind::Never) return {Kind::Append, -1};
    if (*kind == KeyKind::Wildcard) {  // matches any value in its NaN fields: check every entry, like the original
        ++c.wildcardLookups;
        for (int i = 0; i < count; ++i)
            if (Same(elems[i], obj, L)) return {Kind::Match, i};
        return {Kind::Append, -1};
    }
    int best = -1;
    for (int j = ix.First(*key); j >= 0; j = ix.Next(j)) {
        ++c.chainSteps;
        if (Same(elems[j], obj, L)) {
            best = j;
            break;
        }
    }
    for (int w : ix.Wildcards()) {  // earlier entries with NaNs (x64) can match anything; ascending positions
        if (best >= 0 && w > best) break;
        if (Same(elems[w], obj, L)) {
            best = w;
            break;
        }
    }
    return best >= 0 ? Result{Kind::Match, best} : Result{Kind::Append, -1};
}

// The original algorithm (two linear scans), without side effects. `colorEq(a, b)` compares the 16 bytes of ColorF.
template <class ColorEq>
inline Result LinearLookup(char* const* elems, int count, const char* obj, const Layout& L, ColorEq colorEq) {
    for (int i = 0; i < count; ++i)
        if (elems[i] == obj) return {Kind::Present, i};
    for (int i = 0; i < count; ++i) {
        const char* cand = elems[i];
        if (SameExceptColor(cand, obj, L) && colorEq(cand + L.color, obj + L.color)) return {Kind::Match, i};
    }
    return {Kind::Append, -1};
}

}  // namespace optiperf::pb
