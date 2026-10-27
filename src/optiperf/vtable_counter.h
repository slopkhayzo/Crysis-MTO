#pragma once
// Counting copy of an object's vtable (x64): used by the reflection part (the renderer's vtable while a reflection's
// traversal runs) and the environment snapshot (the 3D engine's while the flush runs).

#ifdef _WIN64
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "calltrace.h"
#include "scan.h"

namespace optiperf {

// Counts the calls made through an object's vtable while it is active: the object's vtable pointer is swapped for a
// copy whose slots go through counting thunks, which count calls from inside the module and from elsewhere apart; a
// slot's thunk can be pointed at a function of ours instead of the original, or made to set a marker first (calls
// that make a cached answer out of date), from any thread or from one only (SetMarkThread: the flush's, so that the
// main thread's next frame doesn't spoil the flush's snapshot on a render thread). The swap is one aligned store, so
// other threads calling meanwhile go through the thunks too.
class VtableCounter {
public:
    bool Init(void* object, char* moduleBase, size_t moduleSize, int maxSlots) {
        void** vt = *static_cast<void***>(object);
        int n = 0;
        while (n < maxSlots && IsInCode(moduleBase, static_cast<const char*>(CallTrace_Original(vt[n])), 1)) ++n;
        if (n == 0) return false;
        const size_t code = (size_t(n) * kThunk + 0xFFF) & ~size_t(0xFFF);
        const size_t data = 32 + size_t(n) * 32 + size_t(n + 1) * 8;
        auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, code + data, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!mem) return false;
        // [0] module base, [1] size, [2] the marker, [3] the thread whose calls mark (its low half; 0: any thread's)
        uint64_t* base = reinterpret_cast<uint64_t*>(mem + code);
        base[0] = reinterpret_cast<uint64_t>(moduleBase);
        base[1] = moduleSize;
        base[2] = 0;
        base[3] = 0;
        uint64_t* slot = base + 4;  // per slot: target, calls from inside, calls from outside, target after the marker
        void** copy = reinterpret_cast<void**>(slot + size_t(n) * 4);  // [-1]: the RTTI locator, then the slots
        copy[0] = vt[-1];
        for (int i = 0; i < n; ++i) {
            uint8_t* t = mem + size_t(i) * kThunk;
            uint64_t* s = slot + size_t(i) * 4;
            s[0] = s[3] = reinterpret_cast<uint64_t>(vt[i]);
            static const uint8_t kCode[81] = {
                0x48, 0x8B, 0x04, 0x24,                    // mov rax, [rsp]: the return address
                0x48, 0x2B, 0x05, 0, 0, 0, 0,              // sub rax, [rip+base]
                0x48, 0x3B, 0x05, 0, 0, 0, 0,              // cmp rax, [rip+size]
                0x73, 0x0E,                                // jae outside
                0xF0, 0x48, 0xFF, 0x05, 0, 0, 0, 0,        // lock inc qword [rip+inside]
                0xFF, 0x25, 0, 0, 0, 0,                    // jmp [rip+target]
                0xF0, 0x48, 0xFF, 0x05, 0, 0, 0, 0,        // outside: lock inc qword [rip+outside]
                0xFF, 0x25, 0, 0, 0, 0,                    // jmp [rip+target]
                0x8B, 0x05, 0, 0, 0, 0,                    // +48, a marked slot's target: mov eax, [rip+mark thread]
                0x85, 0xC0,                                // test eax, eax
                0x74, 0x0A,                                // jz mark (any thread's call marks)
                0x65, 0x3B, 0x04, 0x25, 0x48, 0, 0, 0,     // cmp eax, gs:[0x48]: the calling thread's id (the TEB's)
                0x75, 0x07,                                // jne on
                0xC6, 0x05, 0, 0, 0, 0, 0x01,              // mark: mov byte [rip+marker], 1
                0xFF, 0x25, 0, 0, 0, 0};                   // on: jmp [rip+target after the marker]
            std::memcpy(t, kCode, sizeof kCode);
            Rel(t, 7, 11, &base[0]);
            Rel(t, 14, 18, &base[1]);
            Rel(t, 24, 28, &s[1]);
            Rel(t, 30, 34, &s[0]);
            Rel(t, 38, 42, &s[2]);
            Rel(t, 44, 48, &s[0]);
            Rel(t, 50, 54, &base[3]);
            Rel(t, 70, 75, &base[2]);
            Rel(t, 77, 81, &s[3]);
            copy[1 + i] = t;
        }
        DWORD old;
        if (!VirtualProtect(mem, code, PAGE_EXECUTE_READ, &old)) return false;
        FlushInstructionCache(GetCurrentProcess(), mem, code);
        object_ = object;
        original_ = vt;
        copy_ = copy + 1;
        slots_ = slot;
        marker_ = reinterpret_cast<volatile uint8_t*>(&base[2]);
        markThread_ = reinterpret_cast<volatile DWORD*>(&base[3]);
        thunks_ = mem;
        n_ = n;
        return true;
    }
    void Begin() {
        if (copy_) *static_cast<void***>(object_) = copy_;
    }
    void End() {
        if (copy_) *static_cast<void***>(object_) = original_;
    }
    int Slots() const { return n_; }
    uint64_t Inside(int i) const { return slots_[size_t(i) * 4 + 1]; }
    uint64_t Outside(int i) const { return slots_[size_t(i) * 4 + 2]; }
    void* Original(int i) const { return original_[i]; }
    void SetTarget(int i, void* fn) { slots_[size_t(i) * 4] = slots_[size_t(i) * 4 + 3] = reinterpret_cast<uint64_t>(fn); }
    // Calls through slot i set the marker before going on to its target (after SetTarget, if any).
    void Mark(int i) { slots_[size_t(i) * 4] = reinterpret_cast<uint64_t>(thunks_ + size_t(i) * kThunk + 48); }
    bool Marked() const { return marker_ && *marker_ != 0; }
    void ClearMarker() {
        if (marker_) *marker_ = 0;
    }
    // Only this thread's calls through marked slots set the marker (0: any thread's); the others are counted only.
    void SetMarkThread(DWORD threadId) {
        if (markThread_) *markThread_ = threadId;
    }

private:
    static constexpr size_t kThunk = 96;
    static void Rel(uint8_t* t, int field, int next, const void* target) {
        const int32_t d = int32_t(static_cast<const uint8_t*>(target) - (t + next));
        std::memcpy(t + field, &d, 4);
    }
    void* object_ = nullptr;
    void** original_ = nullptr;
    void** copy_ = nullptr;
    uint64_t* slots_ = nullptr;
    volatile uint8_t* marker_ = nullptr;
    volatile DWORD* markThread_ = nullptr;
    uint8_t* thunks_ = nullptr;
    int n_ = 0;
};

}  // namespace optiperf
#endif  // _WIN64
