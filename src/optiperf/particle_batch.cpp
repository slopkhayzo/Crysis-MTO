// Fix for the O(N^2) particle batch merge in IRenderer::EF_AddParticlesToScene (CryRenderD3D9/D3D10, x86 and x64).
//
// For every particle container the 3D engine renders, the renderer tries to reuse an earlier CRenderObject that has
// identical batching state. The original does, in order:
//   1. obj already in this pass's list                  -> return obj
//   2. first earlier entry whose compared fields == obj -> push obj onto the temp-object free list, return that entry
//   3. otherwise append obj to the list                  -> return obj
// Both steps scan the whole list, so a pass with N visible containers costs O(N^2): ~22 % of the main thread on the
// ice level on average and ~50 % in its heaviest seconds (research/docs/cpu-multithreading-plan.md, section 3.5).
//
// The replacement keeps the three steps and their side effects (it calls the game's own PodArray::Add for both
// lists) but finds candidates through a hash index keyed on exactly the compared fields (particle_batch_index.h).
//
// Locating: no version or hash checks. The whole original function is matched by a pattern (wildcards only where the
// renderers differ: member offsets and call targets), which also pins down every field it compares. Member offsets are
// read from the matched code and must agree everywhere they appear; the callees must match their own patterns.
//
// [ParticleBatch] Mode = fast (default) | verify | off      (env OPTIPERF_PARTICLE_BATCH overrides)
//   verify: run the original linear search next to the hashed one on every call, use the original's answer, and count
//           disagreements (expected: zero). Costs as much as vanilla; for validation only.

#include "particle_batch.h"

#include <windows.h>
#include <intrin.h>
#include <xmmintrin.h>

#include <cstdint>
#include <cwchar>
#include <initializer_list>
#include <string>

#include "config.h"
#include "hooks.h"
#include "log.h"
#include "particle_batch_index.h"
#include "scan.h"

