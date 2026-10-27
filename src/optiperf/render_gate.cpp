// The renderer's gate: see render_gate.h.
//
// A slot's thunk (33 bytes of 48):
//   mov eax, gs:[0x48]          the calling thread's id (the TEB's, as GetCurrentThreadId reads it)
//   cmp eax, [rip+gated]        the gated thread (0: none, and no thread has id 0)
//   je slow
//   jmp [rip+original]          not gated: straight on, as if called directly
//   slow: mov r11d, index
//   jmp common
// The common slow path saves the argument registers (rcx, rdx, r8, r9, xmm0-3; r11 with the index), calls
// onGated(index), restores them and jumps to the slot's function: the call goes on with its stack arguments where they
// were and returns to its caller. rax, r11 and the flags are scratch in the x64 convention. The slow path has unwind
// information (RtlAddFunctionTable) for an exception in the callback; the thunks need none (they don't move rsp).
// A few thunks are kept spare after the installed ones, for slots added later (RenderGate_AddSlot).
#include "render_gate.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace optiperf {
namespace {

#ifdef _WIN64
constexpr size_t kThunk = 48;
constexpr unsigned kSpare = 8;  // thunks kept for slots added later (RenderGate_AddSlot)
constexpr uint8_t kThunkCode[33] = {
    0x65, 0x8B, 0x04, 0x25, 0x48, 0, 0, 0,  // mov eax, gs:[0x48]
    0x3B, 0x05, 0, 0, 0, 0,                 // cmp eax, [rip+gated]
    0x74, 0x06,                             // je slow
    0xFF, 0x25, 0, 0, 0, 0,                 // jmp [rip+original]
    0x41, 0xBB, 0, 0, 0, 0,                 // slow: mov r11d, index
    0xE9, 0, 0, 0, 0};                      // jmp common
constexpr uint8_t kSlowCode[92] = {
    0x51,                                // push rcx
    0x52,                                // push rdx
    0x41, 0x50,                          // push r8
    0x41, 0x51,                          // push r9
    0x41, 0x53,                          // push r11
    0x48, 0x83, 0xEC, 0x60,              // sub rsp, 0x60: 32 shadow bytes, then xmm0-3 (rsp 16-aligned here)
    0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20,  // movdqu [rsp+0x20], xmm0
    0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,  // movdqu [rsp+0x30], xmm1
    0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,  // movdqu [rsp+0x40], xmm2
    0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,  // movdqu [rsp+0x50], xmm3
    0x44, 0x89, 0xD9,                    // mov ecx, r11d
    0xFF, 0x15, 0, 0, 0, 0,              // call [rip+onGated]
    0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20,  // movdqu xmm0, [rsp+0x20]
    0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,  // movdqu xmm1, [rsp+0x30]
    0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,  // movdqu xmm2, [rsp+0x40]
    0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,  // movdqu xmm3, [rsp+0x50]
    0x48, 0x83, 0xC4, 0x60,              // add rsp, 0x60
    0x41, 0x5B,                          // pop r11
    0x41, 0x59,                          // pop r9
    0x41, 0x58,                          // pop r8
    0x5A,                                // pop rdx
    0x59,                                // pop rcx
    0x48, 0x8D, 0x05, 0, 0, 0, 0,        // lea rax, [rip+originals]
    0x42, 0xFF, 0x24, 0xD8};             // jmp [rax + r11*8]
// UNWIND_INFO of the slow path: version 1, a 12-byte prolog, 6 codes (last instruction first): sub rsp,0x60
// (UWOP_ALLOC_SMALL, (0x60-8)/8), then the pushes (UWOP_PUSH_NONVOL) of r11, r9, r8, rdx, rcx
constexpr uint8_t kUnwind[16] = {0x01, 12, 6, 0, 12, 0xB2, 8, 0xB0, 6, 0x90, 4, 0x80, 2, 0x20, 1, 0x10};

struct Vt {
    const char* name;
    uint32_t offset, first, slots;
    char** vtable;
    uint32_t slot0;  // the vtable's slot of its first thunk (0, or an added slot's)
};

struct State {
    bool installed = false;
    uint8_t* mem = nullptr;    // the thunks, then the slow path; after the code pages, the data
    uint64_t* data = nullptr;  // [0] the gated thread (its low half), [1] onGated, [2...] where the slots' thunks go
    uint8_t* slow = nullptr;
    std::vector<void*> functions;  // the slots' own functions (RenderGate_Original), whatever the thunks go to
    unsigned slots = 0, capacity = 0;
    std::vector<Vt> vts;
};
State g;

void Rel(uint8_t* at, int field, int next, const void* target) {
    const int32_t d = int32_t(static_cast<const uint8_t*>(target) - (at + next));
    std::memcpy(at + field, &d, 4);
}

// thunk i's code, going on to function (the code page writable)
void WriteThunk(unsigned i, char* function) {
    uint8_t* t = g.mem + size_t(i) * kThunk;
    std::memcpy(t, kThunkCode, sizeof kThunkCode);
    g.data[2 + i] = reinterpret_cast<uint64_t>(function);
    Rel(t, 10, 14, &g.data[0]);
    Rel(t, 18, 22, &g.data[2 + i]);
    std::memcpy(t + 24, &i, 4);
    Rel(t, 29, 33, g.slow);
}
#endif

}  // namespace

