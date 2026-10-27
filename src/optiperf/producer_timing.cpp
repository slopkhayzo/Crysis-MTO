// The producers' time: see producer_timing.h.
//
// Each method's slot goes to a hook that forwards ten integer arguments (this and nine: the methods take integers,
// pointers and references only, none more than nine; the stack ones the caller didn't pass are read from its frame and
// ignored) and the integer result. On the main thread the outermost such call is timed (a nested one only counted),
// under the frame phase it was made in, or under the flush's entry. EF_GetObject's requests are told apart by kind
// (EF_GetObject(bool bTemp, int num): a temporary object, a new permanent one, a permanent one by its id), with the
// permanent ones' callers, and whether a permanent object comes back in a later frame (a render object the next
// traversal would fill while the flush still draws it). And what a recording of the calls would have to copy: per
// method, how often each argument points into the main thread's stack (past a method's own arguments, the words are
// the caller's frame), and per 8-byte word of IRenderMesh::Render's SRendParams, how often it points there.
#include "producer_timing.h"

#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <unordered_map>

#include "calltrace.h"
#include "config.h"
#include "diag.h"
#include "frame_phases.h"
#include "log.h"
#include "render_gate.h"
#include "scan.h"

namespace optiperf {
namespace {

#ifdef _WIN64
struct Entry {
    const char* cls;
    unsigned slot;
    const char* name;
};
const Entry kEntries[] = {
    {".?AVCD3D9Renderer@@", 113, "EF_GetObject"},
    {".?AVCD3D9Renderer@@", 114, "EF_AddEf"},
    {".?AVCD3D9Renderer@@", 87, "EF_AddParticlesToScene"},
    {".?AVCD3D9Renderer@@", 91, "EF_AddPolygonToScene"},
    {".?AVCD3D9Renderer@@", 92, "EF_AddPolygonToScene (2)"},
    {".?AVCD3D9Renderer@@", 117, "EF_ADDDlight"},
    {".?AVCD3D9Renderer@@", 194, "PushFogVolumeContribution"},
    {".?AUCRenderMesh@@", 47, "IRenderMesh::Render"},
    {".?AUCRenderMesh@@", 24, "IRenderMesh::AddRenderElements"},
    {".?AUCRenderMesh@@", 40, "IRenderMesh::AddRE"},
    {".?AUCRenderMesh@@", 34, "IRenderMesh::SetMaterial"},
    {".?AUCRenderMesh@@", 26, "IRenderMesh::SetChunk"},
    {".?AUCRenderMesh@@", 27, "IRenderMesh::SetChunk (2)"},
    {".?AUCRenderMesh@@", 25, "IRenderMesh::SetRECustomData"},
    {".?AUCRenderMesh@@", 30, "IRenderMesh::UpdateSysIndices"},
    {".?AUCRenderMesh@@", 31, "IRenderMesh::UpdateSysVertices"},
    {".?AUCRenderMesh@@", 42, "IRenderMesh::SetVertexContainer"},
    {".?AUCRenderMesh@@", 39, "IRenderMesh::InvalidateVideoBuffer"},
    {".?AUCRenderMesh@@", 36, "IRenderMesh::SetBBox"},
    {".?AUCRenderMesh@@", 38, "IRenderMesh::UpdateBBoxFromMesh"},
    {".?AUCRenderMesh@@", 43, "IRenderMesh::SetUpdateFrame"},
    {".?AVCRenderObject@@", 0, "CRenderObject::AddWaves"},
    {".?AVCRenderObject@@", 1, "CRenderObject::GetObjData"},
};
constexpr int kEntryCount = int(sizeof kEntries / sizeof kEntries[0]);
enum Group { kTraversal, kEntry, kOther, kGroups };  // the traversal (FramePhases' world), the flush's entry, the rest
const char* const kGroupNames[kGroups] = {"the traversal", "the flush's entry", "the rest of the frame"};

using Generic = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t);

struct Stat {
    uint64_t calls = 0, cycles = 0;
};
constexpr int kRenderEntry = 7;     // IRenderMesh::Render(const SRendParams&, CRenderObject*, IMaterial*, ...)
constexpr int kParamsWords = 200 / 8;  // SRendParams (200 bytes on x64)
enum ObjectKind { kTemporary, kNewPermanent, kPermanentById, kObjectKinds };
const char* const kObjectKindNames[kObjectKinds] = {"temporary", "a new permanent one", "a permanent one by its id"};
constexpr int kCallers = 16;
struct Caller {
    void* at = nullptr;
    uint64_t calls = 0;
};
struct State {
    bool installed = false;
    unsigned long main = 0;
    bool inEntry = false;
    uint64_t frames = 0;
    void* orig[kEntryCount] = {};
    Stat stats[kEntryCount][kGroups];
    uint64_t nested[kEntryCount] = {};
    // EF_GetObject (entry 0): its requests by kind and group; the permanent ones' callers; the permanent objects
    // returned, with the frame each was last returned in (one returned in a later frame again: reused)
    uint64_t objects[kObjectKinds][kGroups] = {};
    Caller callers[kCallers];
    uint64_t otherCallers = 0, reused = 0, permanentNull = 0;
    std::unordered_map<void*, uint64_t> permanent;
    // the arguments pointing into the main thread's stack (a1..a9), and IRenderMesh::Render's SRendParams words
    ULONG_PTR stackLow = 0, stackHigh = 0;
    uint64_t stackArgs[kEntryCount][9] = {};
    uint64_t paramsWords[kParamsWords] = {}, paramsCalls = 0;
};
State g;
thread_local int t_depth = 0;

int GroupNow() {
    if (g.inEntry) return kEntry;
    return CallTrace_Phase() == kPhaseWorld ? kTraversal : kOther;
}

bool OnStack(uint64_t v) { return v >= g.stackLow && v < g.stackHigh; }
// on the main thread, an outermost call: which of its arguments point into the stack
void NoteStack(int entry, const uint64_t* args) {
    for (int k = 0; k < 9; ++k)
        if (OnStack(args[k])) ++g.stackArgs[entry][k];
    if (entry != kRenderEntry || !OnStack(args[0])) return;
    ++g.paramsCalls;
    const uint64_t* const params = reinterpret_cast<const uint64_t*>(args[0]);
    for (int w = 0; w < kParamsWords; ++w)
        if (OnStack(params[w])) ++g.paramsWords[w];
}
// on the main thread: an EF_GetObject request (a1 bTemp, a2 num) and its object
void NoteObject(uint64_t bTemp, uint64_t num, void* object, void* caller, int group) {
    const int kind = (bTemp & 0xFF) ? kTemporary : int32_t(num) < 0 ? kNewPermanent : kPermanentById;
    ++g.objects[kind][group];
    if (kind == kTemporary) return;
    int i = 0;
    while (i < kCallers && g.callers[i].at && g.callers[i].at != caller) ++i;
    if (i < kCallers) {
        g.callers[i].at = caller;
        ++g.callers[i].calls;
    } else {
        ++g.otherCallers;
    }
    if (!object) {
        ++g.permanentNull;
        return;
    }
    auto it = g.permanent.find(object);
    if (it == g.permanent.end()) {
        g.permanent.emplace(object, g.frames);
    } else {
        if (it->second != g.frames) ++g.reused;
        it->second = g.frames;
    }
}

template <int I>
uint64_t Hook(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
              uint64_t a8, uint64_t a9) {
    const Generic f = reinterpret_cast<Generic>(g.orig[I]);
    if (GetCurrentThreadId() != g.main) return f(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
    if (t_depth > 0) {  // (inside another producer call: counted, its time is the outer one's)
        ++g.nested[I];
        ++t_depth;
        const uint64_t v = f(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
        --t_depth;
        return v;
    }
    const int group = GroupNow();
    if (group == kTraversal) {
        const uint64_t args[9] = {a1, a2, a3, a4, a5, a6, a7, a8, a9};
        NoteStack(I, args);
    }
    ++t_depth;
    const uint64_t t0 = __rdtsc();
    const uint64_t v = f(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
    const uint64_t dt = __rdtsc() - t0;
    --t_depth;
    Stat& s = g.stats[I][group];
    ++s.calls;
    s.cycles += dt;
    if constexpr (I == 0) NoteObject(a1, a2, reinterpret_cast<void*>(v), _ReturnAddress(), group);
    return v;
}
template <int... I>
struct Hooks {
    static constexpr int kCount = sizeof...(I);
    static void* At(int i) {
        void* const all[] = {reinterpret_cast<void*>(&Hook<I>)...};
        return all[i];
    }
};
using AllHooks = Hooks<0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22>;
static_assert(AllHooks::kCount == kEntryCount, "a hook per entry");

// a vtable slot pointed at fn (one aligned store), its entry kept
bool PatchSlot(void** slot, void* fn, void** old) {
    DWORD protect;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
    *old = *slot;
    InterlockedExchangePointer(slot, fn);
    VirtualProtect(slot, sizeof(void*), protect, &protect);
    return true;
}
#endif

}  // namespace

void ProducerTiming_Install(const wchar_t* moduleName, char* base) {
#ifdef _WIN64
    if (ConfigString(L"Debug", L"ProducerTiming", L"OPTIPERF_PRODUCER_TIMING", L"off") != L"on") return;
    if (!ConfigString(L"Trace", L"Modules", L"OPTIPERF_TRACE", L"").empty()) {
        Log("%ls: producer timing: not installed (the call tracer is on: it has the slots)", moduleName);
        return;
    }
    if (RenderGate_Slots() > 0) {
        Log("%ls: producer timing: not installed (FlushThread=overlap: the gate has the slots)", moduleName);
        return;
    }
    for (int i = 0; i < kEntryCount; ++i) {
        void** const vt = FindRttiVtable(base, kEntries[i].cls);
        if (!vt || !IsInCode(base, static_cast<const char*>(vt[kEntries[i].slot]), 1) ||
            !PatchSlot(&vt[kEntries[i].slot], AllHooks::At(i), &g.orig[i])) {
            Log("%ls: producer timing: not installed (%s's slot %u)", moduleName, kEntries[i].cls, kEntries[i].slot);
            for (int k = 0; k < i; ++k) {  // (put back)
                void* ignored;
                PatchSlot(&FindRttiVtable(base, kEntries[k].cls)[kEntries[k].slot], g.orig[k], &ignored);
            }
            return;
        }
        CallTrace_SeeThrough(AllHooks::At(i), g.orig[i]);  // (the vtable copies made later measure through it)
    }
    g.main = GetCurrentThreadId();
    GetCurrentThreadStackLimits(&g.stackLow, &g.stackHigh);
    g.installed = true;
    Log("%ls: producer timing: %d methods timed on the main thread (the outermost call; per frame phase, with "
        "FramePhases=on)",
        moduleName, kEntryCount);
#else
    (void)moduleName;
    (void)base;
#endif
}

void ProducerTiming_OnFlushEntry() {
#ifdef _WIN64
    g.inEntry = true;
#endif
}

void ProducerTiming_OnFlushStart() {
#ifdef _WIN64
    g.inEntry = false;
    ++g.frames;
#endif
}

void ProducerTiming_Report(const char* when) {
#ifdef _WIN64
    if (!g.installed || !g.frames) return;
    const double f = double(g.frames);
    double total[kGroups] = {};
    char line[3000] = "";
    size_t used = 0;
    for (int i = 0; i < kEntryCount; ++i) {
        uint64_t calls = g.nested[i];
        for (int k = 0; k < kGroups; ++k) {
            calls += g.stats[i][k].calls;
            total[k] += double(g.stats[i][k].cycles);
        }
        if (!calls) continue;
        const int w = snprintf(line + used, sizeof line - used,
                               "%s%s %.1f/%.1f/%.1f calls, %.1f/%.1f/%.1f us (%.1f nested)", used ? "; " : "",
                               kEntries[i].name, double(g.stats[i][kTraversal].calls) / f,
                               double(g.stats[i][kEntry].calls) / f, double(g.stats[i][kOther].calls) / f,
                               double(g.stats[i][kTraversal].cycles) / 3.0e3 / f,
                               double(g.stats[i][kEntry].cycles) / 3.0e3 / f,
                               double(g.stats[i][kOther].cycles) / 3.0e3 / f, double(g.nested[i]) / f);
        if (w < 0 || size_t(w) >= sizeof line - used) break;
        used += size_t(w);
    }
    Log("producer timing [%s]: per frame (%llu frames), the outermost calls' time on the main thread in %s, %s and "
        "%s: %.1f, %.1f and %.1f us",
        when, g.frames, kGroupNames[kTraversal], kGroupNames[kEntry], kGroupNames[kOther],
        total[kTraversal] / 3.0e3 / f, total[kEntry] / 3.0e3 / f, total[kOther] / 3.0e3 / f);
    Log("producer timing [%s]: by method (calls, then us, per frame: the traversal/the flush's entry/the rest): %s",
        when, used ? line : "none");
    char kinds[600] = "";
    size_t ku = 0;
    for (int k = 0; k < kObjectKinds; ++k) {
        const int w = snprintf(kinds + ku, sizeof kinds - ku, "%s%s %.1f/%.1f/%.1f", ku ? ", " : "", kObjectKindNames[k],
                               double(g.objects[k][kTraversal]) / f, double(g.objects[k][kEntry]) / f,
                               double(g.objects[k][kOther]) / f);
        if (w < 0 || size_t(w) >= sizeof kinds - ku) break;
        ku += size_t(w);
    }
    char callers[1600] = "";
    size_t cu = 0;
    for (const Caller& c : g.callers) {
        if (!c.at) break;
        char where[96];
        const int w = snprintf(callers + cu, sizeof callers - cu, "%s%s %llu", cu ? ", " : "",
                               DescribeAddress(c.at, where, sizeof where), c.calls);
        if (w < 0 || size_t(w) >= sizeof callers - cu) break;
        cu += size_t(w);
    }
    Log("producer timing [%s]: EF_GetObject's requests per frame by kind (the traversal/the flush's entry/the rest): %s "
        "| the permanent ones' callers: %s (others %llu) | permanent objects %zu, returned again in a later frame %llu "
        "times (none returned %llu)",
        when, kinds, cu ? callers : "none", g.otherCallers, g.permanent.size(), g.reused, g.permanentNull);
    char stack[3000] = "";
    size_t su = 0;
    for (int i = 0; i < kEntryCount; ++i) {
        const uint64_t calls = g.stats[i][kTraversal].calls;
        bool any = false;
        for (int k = 0; k < 9; ++k) any |= g.stackArgs[i][k] != 0;
        if (!calls || !any) continue;
        int w = snprintf(stack + su, sizeof stack - su, "%s%s:", su ? "; " : "", kEntries[i].name);
        for (int k = 0; k < 9 && w > 0 && size_t(w) < sizeof stack - su; ++k) {
            if (!g.stackArgs[i][k]) continue;
            su += size_t(w);
            w = snprintf(stack + su, sizeof stack - su, " a%d %.0f%%", k + 1,
                         100.0 * double(g.stackArgs[i][k]) / double(calls));
        }
        if (w < 0 || size_t(w) >= sizeof stack - su) break;
        su += size_t(w);
    }
    char words[600] = "";
    size_t wu = 0;
    for (int k = 0; k < kParamsWords; ++k) {
        if (!g.paramsWords[k]) continue;
        const int w = snprintf(words + wu, sizeof words - wu, "%s+0x%X %.0f%%", wu ? ", " : "", k * 8,
                               100.0 * double(g.paramsWords[k]) / double(g.paramsCalls));
        if (w < 0 || size_t(w) >= sizeof words - wu) break;
        wu += size_t(w);
    }
    Log("producer timing [%s]: in the traversal, arguments pointing into the main thread's stack (share of the calls; "
        "past a method's arguments the words are its caller's frame): %s | IRenderMesh::Render's SRendParams words "
        "pointing there (of %llu calls): %s",
        when, su ? stack : "none", g.paramsCalls, wu ? words : "none");
#else
    (void)when;
#endif
}

}  // namespace optiperf
