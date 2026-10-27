#pragma once
// Record and replay of a particle vertex creator's output (particle_prepass.cpp). No engine calls here, so
// tests/shadow_prepass_test.cpp can drive it with a synthetic producer and allocator.
//
// While drawing, the renderer's CREParticle::mfPrepare hands the particle container (IParticleVertexCreator) an
// allocator, CAllocRender, and the container's SetVertices writes its vertices and indices into what the allocator
// grants: Alloc(verts, nVerts, nInds) first commits what was written into the previous grant (verts.nVerts,
// verts.nInds), then grants room for up to the requested counts, at the renderer's batch position verts.base (indices
// are relative to it). The renderer grants min(request, buffer size) for each, drawing the current batch first when
// the request doesn't fit: so the grants depend only on the requests and the buffer sizes, never on the batch state,
// and a recording allocator that grants the same way sees the same calls as the renderer's would.
//
// A record holds those calls, what was written into each grant (vertices as they are, indices relative to base 0),
// and the fill lights the container added meanwhile. Replaying it makes the same calls on the renderer's allocator,
// copies the same bytes into its grants, and adds indices the renderer's base.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace optiperf::pp {

// SRenderVertices (x64), as CAllocRender::Alloc and SetVertices use it
struct RenderVerts {
    int32_t nVerts;      // in: written into the last grant; out: granted
    int32_t nInds;
    uint8_t* verts;      // kVertexSize bytes each
    uint16_t* inds;
    int32_t base;        // the batch's vertex index of verts[0], which indices count from
    int32_t pad;
};
#ifdef _WIN64
static_assert(sizeof(RenderVerts) == 32, "SRenderVertices is 32 bytes on x64");
#endif
constexpr size_t kVertexSize = 0x2C;  // SVertexParticle
constexpr size_t kLightSize = 24;     // the fill light SetVertices passes to EF_ADDFillLight

// The renderer's grant: min(request, buffer size); maxInds is the index buffer's size (3 x its +0x1401C)
inline void Grant(int reqV, int reqI, int maxVerts, int maxInds, int* v, int* i) {
    *v = reqV >= maxVerts ? maxVerts : reqV;
    *i = reqI >= maxInds ? maxInds : reqI;
}

struct AllocCall {
    int32_t writtenV, writtenI;  // what the call committed (written into the previous grant)
    int32_t reqV, reqI;          // what it asked for (0: a commit only)
    int32_t grantV, grantI;
    uint32_t data;               // the grant's place in the arena: grantV vertices, then grantI indices
};

// A growable byte buffer that, unlike std::vector, doesn't zero what it adds (grants are written over anyway).
class Arena {
public:
    void clear() { size_ = 0; }
    size_t size() const { return size_; }
    uint8_t* data() { return p_.get(); }
    const uint8_t* data() const { return p_.get(); }
    void resize(size_t n) {
        if (n > cap_) {
            size_t cap = cap_ ? cap_ : (size_t(1) << 20);
            while (cap < n) cap *= 2;
            std::unique_ptr<uint8_t[]> q(new uint8_t[cap]);
            if (size_) std::memcpy(q.get(), p_.get(), size_);
            p_ = std::move(q);
            cap_ = cap;
        }
        size_ = n;
    }

private:
    std::unique_ptr<uint8_t[]> p_;
    size_t size_ = 0, cap_ = 0;
};

struct Record {
    uint32_t firstCall = 0, calls = 0;    // in Recorder::calls
    uint32_t firstLight = 0, lights = 0;  // in Recorder::lights (kLightSize bytes each)
    uint64_t result = 0;                  // SetVertices' return value
    int32_t maxVerts = 0, maxInds = 0;    // the buffer sizes its grants were made with
};

class Recorder {
public:
    // per frame; maxInds as Grant takes it
    void Reset() {
        calls.clear();
        arena.clear();
        lights.clear();
    }
    void Begin(Record& r, int maxVerts, int maxInds) {
        r = Record{};
        r.firstCall = uint32_t(calls.size());
        r.firstLight = uint32_t(lights.size() / kLightSize);
        r.maxVerts = maxVerts;
        r.maxInds = maxInds;
        current_ = &r;
    }
    // the recording allocator's Alloc
    void Alloc(RenderVerts* rv, int reqV, int reqI) {
        Record& r = *current_;
        AllocCall c{rv->nVerts, rv->nInds, reqV, reqI, 0, 0, 0};
        if (reqV != 0) {
            Grant(reqV, reqI, r.maxVerts, r.maxInds, &c.grantV, &c.grantI);
            const size_t at = (arena.size() + 15) & ~size_t(15);
            const size_t bytes = size_t(c.grantV > 0 ? c.grantV : 0) * kVertexSize + size_t(c.grantI > 0 ? c.grantI : 0) * 2;
            arena.resize(at + bytes);  // (earlier grants are kept as offsets: growing the arena is fine)
            c.data = uint32_t(at);
            rv->verts = arena.data() + at;
            rv->inds = reinterpret_cast<uint16_t*>(arena.data() + at + size_t(c.grantV > 0 ? c.grantV : 0) * kVertexSize);
            rv->base = 0;
            rv->nVerts = c.grantV;
            rv->nInds = c.grantI;
        }
        calls.push_back(c);
        ++r.calls;
    }
    void Light(const void* light) {
        const size_t at = lights.size();
        lights.resize(at + kLightSize);
        std::memcpy(lights.data() + at, light, kLightSize);
        ++current_->lights;
    }
    void End(Record& r, uint64_t result) {
        r.result = result;
        current_ = nullptr;
    }