bool RenderGate_Install(const std::vector<RttiVtable>& vtables, void (*onGated)(unsigned index)) {
#ifdef _WIN64
    if (g.installed || !onGated) return false;
    size_t n = 0;
    for (const RttiVtable& v : vtables) n += v.slots;
    if (n == 0 || n > (1u << 20)) return false;
    const size_t total = n + kSpare;
    const size_t code = (total * kThunk + sizeof kSlowCode + 0xFFF) & ~size_t(0xFFF);
    const size_t data = (2 + total) * 8 + sizeof(RUNTIME_FUNCTION) + sizeof kUnwind;
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, code + data, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!mem) return false;
    uint64_t* d = reinterpret_cast<uint64_t*>(mem + code);
    d[0] = 0;
    d[1] = reinterpret_cast<uint64_t>(onGated);
    uint8_t* const slow = mem + total * kThunk;  // (16-aligned)
    std::memcpy(slow, kSlowCode, sizeof kSlowCode);
    Rel(slow, 41, 45, &d[1]);
    Rel(slow, 84, 88, &d[2]);
    g.mem = mem;
    g.data = d;
    g.slow = slow;
    unsigned i = 0;
    for (const RttiVtable& v : vtables) {
        g.vts.push_back({v.mangled, v.offset, i, v.slots, v.vtable, 0});
        for (uint32_t k = 0; k < v.slots; ++k, ++i) {
            WriteThunk(i, v.vtable[k]);
            g.functions.push_back(v.vtable[k]);
        }
    }
    auto* rf = reinterpret_cast<RUNTIME_FUNCTION*>(d + 2 + total);
    uint8_t* const unwind = reinterpret_cast<uint8_t*>(rf + 1);  // (4-aligned)
    std::memcpy(unwind, kUnwind, sizeof kUnwind);
    rf->BeginAddress = DWORD(slow - mem);
    rf->EndAddress = DWORD(slow - mem + sizeof kSlowCode);
    rf->UnwindData = DWORD(unwind - mem);
    DWORD old;
    if (!VirtualProtect(mem, code, PAGE_EXECUTE_READ, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), mem, code);
    RtlAddFunctionTable(rf, 1, DWORD64(uintptr_t(mem)));
    g.slots = unsigned(n);
    g.capacity = unsigned(total);
    MemoryBarrier();
    // the vtables at the thunks (each entry an aligned pointer store: a caller sees the old or the new one)
    i = 0;
    for (const RttiVtable& v : vtables) {
        DWORD prot;
        if (VirtualProtect(v.vtable, v.slots * sizeof(void*), PAGE_READWRITE, &prot)) {
            for (uint32_t k = 0; k < v.slots; ++k) v.vtable[k] = reinterpret_cast<char*>(mem + size_t(i + k) * kThunk);
            VirtualProtect(v.vtable, v.slots * sizeof(void*), prot, &prot);
        }
        i += v.slots;
    }
    g.installed = true;
    return true;
#else
    (void)vtables;
    (void)onGated;
    return false;
#endif
}