namespace optiperf {
namespace {

using namespace pb;

#ifdef _WIN64
// x64 build 6156 (CryRenderD3D9.dll and CryRenderD3D10.dll); the ColorF compare is inlined here
constexpr const char kMergePattern[] =
    "48 89 5C 24 18 56 48 83 EC 20 48 85 D2 48 8B DA 48 8B F1 0F 84 93 01 00 00 44 8B 81 ?? ?? ?? ?? "
    "33 C0 45 85 C0 74 1A 48 8B 91 ?? ?? ?? ?? 66 90 48 3B 1A 74 0C 83 C0 01 48 83 C2 08 41 3B C0 72 "
    "EF 41 3B C0 0F 85 54 01 00 00 48 89 7C 24 38 33 FF 45 85 C0 0F 84 D1 00 00 00 0F B7 93 CE 00 00 "
    "00 48 89 6C 24 30 48 8B A9 ?? ?? ?? ?? 66 66 90 48 8B 4D 00 66 39 91 CE 00 00 00 0F 85 80 00 00 "
    "00 0F B7 83 D0 00 00 00 66 39 81 D0 00 00 00 75 70 8B 83 8C 00 00 00 39 81 8C 00 00 00 75 62 8B "
    "83 C0 00 00 00 39 81 C0 00 00 00 75 54 F3 0F 10 83 C4 00 00 00 0F 2E 81 C4 00 00 00 75 43 8B 41 "
    "10 33 43 10 A9 20 C0 00 00 75 36 0F B7 43 0E 66 39 41 0E 75 2C F3 0F 10 43 44 0F 2E 41 44 75 21 "
    "F3 0F 10 43 48 0F 2E 41 48 75 16 F3 0F 10 43 4C 0F 2E 41 4C 75 0B F3 0F 10 43 50 0F 2E 41 50 74 "
    "12 83 C7 01 48 83 C5 08 41 3B F8 0F 82 5F FF FF FF EB 13 48 8D 8E ?? ?? ?? ?? 48 8B D3 E8 ?? ?? "
    "?? ?? 48 8B 5D 00 48 8B 6C 24 30 3B BE ?? ?? ?? ?? 75 58 8B BE ?? ?? ?? ?? 8D 4F 01 3B 8E ?? ?? "
    "?? ?? 89 8E ?? ?? ?? ?? 76 36 8B C1 D1 E8 8D 54 08 0A 85 D2 89 96 ?? ?? ?? ?? 75 0D 48 C7 86 ?? "
    "?? ?? ?? 00 00 00 00 EB 17 48 8B 8E ?? ?? ?? ?? 48 C1 E2 03 E8 ?? ?? ?? ?? 48 89 86 ?? ?? ?? ?? "
    "48 8B 86 ?? ?? ?? ?? 48 89 1C F8 48 8B C3 48 8B 7C 24 38 48 8B 5C 24 40 48 83 C4 20 5E C3 48 8B "
    "C3 48 8B 5C 24 40 48 83 C4 20 5E C3 48 8B C2 48 8B 5C 24 40 48 83 C4 20 5E C3";
constexpr int kElemsAt[] = {0x2A, 0x69, 0x15F, 0x16C, 0x17C, 0x183};
constexpr int kCountAt[] = {0x1C, 0x12D, 0x135, 0x144};
constexpr int kCapacityAt[] = {0x13E, 0x156};
constexpr int kFreeListAt = 0x116;
constexpr int kCallPodAdd = 0x11D, kCallColorEq = -1;
constexpr uint32_t kCountAfterElems = 8, kCapacityAfterElems = 12;
constexpr const char kPodAddPattern[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 79 08 48 8B D9 48 8B F2 8D 4F 01 3B 4B 0C 89 4B 08 76 29 8B C1 D1 E8 "
    "8D 4C 08 0A 85 C9 89 4B 0C 75 09 48 C7 03 00 00 00 00 EB 11 8B D1 48 8B 0B 48 C1 E2 03 E8 ?? ?? ?? ?? 48 89 03 48 8B 03 "
    "48 8B 5C 24 30 48 89 34 F8 48 8B 74 24 38 48 83 C4 20 5F C3";
constexpr const char kColorEqPattern[] = "";
#else
// x86 build 6156 (CryRenderD3D9.dll and CryRenderD3D10.dll)
constexpr const char kMergePattern[] =
    "53 56 8B 74 24 0C 85 F6 8B D9 0F 84 5A 01 00 00 57 8B BB ?? ?? ?? ?? 33 C0 85 FF 76 14 8B 8B ?? "
    "?? ?? ?? 3B 31 74 0A 83 C0 01 83 C1 04 3B C7 72 F2 3B C7 0F 85 29 01 00 00 85 FF C7 44 24 10 00 "
    "00 00 00 0F 86 AB 00 00 00 55 8B AB ?? ?? ?? ?? 0F B7 86 BE 00 00 00 8B 4D 00 66 39 81 BE 00 00 "
    "00 75 69 66 8B 81 C0 00 00 00 66 3B 86 C0 00 00 00 75 59 8B 91 88 00 00 00 3B 96 88 00 00 00 75 "
    "4B 8B 81 B0 00 00 00 3B 86 B0 00 00 00 75 3D F3 0F 10 81 B4 00 00 00 0F 2E 86 B4 00 00 00 9F F6 "
    "C4 44 7A 28 8B 51 0C 33 56 0C F7 C2 20 C0 00 00 75 1A 66 8B 41 0A 66 3B 46 0A 75 10 8D 56 40 52 "
    "83 C1 40 E8 ?? ?? ?? ?? 84 C0 75 18 8B 44 24 14 83 C0 01 83 C5 04 3B C7 89 44 24 14 0F 82 6E FF "
    "FF FF EB 0F 56 8D 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 75 00 5D 8B 44 24 10 3B 83 ?? ?? ?? ?? 75 62 "
    "8B BB ?? ?? ?? ?? 8D 47 01 3B 83 ?? ?? ?? ?? 89 83 ?? ?? ?? ?? 76 42 8B C8 D1 E9 8D 44 01 0A 85 "
    "C0 89 83 ?? ?? ?? ?? 75 13 89 83 ?? ?? ?? ?? 8B C8 89 34 B9 5F 8B C6 5E 5B C2 04 00 8D 14 85 00 "
    "00 00 00 8B 83 ?? ?? ?? ?? 52 50 E8 ?? ?? ?? ?? 83 C4 08 89 83 ?? ?? ?? ?? 8B 8B ?? ?? ?? ?? 89 "
    "34 B9 5F 8B C6 5E 5B C2 04 00 8B C6 5E 5B C2 04 00";
constexpr int kElemsAt[] = {0x1F, 0x4C, 0x12B, 0x145, 0x155, 0x15B};
constexpr int kCountAt[] = {0x13, 0xFA, 0x102, 0x111};
constexpr int kCapacityAt[] = {0x10B, 0x123};
constexpr int kFreeListAt = 0xE7;
constexpr int kCallPodAdd = 0xEB, kCallColorEq = 0xC3;
constexpr uint32_t kCountAfterElems = 4, kCapacityAfterElems = 8;
constexpr const char kPodAddPattern[] =
    "56 8B F1 57 8B 7E 04 8D 47 01 3B 46 08 89 46 04 76 11 8B C8 D1 E9 8D 44 01 0A 85 C0 89 46 08 75 10 89 06 8B 16 8B 44 "
    "24 0C 89 04 BA 5F 5E C2 04 00 8B 16 8D 0C 85 00 00 00 00 51 52 E8 ?? ?? ?? ?? 8B 4C 24 14 83 C4 08 89 06 89 0C B8 5F "
    "5E C2 04 00";
constexpr const char kColorEqPattern[] =
    "8B 54 24 04 F3 0F 10 01 0F 2E 02 9F F6 C4 44 7A 35 F3 0F 10 41 04 0F 2E 42 04 9F F6 C4 44 7A 26 F3 0F 10 41 08 0F "
    "2E 42 08 9F F6 C4 44 7A 17 F3 0F 10 41 0C 0F 2E 42 0C 9F F6 C4 44 7A 08 B8 01 00 00 00 C2 04 00 33 C0 C2 04 00";
#endif

const wchar_t* const kRendererModules[] = {L"CryRenderD3D9.dll", L"CryRenderD3D10.dll"};

struct PodArrayPtr {  // CryEngine PodArray<T*>: elements, count, capacity (x86: +0/+4/+8, x64: +0/+8/+12)
    char** elems;
    int count;
    int capacity;
};

#ifdef _WIN64
using PodArrayAddFn = void (*)(PodArrayPtr* self, char* item);
#define OPTIPERF_THISCALL_HOOK(name) char* name(char* renderer, char* obj)
#else
using PodArrayAddFn = void(__fastcall*)(PodArrayPtr* self, void* edx, char* item);  // thiscall PodArray<T*>::Add
using ColorEqualFn = bool(__fastcall*)(const char* a, void* edx, const char* b);   // thiscall ColorF::operator==
#define OPTIPERF_THISCALL_HOOK(name) char* __fastcall name(char* renderer, void* /*edx*/, char* obj)
#endif

enum class Mode { Off, Fast, Verify };
const char* KindName(Kind k) { return k == Kind::Present ? "present" : k == Kind::Match ? "match" : "append"; }

struct Stats {
    uint64_t calls = 0, nulls = 0, present = 0, matches = 0, appends = 0, mismatches = 0, otherThread = 0;
    uint64_t cyclesHook = 0, timedCalls = 0, cyclesLinear = 0, cyclesHashed = 0;
    int maxList = 0;
    Counters idx;
};

struct State {
    Mode mode = Mode::Fast;
    bool installed = false;
    ParticleBatchTarget target;
    PodArrayAddFn podAdd = nullptr;
#ifndef _WIN64
    ColorEqualFn colorEq = nullptr;
#endif
    char* renderer = nullptr;
    DWORD mainThread = 0;
    Index index;
    Stats st;
};
State g;

Mode ReadMode() {
    const std::wstring v = ConfigString(L"ParticleBatch", L"Mode", L"OPTIPERF_PARTICLE_BATCH", L"fast");
    if (_wcsicmp(v.c_str(), L"off") == 0 || v == L"0") return Mode::Off;
    if (_wcsicmp(v.c_str(), L"verify") == 0) return Mode::Verify;
    return Mode::Fast;
}

void Report(const char* when) {
    if (!g.installed) return;
    const Stats& s = g.st;
    const double calls = s.calls ? double(s.calls) : 1.0;
    Log("particle batch [%s]: calls=%llu merged=%llu appended=%llu present=%llu null=%llu | passes=%llu avg list/pass=%.1f "
        "max list=%d | hash chain steps/call=%.3f wildcard lookups=%llu catch-ups=%llu other-thread calls=%llu",
        when, s.calls, s.matches, s.appends, s.present, s.nulls, s.idx.resets,
        s.idx.resets ? double(s.idx.sumListAtReset) / double(s.idx.resets) : 0.0, s.maxList,
        double(s.idx.chainSteps) / calls, s.idx.wildcardLookups, s.idx.catchups, s.otherThread);
    if (g.mode == Mode::Verify)
        Log("particle batch [%s]: VERIFY mismatches=%llu | cycles/call: original linear search=%.0f, hashed lookup=%.0f", when,
            s.mismatches, double(s.cyclesLinear) / calls, double(s.cyclesHashed) / calls);
    else
        Log("particle batch [%s]: cycles/call (whole replacement, sampled) = %.0f", when,
            double(s.cyclesHook) / double(s.timedCalls ? s.timedCalls : 1));
}

void Add(PodArrayPtr* list, char* item) {
#ifdef _WIN64
    g.podAdd(list, item);
#else
    g.podAdd(list, nullptr, item);
#endif
}

bool OriginalColorEq(const char* a, const char* b) {
#ifdef _WIN64
    return SameColor(a - kLayout.color, b - kLayout.color, kLayout);  // inlined in the x64 original
#else
    return g.colorEq(a, nullptr, b);
#endif
}

// Replaces the renderer function: thiscall(this = renderer, CRenderObject* obj).
OPTIPERF_THISCALL_HOOK(MergeHook) {
    // rdtsc costs ~25 cycles; in fast mode only every 64th call is timed (the report extrapolates)
    const bool timed = g.mode == Mode::Verify || (g.st.calls & 63) == 0;
    const uint64_t t0 = timed ? __rdtsc() : 0;
    if (!obj) {
        ++g.st.nulls;
        return obj;
    }
    if (renderer != g.renderer) {
        g.renderer = renderer;
        g.index.Reset();
    }
    const DWORD tid = GetCurrentThreadId();
    if (!g.mainThread) g.mainThread = tid;
    if (tid != g.mainThread) ++g.st.otherThread;
    ++g.st.calls;

    auto* list = reinterpret_cast<PodArrayPtr*>(renderer + g.target.listOffset);
    auto* freeList = reinterpret_cast<PodArrayPtr*>(renderer + g.target.freeListOffset);
    const bool daz = (_mm_getcsr() & 0x40) != 0;
    Sync(g.index, list->elems, list->count, daz, kLayout, g.st.idx);
    if (list->count > g.st.maxList) g.st.maxList = list->count;

    Key key;
    KeyKind kind;
    const uint64_t t1 = g.mode == Mode::Verify ? __rdtsc() : 0;
    Result r = HashedLookup(g.index, list->elems, list->count, obj, daz, kLayout, &key, &kind, g.st.idx);
    if (g.mode == Mode::Verify) {
        const uint64_t t2 = __rdtsc();
        const Result ref = LinearLookup(list->elems, list->count, obj, kLayout, OriginalColorEq);
        const uint64_t t3 = __rdtsc();
        g.st.cyclesHashed += t2 - t1;
        g.st.cyclesLinear += t3 - t2;
        if (ref.kind != r.kind || (ref.kind == Kind::Match && ref.index != r.index)) {
            if (++g.st.mismatches <= 20)
                Log("VERIFY mismatch #%llu: original=%s[%d] hashed=%s[%d] list=%d obj=%p", g.st.mismatches, KindName(ref.kind),
                    ref.index, KindName(r.kind), r.index, list->count, obj);
            r = ref;  // behave exactly like the original
        }
    }

    char* result = obj;
    switch (r.kind) {
        case Kind::Present:
            ++g.st.present;
            break;
        case Kind::Match:
            ++g.st.matches;
            Add(freeList, obj);  // the original recycles the duplicate temp object
            result = list->elems[r.index];
            break;
        case Kind::Append:
            ++g.st.appends;
            Add(list, obj);
            g.index.Add(obj, key, kind);
            break;
    }
    if (timed) {
        g.st.cyclesHook += __rdtsc() - t0;
        ++g.st.timedCalls;
    }
    return result;
}

bool AllEqual(const char* f, std::initializer_list<int> offsets, uint32_t* value) {
    *value = ReadU32(f + *offsets.begin());
    for (int o : offsets)
        if (ReadU32(f + o) != *value) return false;
    return true;
}

}  // namespace

bool ParticleBatch_Locate(char* base, ParticleBatchTarget* out, const char** why) {
    static const Pattern merge = ParsePattern(kMergePattern), podAdd = ParsePattern(kPodAddPattern),
                         colorEq = ParsePattern(kColorEqPattern);
    const auto hits = FindInCode(base, merge, 2);
    if (hits.size() != 1) {
        *why = hits.empty() ? "merge function not found" : "merge function pattern matches more than once";
        return false;
    }
    char* f = hits[0];
    uint32_t elems, count, capacity;
    if (!AllEqual(f, {kElemsAt[0], kElemsAt[1], kElemsAt[2], kElemsAt[3], kElemsAt[4], kElemsAt[5]}, &elems) ||
        !AllEqual(f, {kCountAt[0], kCountAt[1], kCountAt[2], kCountAt[3]}, &count) ||
        !AllEqual(f, {kCapacityAt[0], kCapacityAt[1]}, &capacity) || count != elems + kCountAfterElems ||
        capacity != elems + kCapacityAfterElems) {
        *why = "list member offsets in the merge function are inconsistent";
        return false;
    }
    char* podAddFn = RelTarget(f + kCallPodAdd);
    if (!IsInCode(base, podAddFn, podAdd.size()) || !MatchesAt(podAddFn, podAdd)) {
        *why = "PodArray::Add does not look as expected";
        return false;
    }
    char* colorEqFn = nullptr;
    if constexpr (kCallColorEq >= 0) {
        colorEqFn = RelTarget(f + kCallColorEq);
        if (!IsInCode(base, colorEqFn, colorEq.size()) || !MatchesAt(colorEqFn, colorEq)) {
            *why = "ColorF compare does not look as expected";
            return false;
        }
    }
    out->merge = f;
    out->listOffset = elems;
    out->freeListOffset = ReadU32(f + kFreeListAt);
    out->podAdd = podAddFn;
    out->colorEq = colorEqFn;
    return true;
}

void ParticleBatch_OnModuleLoaded(const wchar_t* baseName, char* base, size_t /*size*/) {
    bool renderer = false;
    for (const wchar_t* name : kRendererModules) renderer |= _wcsicmp(baseName, name) == 0;
    if (!renderer || g.installed) return;

    g.mode = ReadMode();
    if (g.mode == Mode::Off) {
        Log("%ls: particle batch fix disabled ([ParticleBatch] Mode=off)", baseName);
        return;
    }
    const char* why = nullptr;
    ParticleBatchTarget t;
    if (!ParticleBatch_Locate(base, &t, &why)) {
        Log("%ls: particle batch fix not applied: %s", baseName, why);
        return;
    }
    g.target = t;
    g.podAdd = reinterpret_cast<PodArrayAddFn>(t.podAdd);
#ifndef _WIN64
    g.colorEq = reinterpret_cast<ColorEqualFn>(t.colorEq);
#endif
    if (!InstallHook(t.merge, reinterpret_cast<void*>(&MergeHook), nullptr, "particle batch merge")) {
        Log("%ls: particle batch fix not applied: could not write the hook", baseName);
        return;
    }
    g.installed = true;
    Log("%ls: particle batch fix installed at +0x%X (list +0x%X, free list +0x%X, mode=%s)", baseName,
        unsigned(t.merge - base), t.listOffset, t.freeListOffset, g.mode == Mode::Verify ? "verify" : "fast");
}

void ParticleBatch_Report(const char* when) { Report(when); }

void ParticleBatch_ResetIndex() { g.index.Reset(); }

bool ParticleBatch_Installed(ParticleBatchTarget* out) {
    if (!g.installed) return false;
    *out = g.target;
    return true;
}

void ParticleBatch_ResetForTest() {
    g.installed = false;
    g.renderer = nullptr;
    g.index.Reset();
    g.st = Stats{};
}

}  // namespace optiperf
