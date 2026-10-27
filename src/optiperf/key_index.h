#pragma once
// A 64-bit key -> 32-bit value index for per-flush records (the skinning and material parts): open addressing, cleared
// by a generation count, so that once grown it doesn't allocate.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace optiperf {

class KeyIndex {
public:
    void Clear() {
        size_ = 0;
        if (++gen_ == 0) {
            for (Slot& x : slots_) x = Slot{};
            gen_ = 1;
        }
    }
    const uint32_t* Find(uint64_t key) const {
        if (slots_.empty()) return nullptr;
        for (size_t h = Hash(key) & (slots_.size() - 1);; h = (h + 1) & (slots_.size() - 1)) {
            const Slot& x = slots_[h];
            if (x.gen != gen_) return nullptr;
            if (x.key == key) return &x.value;
        }
    }
    uint32_t* Find(uint64_t key) { return const_cast<uint32_t*>(static_cast<const KeyIndex&>(*this).Find(key)); }
    void Insert(uint64_t key, uint32_t value) {  // (key not present)
        if ((size_ + 1) * 2 > slots_.size()) Grow();
        Put(key, value);
        ++size_;
    }
    size_t size() const { return size_; }

private:
    struct Slot {
        uint64_t key = 0;
        uint32_t value = 0, gen = 0;
    };
    static size_t Hash(uint64_t key) { return size_t((key >> 4) * 0x9E3779B97F4A7C15ull >> 20); }
    void Put(uint64_t key, uint32_t value) {
        for (size_t h = Hash(key) & (slots_.size() - 1);; h = (h + 1) & (slots_.size() - 1)) {
            Slot& x = slots_[h];
            if (x.gen != gen_) {
                x = Slot{key, value, gen_};
                return;
            }
        }
    }
    void Grow() {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.empty() ? 1024 : old.size() * 2, Slot{});
        for (const Slot& x : old)
            if (x.gen == gen_) Put(x.key, x.value);
    }
    std::vector<Slot> slots_;
    size_t size_ = 0;
    uint32_t gen_ = 1;
};

}  // namespace optiperf