void RenderGate_Set(unsigned long threadId) {
#ifdef _WIN64
    if (g.data) *reinterpret_cast<volatile DWORD*>(g.data) = DWORD(threadId);
#else
    (void)threadId;
#endif
}

void* RenderGate_Original(void* entry) {
#ifdef _WIN64
    const auto* p = static_cast<const uint8_t*>(entry);
    if (!g.mem || p < g.mem || p >= g.mem + size_t(g.slots) * kThunk || size_t(p - g.mem) % kThunk) return entry;
    return g.functions[size_t(p - g.mem) / kThunk];
#else
    return entry;
#endif
}

void* RenderGate_Redirect(unsigned index, void* fn) {
#ifdef _WIN64
    if (!g.data || index >= g.slots || !fn) return nullptr;
    reinterpret_cast<void* volatile*>(g.data + 2)[index] = fn;  // (one aligned store: a call goes one way or the other)
    return g.functions[index];
#else
    (void)index;
    (void)fn;
    return nullptr;
#endif
}

void* RenderGate_Target(unsigned index) {
#ifdef _WIN64
    if (!g.data || index >= g.slots) return nullptr;
    return reinterpret_cast<void* const volatile*>(g.data + 2)[index];
#else
    (void)index;
    return nullptr;
#endif
}

int RenderGate_AddSlot(char** vtable, unsigned slot, const char* mangled) {
#ifdef _WIN64
    if (!g.installed || !vtable || g.slots >= g.capacity) return -1;
    if (RenderGate_IndexOf(vtable, slot) >= 0) return RenderGate_IndexOf(vtable, slot);
    const unsigned i = g.slots;
    uint8_t* const t = g.mem + size_t(i) * kThunk;
    DWORD old;
    // (the page stays executable: other threads may be running the thunks before this one on it)
    if (!VirtualProtect(t, kThunk, PAGE_EXECUTE_READWRITE, &old)) return -1;
    char* const function = vtable[slot];
    WriteThunk(i, function);
    VirtualProtect(t, kThunk, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, kThunk);
    g.functions.push_back(function);
    g.vts.push_back({mangled, 0, i, 1, vtable, slot});
    MemoryBarrier();
    g.slots = i + 1;
    DWORD prot;
    if (VirtualProtect(vtable + slot, sizeof(void*), PAGE_READWRITE, &prot)) {
        vtable[slot] = reinterpret_cast<char*>(t);  // (one aligned store: a caller sees the old or the new entry)
        VirtualProtect(vtable + slot, sizeof(void*), prot, &prot);
    }
    return int(i);
#else
    (void)vtable;
    (void)slot;
    (void)mangled;
    return -1;
#endif
}

int RenderGate_IndexOf(char** vtable, unsigned slot) {
#ifdef _WIN64
    for (const Vt& v : g.vts)
        if (v.vtable == vtable && slot >= v.slot0 && slot < v.slot0 + v.slots) return int(v.first + slot - v.slot0);
#else
    (void)vtable;
    (void)slot;
#endif
    return -1;
}

unsigned RenderGate_Slots() {
#ifdef _WIN64
    return g.slots;
#else
    return 0;
#endif
}

unsigned RenderGate_Vtables() {
#ifdef _WIN64
    return unsigned(g.vts.size());
#else
    return 0;
#endif
}

void RenderGate_Describe(unsigned index, char* buffer, unsigned size) {
    snprintf(buffer, size, "slot %u", index);
#ifdef _WIN64
    for (const Vt& v : g.vts) {
        if (index < v.first || index >= v.first + v.slots) continue;
        // ".?AVCD3D9Renderer@@" -> "CD3D9Renderer"
        const char* name = v.name && std::strlen(v.name) > 4 ? v.name + 4 : "?";
        const char* end = std::strstr(name, "@@");
        const int len = end ? int(end - name) : int(std::strlen(name));
        if (v.offset)
            snprintf(buffer, size, "%.*s (+0x%X) slot %u", len, name, v.offset, v.slot0 + index - v.first);
        else
            snprintf(buffer, size, "%.*s slot %u", len, name, v.slot0 + index - v.first);
        return;
    }
#endif
}

}  // namespace optiperf
