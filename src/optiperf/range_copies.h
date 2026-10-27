#pragma once
// Copies of ranges of the engine's memory for one flush (engine_data.cpp). A range is any [start, start + bytes): a
// whole object, or a structure inside one (the sky's parameters inside its manager). Each is copied twice into an
// arena: the copy the flush reads, and a pristine twin, which tells what the flush wrote from what the copies were
// given. A pointer to any byte of a copied range maps to the same byte of its copy. No engine calls here (the caller
// reads the engine's memory safely and hands over the bytes), so tests/shadow_prepass_test.cpp can drive it with
// synthetic data.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace optiperf::rc {

inline char* Get(const char* p) {
    char* v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
inline void Set(char* p, char* v) { std::memcpy(p, &v, sizeof v); }

constexpr int kKinds = 8;  // the caller's kinds of range, for the comparison's counts

// After a flush, per kind of range: how many ranges, and bytes in all, differed.
struct Diff {
    uint64_t ranges[kKinds] = {}, bytes[kKinds] = {};
};

class RangeCopies {
public:
    static size_t ArenaBytes(size_t bytes) { return bytes + 16; }
    // The arena (copies and twins): the caller's memory (ArenaBytes of it), or its own.
    void Init(size_t bytes, char* memory = nullptr) {
        if (!memory) {
            mem_.assign(ArenaBytes(bytes), 0);
            memory = reinterpret_cast<char*>(mem_.data());
        }
        base_ = memory + (16 - reinterpret_cast<uintptr_t>(memory) % 16) % 16;
        size_ = bytes;
        Clear();
    }

    // After a flush (and before the first): nothing copied.
    void Clear() {
        ranges_.clear();
        byCopy_.clear();
        fields_.clear();
        given_.clear();
        used_ = 0;
        if (++gen_ == 0) {  // (the pages' generations wrapped: cleared for real)
            std::fill(pages_.begin(), pages_.end(), PageSlotEntry{});
            gen_ = 1;
        }
        pageCount_ = 0;
        pagesFull_ = false;
    }

    // The copy of p, if [p, p + n) lies inside a copied range; else null.
    char* Find(const char* p, size_t n) const {
        const Range* r = RangeOf(p);
        return r && p + n <= r->start + r->bytes ? base_ + r->at + (p - r->start) : nullptr;
    }
    // [p, p + n) copied, from `bytes` (what the caller read there), as a range of `kind`; a range already copied that
    // holds it is used again. Returns the copy of p, or null: n is 0, the arena is full, or [p, p + n) overlaps a copied
    // range without lying inside it.
    char* Add(const char* p, size_t n, int kind, const char* bytes) {
        if (char* c = Find(p, n)) return c;
        auto it = std::upper_bound(ranges_.begin(), ranges_.end(), p,
                                   [](const char* a, const Range& r) { return a < r.start; });
        if (n == 0 || (it != ranges_.begin() && p < (it - 1)->start + (it - 1)->bytes) ||
            (it != ranges_.end() && p + n > it->start))
            return nullptr;
        const size_t stride = (n + 15) & ~size_t(15);
        if (used_ + 2 * stride > size_) return nullptr;
        const Range r{p, n, used_, stride, kind};
        std::memcpy(base_ + r.at, bytes, n);
        std::memcpy(base_ + r.at + stride, bytes, n);
        used_ += 2 * stride;
        ranges_.insert(it, r);
        byCopy_.push_back(r);  // (in arena order: by the copy's place)
        AddPages(p, n);
        return base_ + r.at;
    }
    size_t Ranges() const { return ranges_.size(); }
    size_t Used() const { return used_; }
    int KindOf(size_t i) const { return ranges_[i].kind; }
    const char* Start(size_t i) const { return ranges_[i].start; }
    size_t Bytes(size_t i) const { return ranges_[i].bytes; }
    char* CopyOf(size_t i) const { return base_ + ranges_[i].at; }

    // The kind of the copied range a byte lies in; else -1.
    int KindAt(const char* p) const {
        const Range* r = RangeOf(p);
        return r ? r->kind : -1;
    }
    // A byte of a copied range: the same byte of its copy; else null.
    char* Map(const char* p) const {
        const Range* r = RangeOf(p);
        return r ? base_ + r->at + (p - r->start) : nullptr;
    }
    // A byte of a copy (not of a twin): the original's; else null.
    char* Unmap(const char* p) const {
        const Range* r = RangeOfCopy(p);
        return r ? const_cast<char*>(r->start) + (p - (base_ + r->at)) : nullptr;
    }
    // Whether p points into the arena's copies (a copy or a twin), or anywhere in its memory (a copy made before the
    // last Clear, overwritten as the arena fills again).
    bool InArena(const char* p) const { return p >= base_ && p < base_ + used_; }
    bool InRegion(const char* p) const { return p >= base_ && p < base_ + size_; }

    // A renderer field holding a pointer into a copied range: pointed at the copy (Restore puts it back). False if it
    // holds none.
    bool RedirectField(char* field) {
        char* const v = Get(field);
        char* const m = Map(v);
        if (!m) return false;
        Set(field, m);
        fields_.push_back({field, v, m});
        return true;
    }
    // As RedirectField, for a field the caller knows to hold `original`, whose copy is `copy`.
    void RedirectFieldTo(char* field, char* original, char* copy) {
        Set(field, copy);
        fields_.push_back({field, original, copy});
    }
    size_t FieldsRedirected() const { return fields_.size(); }

    // A pointer field of a copy set to v, in its twin too: what the copies need (another range's copy, a buffer of the
    // caller's), not a write of the flush's. The original's value is kept for Compare. False if copyField isn't in a
    // copy.
    bool GivePointer(char* copyField, char* v) {
        const Range* r = RangeOfCopy(copyField);
        if (!r || copyField + sizeof(char*) > base_ + r->at + r->bytes) return false;
        const size_t off = size_t(copyField - (base_ + r->at));
        char* const twin = base_ + r->at + r->stride + off;
        given_.push_back({r->start, off, Get(twin)});  // (the twin still holds the original's value)
        Set(copyField, v);
        Set(twin, v);
        return true;
    }
    // The pointer given at a copy's field, else null (for the searches: not a pointer the redirect missed).
    bool Given(const char* copyField) const {
        const Range* r = RangeOfCopy(copyField);
        if (!r) return false;
        const size_t off = size_t(copyField - (base_ + r->at));
        for (const GivenPtr& p : given_)
            if (p.start == r->start && p.offset == off) return true;
        return false;
    }

    // After the flush: what it wrote into the copies (copy against twin), and which originals changed while it ran
    // (original against twin, with the given pointers' original values; none should change: the flush reads the
    // copies). The originals are read directly: the caller knows they are still there. onChanged, if set, hears of
    // each changed original: its range, the first byte that differs, how many do.
    template <class OnChanged>
    void Compare(Diff& written, Diff& changed, OnChanged onChanged) const {
        std::vector<char> was;
        for (size_t i = 0; i < ranges_.size(); ++i) {
            const Range& r = ranges_[i];
            const char* copy = base_ + r.at;
            const char* twin = copy + r.stride;
            AddDiff(written, r.kind, copy, twin, r.bytes);
            was.assign(twin, twin + r.bytes);
            for (const GivenPtr& p : given_)
                if (p.start == r.start) Set(was.data() + p.offset, p.was);
            size_t first = r.bytes;
            const size_t n = AddDiff(changed, r.kind, r.start, was.data(), r.bytes, &first);
            if (n) onChanged(i, first, n);
        }
    }
    void Compare(Diff& written, Diff& changed) const {
        Compare(written, changed, [](size_t, size_t, size_t) {});
    }
    // After the flush: the bytes it wrote into the copies (where a copy differs from its twin), written into the
    // originals; a pointer into a copy among them (an 8-byte-aligned word of the original's) is written as the original
    // it stands for. Returns how many bytes; *translated, if set, gets how many such pointers.
    size_t WriteBack(size_t* translated = nullptr) {
        size_t n = 0, t = 0;
        for (const Range& r : ranges_) {
            const char* copy = base_ + r.at;
            const char* twin = copy + r.stride;
            if (std::memcmp(copy, twin, r.bytes) == 0) continue;  // (most: the flush wrote nothing there)
            char* const o = const_cast<char*>(r.start);
            bool any = false;
            for (size_t b = 0; b < r.bytes; ++b)
                if (copy[b] != twin[b]) {
                    o[b] = copy[b];
                    ++n;
                    any = true;
                }
            if (!any) continue;
            for (size_t b = (8 - reinterpret_cast<uintptr_t>(o) % 8) % 8; b + 8 <= r.bytes; b += 8) {
                if (std::memcmp(copy + b, twin + b, 8) == 0) continue;
                if (char* m = Unmap(Get(o + b))) {
                    Set(o + b, m);
                    ++t;
                }
            }
        }
        if (translated) *translated = t;
        return n;
    }
    // After the flush: the redirected fields back to the originals (one the flush itself pointed elsewhere into a copy
    // is put back to that byte's original; one it changed to something else is left as it is). Returns how many were
    // put back.
    size_t Restore() {
        size_t n = 0;
        for (const Field& f : fields_) {
            char* const v = Get(f.field);
            char* const o = v == f.copy ? f.original : Unmap(v);
            if (!o) continue;
            Set(f.field, o);
            ++n;
        }
        fields_.clear();
        return n;
    }
    // Every 8-byte-aligned pointer into a copy in [p, p + bytes), put back to its original. Returns how many.
    size_t UnmapRange(char* p, size_t bytes) {
        size_t n = 0;
        for (size_t o = 0; o + 8 <= bytes; o += 8)
            if (char* m = Unmap(Get(p + o))) {
                Set(p + o, m);
                ++n;
            }
        return n;
    }

private:
    struct Range {
        const char* start;
        size_t bytes;
        size_t at, stride;  // the copy's place in the arena (its twin follows it), and its rounded size
        int kind;
    };
    struct GivenPtr {
        const char* start;  // its range's
        size_t offset;
        char* was;  // the original's value then
    };
    struct Field {
        char* field;
        char* original;  // what it held
        char* copy;      // what it was pointed at
    };
    // The 4 KB pages the copied ranges lie on, as an open-addressing set cleared by a generation: a pointer on none of
    // them is in no range, without the search (most of the pointers looked up are in none). Half full, it answers yes
    // for every page until the next Clear.
    static constexpr size_t kPageSlots = size_t(1) << 12;
    struct PageSlotEntry {
        uintptr_t page = 0;
        uint64_t gen = 0;  // (its generation; 0: never set)
    };
    static size_t PageSlot(uintptr_t page) { return size_t((page * 0x9E3779B97F4A7C15ull) >> 52); }  // (12 bits)
    void AddPages(const char* p, size_t n) {
        const uintptr_t last = (reinterpret_cast<uintptr_t>(p) + n - 1) >> 12;
        for (uintptr_t page = reinterpret_cast<uintptr_t>(p) >> 12; page <= last && !pagesFull_; ++page) {
            if (2 * pageCount_ >= kPageSlots) {
                pagesFull_ = true;
                return;
            }
            size_t i = PageSlot(page);
            while (pages_[i].gen == gen_ && pages_[i].page != page) i = (i + 1) & (kPageSlots - 1);
            if (pages_[i].gen == gen_) continue;
            pages_[i] = {page, gen_};
            ++pageCount_;
        }
    }
    bool OnAPage(const char* p) const {
        if (pagesFull_) return true;
        const uintptr_t page = reinterpret_cast<uintptr_t>(p) >> 12;
        for (size_t i = PageSlot(page);; i = (i + 1) & (kPageSlots - 1)) {
            const PageSlotEntry& e = pages_[i];
            if (e.gen != gen_) return false;
            if (e.page == page) return true;
        }
    }
    const Range* RangeOf(const char* p) const {
        if (ranges_.empty() || !OnAPage(p)) return nullptr;
        auto it = std::upper_bound(ranges_.begin(), ranges_.end(), p,
                                   [](const char* a, const Range& r) { return a < r.start; });
        if (it == ranges_.begin()) return nullptr;
        const Range& r = *(it - 1);
        return p < r.start + r.bytes ? &r : nullptr;
    }
    const Range* RangeOfCopy(const char* p) const {
        if (!InArena(p)) return nullptr;
        const size_t at = size_t(p - base_);
        auto it = std::upper_bound(byCopy_.begin(), byCopy_.end(), at,
                                   [](size_t a, const Range& r) { return a < r.at; });
        if (it == byCopy_.begin()) return nullptr;
        const Range& r = *(it - 1);
        return at < r.at + r.bytes ? &r : nullptr;  // (past it: the padding, or its twin)
    }
    static size_t AddDiff(Diff& d, int kind, const char* a, const char* b, size_t n, size_t* first = nullptr) {
        size_t bytes = 0;
        for (size_t i = 0; i < n; ++i)
            if (a[i] != b[i]) {
                if (!bytes && first) *first = i;
                ++bytes;
            }
        if (bytes && kind >= 0 && kind < kKinds) {
            ++d.ranges[kind];
            d.bytes[kind] += bytes;
        }
        return bytes;
    }

    std::vector<Range> ranges_;  // by start
    std::vector<Range> byCopy_;  // by the copy's place
    std::vector<Field> fields_;
    std::vector<GivenPtr> given_;
    std::vector<uint8_t> mem_;
    char* base_ = nullptr;
    size_t size_ = 0, used_ = 0;
    std::vector<PageSlotEntry> pages_ = std::vector<PageSlotEntry>(kPageSlots);
    uint64_t gen_ = 1;
    size_t pageCount_ = 0;
    bool pagesFull_ = false;
};

}  // namespace optiperf::rc
