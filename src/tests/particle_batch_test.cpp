// Differential test of the particle batch fix against the game's own code (x86 and x64).
//
// The renderer DLL is mapped twice without running it (DONT_RESOLVE_DLL_REFERENCES): one copy is patched by optiperf
// exactly as in the game, the other keeps the original function. Both are driven through fake renderer objects with
// the same sequence of synthetic CRenderObjects — built so that merges, repeated submissions, NaN, +-0 and denormals
// (with DAZ on and off) all occur — and must return the same objects and leave identical lists behind.
// The original code only reaches the rest of the engine when a list has to grow; lists are pre-sized so it never does.
//
// usage: particle_batch_test.exe ["<Crysis>\Bin32" | "<Crysis>\Bin64"]   (match the test's own architecture)

#include <windows.h>
#include <intrin.h>
#include <xmmintrin.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

#include "../optiperf/particle_batch.h"
#include "../optiperf/particle_batch_index.h"

namespace {

using namespace optiperf::pb;

struct PodArrayPtr {
    char** elems;
    int count;
    int capacity;
};

#ifdef _WIN64
using MergeFn = char* (*)(char* self, char* obj);
char* Call(MergeFn f, char* self, char* obj) { return f(self, obj); }
constexpr const wchar_t* kDefaultDir = L"D:\\Program Files\\Games\\GOG\\Crysis\\Bin64";
constexpr int kProbe = 0x77;  // the 0xCE of "cmp word ptr [rcx+0CEh], dx"
#else
using MergeFn = char*(__fastcall*)(char* self, void* edx, char* obj);
char* Call(MergeFn f, char* self, char* obj) { return f(self, nullptr, obj); }
constexpr const wchar_t* kDefaultDir = L"D:\\Program Files\\Games\\GOG\\Crysis\\Bin32";
constexpr int kProbe = 0x5D;  // the 0xBE of "cmp word ptr [ecx+0BEh], ax"
#endif

struct Target {
    const wchar_t* dll;
    uint32_t listOffset, freeListOffset;  // filled from ParticleBatch_Locate
};

constexpr int kCapacity = 1 << 17;
constexpr int kObjSize = 0x100;
static_assert(kObjSize > kLayoutX64.comparedBytes && kObjSize > kLayoutX86.comparedBytes, "objects must cover all fields");

struct FakeRenderer {
    std::vector<char> mem = std::vector<char>(0x20000);
    std::vector<char*> list = std::vector<char*>(kCapacity), freeList = std::vector<char*>(kCapacity);
    uint32_t listOffset, freeListOffset;
    FakeRenderer(uint32_t l, uint32_t f) : listOffset(l), freeListOffset(f) {
        *List() = {list.data(), 0, kCapacity};
        *Free() = {freeList.data(), 0, kCapacity};
    }
    PodArrayPtr* List() { return reinterpret_cast<PodArrayPtr*>(mem.data() + listOffset); }
    PodArrayPtr* Free() { return reinterpret_cast<PodArrayPtr*>(mem.data() + freeListOffset); }
    // what the renderer does before a pass; the free list is drained by EF_GetObject in the game, emptied here
    void StartPass() {
        List()->count = 0;
        Free()->count = 0;
    }
};

bool SameArrays(FakeRenderer& a, FakeRenderer& b) {
    return a.List()->count == b.List()->count && a.Free()->count == b.Free()->count &&
           std::memcmp(a.list.data(), b.list.data(), sizeof(char*) * size_t(a.List()->count)) == 0 &&
           std::memcmp(a.freeList.data(), b.freeList.data(), sizeof(char*) * size_t(a.Free()->count)) == 0;
}

uint32_t PickFloat(std::mt19937& r) {
    static const uint32_t common[] = {0x00000000u, 0x80000000u, 0x3F800000u, 0x3F000000u};  // +0, -0, 1, 0.5
    static const uint32_t rare[] = {0x00000001u, 0x80000003u, 0x007FFFFFu, 0x7FC00000u, 0x7F800000u, 0xFF800001u};
    // denormals, NaN, +inf, -NaN
    return (r() % 50) ? common[r() % 4] : rare[r() % 6];
}

void Fill(char* o, std::mt19937& r, unsigned variety) {
    const Layout& L = kLayout;
    for (int i = 0; i < kObjSize; ++i) o[i] = char(r());  // non-compared bytes vary freely
    auto put16 = [&](int off, uint32_t v) { const uint16_t x = uint16_t(v); std::memcpy(o + off, &x, 2); };
    auto put32 = [&](int off, uint32_t v) { std::memcpy(o + off, &v, 4); };
    put16(L.w0, r() % variety);
    put16(L.w1, r() % 2);
    put32(L.d0, (r() % 2) * 0x100);
    put32(L.d1, (r() % 2) * 0x20000);
    put32(L.f0, PickFloat(r));
    // random flag word, but only a few combinations of the compared bits
    static const uint32_t maskBits[] = {0, 0x20, 0x8000, 0xC020};
    put32(L.flags, (uint32_t(r()) & ~kFlagsMask) | maskBits[r() % 4]);
    put16(L.w2, r() % 2);
    for (int i = 0; i < 4; ++i) put32(L.color + 4 * i, PickFloat(r));
}

struct CaseStats {
    uint64_t calls = 0, merged = 0, appended = 0, present = 0, failures = 0;
};

// Runs one randomized scenario through both functions, adding divergences to s.failures.
void RunCase(MergeFn original, MergeFn patched, const Target& t, uint32_t seed, bool daz, unsigned variety, CaseStats& s) {
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(daz ? (csr | 0x40u) : (csr & ~0x40u));
    std::mt19937 r(seed);
    std::vector<std::vector<char>> pool(3000, std::vector<char>(kObjSize));
    for (auto& o : pool) Fill(o.data(), r, variety);
    FakeRenderer ref(t.listOffset, t.freeListOffset), opt(t.listOffset, t.freeListOffset);

    for (int pass = 0; pass < 200; ++pass) {
        ref.StartPass();
        opt.StartPass();
        for (int k = 0; k < 40; ++k) Fill(pool[r() % pool.size()].data(), r, variety);  // temp objects get reused
        const int calls = int(r() % 1200);
        for (int c = 0; c < calls; ++c) {
            char* obj = (r() % 16 == 0 && opt.List()->count) ? opt.List()->elems[r() % opt.List()->count]  // resubmit
                                                               : pool[r() % pool.size()].data();
            const int refCount = ref.List()->count, refFree = ref.Free()->count;
            char* a = Call(original, ref.mem.data(), obj);
            char* b = Call(patched, opt.mem.data(), obj);
            ++s.calls;
            if (ref.List()->count > refCount) ++s.appended;
            else if (ref.Free()->count > refFree) ++s.merged;
            else ++s.present;
            const bool same = a == b && ref.List()->count == opt.List()->count && ref.Free()->count == opt.Free()->count &&
                              (ref.List()->count == 0 || ref.list[ref.List()->count - 1] == opt.list[opt.List()->count - 1]) &&
                              (ref.Free()->count == 0 || ref.freeList[ref.Free()->count - 1] == opt.freeList[opt.Free()->count - 1]);
            if (!same && ++s.failures <= 10)
                printf("  DIVERGENCE seed=%u pass=%d call=%d: original returned %p (list %d, free %d), patched %p (list %d, free %d)\n",
                       seed, pass, c, static_cast<void*>(a), ref.List()->count, ref.Free()->count, static_cast<void*>(b),
                       opt.List()->count, opt.Free()->count);
        }
        if (!SameArrays(ref, opt) && ++s.failures <= 10) printf("  DIVERGENCE seed=%u pass=%d: list contents differ\n", seed, pass);
    }
    _mm_setcsr(csr);
}

// One pass of `n` all-distinct objects, like the game's particle containers (no merges): cycles per call.
void Benchmark(MergeFn original, MergeFn patched, const Target& t, int n) {
    std::mt19937 r(7);
    std::vector<std::vector<char>> objs(n, std::vector<char>(kObjSize));
    for (int i = 0; i < n; ++i) {
        Fill(objs[i].data(), r, 2);
        std::memcpy(objs[i].data() + kLayout.d0, &i, 4);  // make every object unique
        const float one = 1.0f;
        for (int k = 0; k < 4; ++k) std::memcpy(objs[i].data() + kLayout.color + 4 * k, &one, 4);  // no NaN wildcards
        std::memcpy(objs[i].data() + kLayout.f0, &one, 4);
    }
    FakeRenderer ref(t.listOffset, t.freeListOffset), opt(t.listOffset, t.freeListOffset);
    uint64_t c0 = __rdtsc();
    for (auto& o : objs) Call(original, ref.mem.data(), o.data());
    uint64_t c1 = __rdtsc();
    for (auto& o : objs) Call(patched, opt.mem.data(), o.data());
    uint64_t c2 = __rdtsc();
    printf("  one pass of %d distinct objects: original %.0f cycles/call, patched %.0f cycles/call (%.0fx)\n", n,
           double(c1 - c0) / n, double(c2 - c1) / n, double(c1 - c0) / double(c2 - c1 ? c2 - c1 : 1));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring dir = argc > 1 ? argv[1] : kDefaultDir;
    SetEnvironmentVariableW(L"OPTIPERF_PARTICLE_BATCH", L"fast");
    int failures = 0;
    for (const wchar_t* dllName : {L"CryRenderD3D9.dll", L"CryRenderD3D10.dll"}) {
        Target t{dllName, 0, 0};
        printf("%ls\n", t.dll);
        const std::wstring src = dir + L"\\" + t.dll;
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring refPath = std::wstring(tmp) + L"optiperf_reference_" + t.dll;
        if (!CopyFileW(src.c_str(), refPath.c_str(), FALSE)) {
            printf("  cannot copy %ls (error %lu)\n", src.c_str(), GetLastError());
            ++failures;
            continue;
        }
        HMODULE patchedMod = LoadLibraryExW(src.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        HMODULE refMod = LoadLibraryExW(refPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!patchedMod || !refMod) {
            printf("  cannot map the DLL (error %lu)\n", GetLastError());
            ++failures;
            continue;
        }
        auto refBase = reinterpret_cast<char*>(refMod);
        optiperf::ParticleBatchTarget found;
        const char* why = nullptr;
        if (!optiperf::ParticleBatch_Locate(refBase, &found, &why)) {
            printf("  locating the merge function failed: %s\n", why);
            ++failures;
            continue;
        }
        const size_t mergeRva = size_t(found.merge - refBase);
        t.listOffset = found.listOffset;
        t.freeListOffset = found.freeListOffset;
        printf("  located at +0x%zX (list +0x%X, free list +0x%X)\n", mergeRva, t.listOffset, t.freeListOffset);

        // soft check: one changed byte inside the function must make the patch refuse to install
        {
            char* probe = found.merge + kProbe;
            DWORD old;
            VirtualProtect(probe, 1, PAGE_EXECUTE_READWRITE, &old);
            *probe ^= 0x02;  // a different field offset
            optiperf::ParticleBatchTarget dummy;
            const bool located = optiperf::ParticleBatch_Locate(refBase, &dummy, &why);
            *probe ^= 0x02;
            VirtualProtect(probe, 1, old, &old);
            printf("  altered code: %s\n", located ? "WRONGLY ACCEPTED" : why);
            if (located) ++failures;
        }

        auto base = reinterpret_cast<char*>(patchedMod);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        optiperf::ParticleBatch_ResetForTest();
        optiperf::ParticleBatch_OnModuleLoaded(t.dll, base, nt->OptionalHeader.SizeOfImage);
        if (uint8_t(base[mergeRva]) != 0xE9) {
            printf("  patch was not applied\n");
            ++failures;
            continue;
        }
        auto original = reinterpret_cast<MergeFn>(found.merge);
        auto patched = reinterpret_cast<MergeFn>(base + mergeRva);

        CaseStats s;
        uint32_t seed = 1;
        for (bool daz : {false, true})
            for (unsigned variety : {2u, 3u, 40u})
                for (int i = 0; i < 3; ++i) RunCase(original, patched, t, seed++, daz, variety, s);
        printf("  %llu calls: %llu merged, %llu appended, %llu already present -> %llu divergences\n", s.calls, s.merged,
               s.appended, s.present, s.failures);
        failures += int(s.failures);
        Benchmark(original, patched, t, 2000);
        Benchmark(original, patched, t, 6000);
        FreeLibrary(refMod);
        FreeLibrary(patchedMod);
        DeleteFileW(refPath.c_str());
    }
    printf(failures ? "FAILED\n" : "OK\n");
    return failures ? 1 : 0;
}