    // What a grant got written into it: the counts the next call committed (none after the last call).
    void Written(const Record& r, uint32_t k, int* v, int* i) const {
        if (k + 1 < r.calls) {
            *v = calls[r.firstCall + k + 1].writtenV;
            *i = calls[r.firstCall + k + 1].writtenI;
        } else {
            *v = *i = 0;
        }
    }
    const uint8_t* Light(const Record& r, uint32_t k) const { return lights.data() + size_t(r.firstLight + k) * kLightSize; }

    // Whether the call ended with its final commit (Alloc(verts, 0, 0)): SetVertices' normal end, after which it
    // updates the container's per-frame counters (an empty container returns before any call, a failed grant after
    // a request).
    bool Committed(const Record& r) const { return r.calls && calls[r.firstCall + r.calls - 1].reqV == 0; }

    // Replays r's allocator calls on the renderer's: alloc(RenderVerts*, nVerts, nInds). Returns the number of grants
    // that differed from the recorded ones (then only what fits is copied, and the commits are cut to it).
    template <class AllocFn>
    int Replay(const Record& r, AllocFn&& alloc) const {
        RenderVerts rv{};
        int mismatches = 0, fitV = 0, fitI = 0;
        for (uint32_t k = 0; k < r.calls; ++k) {
            const AllocCall& c = calls[r.firstCall + k];
            rv.nVerts = c.writtenV < fitV ? c.writtenV : fitV;
            rv.nInds = c.writtenI < fitI ? c.writtenI : fitI;
            alloc(&rv, c.reqV, c.reqI);
            if (c.reqV == 0) {
                fitV = fitI = 0;
                continue;
            }
            if (rv.nVerts != c.grantV || rv.nInds != c.grantI) ++mismatches;
            int v, i;
            Written(r, k, &v, &i);
            fitV = v < rv.nVerts ? v : rv.nVerts;
            fitI = i < rv.nInds ? i : rv.nInds;
            if (fitV > 0) std::memcpy(rv.verts, arena.data() + c.data, size_t(fitV) * kVertexSize);
            const uint16_t* src =
                reinterpret_cast<const uint16_t*>(arena.data() + c.data + size_t(c.grantV > 0 ? c.grantV : 0) * kVertexSize);
            const uint16_t base = uint16_t(rv.base);
            for (int n = 0; n < fitI; ++n) rv.inds[n] = uint16_t(src[n] + base);
        }
        return mismatches;
    }

    // Whether two records made the same calls, wrote the same bytes and added the same lights; *what says where they
    // first differ ("calls", "vertices", "indices", "lights", "result").
    bool Same(const Record& a, const Recorder& other, const Record& b, const char** what) const {
        *what = nullptr;
        if (a.calls != b.calls) {
            *what = "calls";
            return false;
        }
        for (uint32_t k = 0; k < a.calls; ++k) {
            const AllocCall& x = calls[a.firstCall + k];
            const AllocCall& y = other.calls[b.firstCall + k];
            if (x.writtenV != y.writtenV || x.writtenI != y.writtenI || x.reqV != y.reqV || x.reqI != y.reqI ||
                x.grantV != y.grantV || x.grantI != y.grantI) {
                *what = "calls";
                return false;
            }
            if (x.reqV == 0) continue;
            int v, i;
            Written(a, k, &v, &i);
            if (v > 0 && std::memcmp(arena.data() + x.data, other.arena.data() + y.data, size_t(v) * kVertexSize) != 0) {
                *what = "vertices";
                return false;
            }
            const size_t off = size_t(x.grantV > 0 ? x.grantV : 0) * kVertexSize;
            const size_t offB = size_t(y.grantV > 0 ? y.grantV : 0) * kVertexSize;
            if (i > 0 && std::memcmp(arena.data() + x.data + off, other.arena.data() + y.data + offB, size_t(i) * 2) != 0) {
                *what = "indices";
                return false;
            }
        }
        if (a.lights != b.lights ||
            (a.lights && std::memcmp(Light(a, 0), other.Light(b, 0), size_t(a.lights) * kLightSize) != 0)) {
            *what = "lights";
            return false;
        }
        if (a.result != b.result) {
            *what = "result";
            return false;
        }
        return true;
    }

    size_t ArenaBytes() const { return arena.size(); }

    std::vector<AllocCall> calls;
    Arena arena;
    std::vector<uint8_t> lights;

private:
    Record* current_ = nullptr;
};

}  // namespace optiperf::pp
