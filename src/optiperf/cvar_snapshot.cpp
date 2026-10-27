// Cvar snapshot: Phase 2 of research/docs/cpu-multithreading-plan.md, after the material snapshot (details in
// research/docs/cvar-snapshot.md). x64 only.
//
// The renderer reads other modules' console variables through ICVar pointers it fetched once by name and keeps in
// statics: the skinned draw ca_SphericalSkinning and e_vegetation_sphericalskinning per batch, the shadow setup the
// e_gsm_* cvars, EF_EndEf3D two string cvars; 300-530 calls per frame. On the eight test levels every one of them is
// a CXConsoleVariableIntRef (an int pointing at the owning module's variable: GetIVal, GetFVal) or a
// CXConsoleVariableString (its text in the object: GetString), both CrySystem's.
//   - on: the two classes' getter slots (ICVar 1 GetIVal, 2 GetFVal, 3 GetString) are patched in their vtables. A
//     call from the renderer's code on the main thread inside the flush is answered from that cvar's capture. The
//     captures are of the cvars and getters the renderer has asked for inside a flush so far, read right before each
//     flush (a string copied into a buffer of ours); one asked for the first time is captured then, its one call to
//     CrySystem, and answered from then on. The setter slots (4 Set(int), 5 Set(float), 6 Set(const char*), 7 ForceSet)
//     mark the captures out of date when called on the main thread inside the flush: the rest of it reads CrySystem.
//     Every other call goes to CrySystem.
//   - verify: as on, but each answered call also reads CrySystem, compared (a string by its text) and returned.
// The getters' code is checked by pattern; with any of it different the part stays off.
//
// [ShadowPrepass] Cvars = off (default) | on | verify     (env OPTIPERF_SHADOW_PREPASS_CVARS)

#include "cvar_snapshot.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <string>

#include "calltrace.h"
#include "config.h"
#include "diag.h"
#include "flush_thread.h"
#include "key_index.h"
#include "log.h"
#include "scan.h"

namespace optiperf {
namespace {

enum class CMode { Off, On, Verify };

CMode ReadMode() {
    const std::wstring v = ConfigString(L"ShadowPrepass", L"Cvars", L"OPTIPERF_SHADOW_PREPASS_CVARS", L"off");
    if (_wcsicmp(v.c_str(), L"on") == 0 || v == L"1") return CMode::On;
    if (_wcsicmp(v.c_str(), L"verify") == 0) return CMode::Verify;
    return CMode::Off;
}

#ifdef _WIN64
const char* ModeName(CMode m) { return m == CMode::On ? "on" : m == CMode::Verify ? "verify" : "off"; }

// --- the two classes (x64 CrySystem.dll, build 6156) -------------------------------------------------------------------
constexpr const char* kClassNames[kCvClasses] = {".?AVCXConsoleVariableIntRef@@", ".?AVCXConsoleVariableString@@"};
constexpr const char* kClassLabels[kCvClasses] = {"IntRef", "String"};
// their getters, the whole of each: an IntRef reads the int it points at (+0x30); a String keeps its text at +0x30
constexpr const char* kGetterCode[kCvClasses][kCvGetters] = {
    {"48 8B 41 30 8B 00 C3", "48 8B 41 30 F3 0F 2A 00 C3",
     "48 83 EC 28 48 8B 41 30 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 44 8B 00 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 "
     "83 C4 28 C3"},
    {"48 83 C1 30 48 FF 25 ?? ?? ?? ??", "48 83 EC 28 48 83 C1 30 FF 15 ?? ?? ?? ?? F2 0F 5A C0 48 83 C4 28 C3",
     "48 8D 41 30 C3"},
};
constexpr const char* kGetterNames[kCvGetters] = {"GetIVal", "GetFVal", "GetString"};
constexpr int kFirstGetter = 1, kFirstSetter = 4;  // ICVar slots
constexpr size_t kMaxString = 0x100;  // a String cvar's buffer

using IntFn = int (*)(char*);
using FloatFn = float (*)(char*);
using StringFn = const char* (*)(char*);
using SetIntFn = void (*)(char*, int);
using SetFloatFn = void (*)(char*, float);
using SetStringFn = void (*)(char*, const char*);

struct CvEntry {  // one cvar and getter the renderer asks for inside the flush
    char* cvar = nullptr;
    uint8_t cls = 0, getter = 0;
    bool captured = false;  // read before this flush
    int iv = 0;
    float fv = 0.0f;
    char str[kMaxString] = {};
    uint64_t answered = 0, same = 0, differ = 0;
};

struct Caller {  // code outside the renderer that read a cvar inside the flush, on the main thread
    void* ret = nullptr;
    uint64_t calls = 0;
};

struct CStats {
    uint64_t flushes = 0, captureCycles = 0, stale = 0, setsInFlush = 0, firstAsked = 0, longStrings = 0;
    uint64_t answered[kCvClasses][kCvGetters] = {}, same = 0, differ = 0, otherThread = 0;
    uint64_t outside = 0;  // ... those calls (CrySystem answers them), and the first few callers
    Caller callers[8];
};

struct CState {
    CMode mode = CMode::Off;
    bool installed = false, setupTried = false, ready = false, verify = false;
    bool inFlush = false, live = false;  // inside the flush; and the captures still up to date
    char* rendererBase = nullptr;
    size_t rendererSize = 0;
    DWORD mainThread = 0;
    CvarTarget t;
    void* getOrig[kCvClasses][kCvGetters] = {};  // the vtable entries the hooks replaced
    void* setOrig[kCvClasses][kCvSetters] = {};
    std::deque<CvEntry> entries;  // (a deque: the strings handed out stay where they are)
    KeyIndex index;
    CStats st;
    int logged = 0, loggedSet = 0;
};
CState g;

void Append(char* line, size_t size, size_t& n, const char* fmt, ...) {
    if (n >= size) return;
    va_list a;
    va_start(a, fmt);
    const int k = vsnprintf(line + n, size - n, fmt, a);
    va_end(a);
    if (k > 0) n = (std::min)(size - 1, n + size_t(k));
}

bool Matches(char* base, const char* at, const char* text) {
    const Pattern p = ParsePattern(text);
    return at && IsInCode(base, at, p.size()) && MatchesAt(at, p);
}

// a vtable entry as a mapped module's address (the module may not be relocated where the test maps it)
char* Entry(char* base, void* entry) {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint64_t e = reinterpret_cast<uint64_t>(entry), size = nt->OptionalHeader.SizeOfImage;
    if (e - reinterpret_cast<uint64_t>(base) < size) return static_cast<char*>(entry);
    if (e - nt->OptionalHeader.ImageBase < size) return base + (e - nt->OptionalHeader.ImageBase);
    return nullptr;
}

uint64_t Key(const char* cvar, int cls, int getter) {
    return uint64_t(uintptr_t(cvar)) | uint64_t(cls * kCvGetters + getter) << 56;
}

// the captured value of one getter of one cvar
void Capture(CvEntry& e) {
    void* fn = g.getOrig[e.cls][e.getter];
    e.captured = true;
    switch (e.getter) {
    case 0: e.iv = reinterpret_cast<IntFn>(fn)(e.cvar); break;
    case 1: e.fv = reinterpret_cast<FloatFn>(fn)(e.cvar); break;
    default: {
        const char* s = reinterpret_cast<StringFn>(fn)(e.cvar);
        const size_t n = s ? strnlen(s, kMaxString) : kMaxString;
        if (n >= kMaxString) {  // (not a string that fits: left to CrySystem)
            e.captured = false;
            ++g.st.longStrings;
            break;
        }
        std::memcpy(e.str, s, n + 1);
    }
    }
}

// A getter call: the entry to answer it from, or null (then CrySystem's getter answers it). Asked for the first time
// by the renderer inside the flush, the cvar and getter are noted and captured there (CrySystem's getter, once: the
// call vanilla makes), and every call from then on is answered (the next flushes' captures are taken before them).
CvEntry* Answering(char* cvar, int cls, int getter, void* ret) {
    if (!g.inFlush) return nullptr;
    if (uintptr_t(ret) - uintptr_t(g.rendererBase) >= g.rendererSize) {  // (counted on the flush's thread, by caller)
        if (!OnFlushThread()) return nullptr;
        ++g.st.outside;
        for (Caller& c : g.st.callers)
            if (c.ret == ret || !c.ret) {
                c.ret = ret;
                ++c.calls;
                break;
            }
        return nullptr;
    }
    if (!OnFlushThread()) {
        ++g.st.otherThread;
        return nullptr;
    }
    const uint64_t key = Key(cvar, cls, getter);
    if (const uint32_t* at = g.index.Find(key)) {
        CvEntry& e = g.entries[*at];
        return g.live && e.captured ? &e : nullptr;
    }
    g.index.Insert(key, uint32_t(g.entries.size()));
    CvEntry& e = g.entries.emplace_back();
    e.cvar = cvar;
    e.cls = uint8_t(cls);
    e.getter = uint8_t(getter);
    ++g.st.firstAsked;
    Capture(e);
    return g.live && e.captured ? &e : nullptr;
}

void Compare(CvEntry& e, bool same, const char* live, const char* mine) {
    if (same) {
        ++e.same;
        ++g.st.same;
        return;
    }
    ++e.differ;
    ++g.st.differ;
    if (++g.logged <= 20)
        Log("cvars: VERIFY flush %llu: %s::%s on %p differs: capture %s, CrySystem %s", g.st.flushes,
            kClassLabels[e.cls], kGetterNames[e.getter], e.cvar, mine, live);
}

template <int C>
int GetIValHook(char* self) {
    const auto orig = reinterpret_cast<IntFn>(g.getOrig[C][0]);
    CvEntry* e = Answering(self, C, 0, _ReturnAddress());
    if (!e) return orig(self);
    if (g.verify) {
        const int live = orig(self);
        char a[16], b[16];
        snprintf(a, sizeof a, "%d", live);
        snprintf(b, sizeof b, "%d", e->iv);
        Compare(*e, live == e->iv, a, b);
        return live;
    }
    ++e->answered;
    ++g.st.answered[C][0];
    return e->iv;
}

template <int C>
float GetFValHook(char* self) {
    const auto orig = reinterpret_cast<FloatFn>(g.getOrig[C][1]);
    CvEntry* e = Answering(self, C, 1, _ReturnAddress());
    if (!e) return orig(self);
    if (g.verify) {
        const float live = orig(self);
        char a[32], b[32];
        snprintf(a, sizeof a, "%.9g", double(live));
        snprintf(b, sizeof b, "%.9g", double(e->fv));
        Compare(*e, std::memcmp(&live, &e->fv, sizeof live) == 0, a, b);
        return live;
    }
    ++e->answered;
    ++g.st.answered[C][1];
    return e->fv;
}

template <int C>
const char* GetStringHook(char* self) {
    const auto orig = reinterpret_cast<StringFn>(g.getOrig[C][2]);
    CvEntry* e = Answering(self, C, 2, _ReturnAddress());
    if (!e) return orig(self);
    if (g.verify) {
        const char* live = orig(self);
        Compare(*e, live && std::strcmp(live, e->str) == 0, live ? live : "(null)", e->str);
        return live;
    }
    ++e->answered;
    ++g.st.answered[C][2];
    return e->str;
}

// a cvar set on the flush's thread inside the flush: the captures may be out of date from here on
void Changed(const char* how, char* cvar) {
    if (!g.inFlush || !OnFlushThread()) return;
    ++g.st.setsInFlush;
    if (g.live) {
        g.live = false;
        ++g.st.stale;
    }
    if (++g.loggedSet <= 10)
        Log("cvars: flush %llu: a cvar (%p) set with %s inside the flush: the rest of it reads CrySystem", g.st.flushes,
            cvar, how);
}

template <int C>
void SetIntHook(char* self, int v) {
    Changed("Set(int)", self);
    reinterpret_cast<SetIntFn>(g.setOrig[C][0])(self, v);
}
template <int C>
void SetFloatHook(char* self, float v) {
    Changed("Set(float)", self);
    reinterpret_cast<SetFloatFn>(g.setOrig[C][1])(self, v);
}
template <int C>
void SetStringHook(char* self, const char* v) {
    Changed("Set(const char*)", self);
    reinterpret_cast<SetStringFn>(g.setOrig[C][2])(self, v);
}
template <int C>
void ForceSetHook(char* self, const char* v) {
    Changed("ForceSet", self);
    reinterpret_cast<SetStringFn>(g.setOrig[C][3])(self, v);
}

void* const kGetHooks[kCvClasses][kCvGetters] = {
    {reinterpret_cast<void*>(&GetIValHook<0>), reinterpret_cast<void*>(&GetFValHook<0>),
     reinterpret_cast<void*>(&GetStringHook<0>)},
    {reinterpret_cast<void*>(&GetIValHook<1>), reinterpret_cast<void*>(&GetFValHook<1>),
     reinterpret_cast<void*>(&GetStringHook<1>)},
};
void* const kSetHooks[kCvClasses][kCvSetters] = {
    {reinterpret_cast<void*>(&SetIntHook<0>), reinterpret_cast<void*>(&SetFloatHook<0>),
     reinterpret_cast<void*>(&SetStringHook<0>), reinterpret_cast<void*>(&ForceSetHook<0>)},
    {reinterpret_cast<void*>(&SetIntHook<1>), reinterpret_cast<void*>(&SetFloatHook<1>),
     reinterpret_cast<void*>(&SetStringHook<1>), reinterpret_cast<void*>(&ForceSetHook<1>)},
};

// a vtable slot pointed at fn (one aligned store: other threads calling meanwhile get the old or the new entry)
bool PatchSlot(void** slot, void* fn, void** old) {
    DWORD protect;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
    *old = *slot;
    InterlockedExchangePointer(slot, fn);
    VirtualProtect(slot, sizeof(void*), protect, &protect);
    return true;
}

// At the first flush: CrySystem's two classes, their getters checked, the slots patched.
void Setup() {
    g.setupTried = true;
    HMODULE sys = GetModuleHandleW(L"CrySystem.dll");
    const char* why = "CrySystem.dll is not loaded";
    if (!sys || !CvarSnapshot_Locate(reinterpret_cast<char*>(sys), &g.t, &why)) {
        Log("cvars: left to CrySystem: %s", why);
        return;
    }
    struct Done {
        void** slot;
        void* old;
    } done[kCvClasses * (kCvGetters + kCvSetters)];
    int n = 0;
    for (int c = 0; c < kCvClasses; ++c) {
        for (int i = 0; i < kCvGetters + kCvSetters; ++i) {
            const bool getter = i < kCvGetters;
            void** slot = &g.t.vtables[c][getter ? kFirstGetter + i : kFirstSetter + i - kCvGetters];
            void** old = getter ? &g.getOrig[c][i] : &g.setOrig[c][i - kCvGetters];
            if (!PatchSlot(slot, getter ? kGetHooks[c][i] : kSetHooks[c][i - kCvGetters], old)) {
                for (int k = 0; k < n; ++k) {
                    void* ignored;
                    PatchSlot(done[k].slot, done[k].old, &ignored);
                }
                Log("cvars: left to CrySystem: could not write a cvar class's vtable");
                return;
            }
            done[n++] = Done{slot, *old};
        }
    }
    g.mainThread = GetCurrentThreadId();
    g.ready = true;
    char* base = reinterpret_cast<char*>(sys);
    Log("cvars: mode=%s: CXConsoleVariableIntRef +0x%X and CXConsoleVariableString +0x%X: getters and setters patched",
        ModeName(g.mode), unsigned(reinterpret_cast<char*>(g.t.vtables[kCvIntRef]) - base),
        unsigned(reinterpret_cast<char*>(g.t.vtables[kCvString]) - base));
}

void Report(const char* when) {
    const CStats& s = g.st;
    uint64_t answered = 0;
    char line[2048] = "";
    size_t n = 0;
    for (int c = 0; c < kCvClasses; ++c)
        for (int i = 0; i < kCvGetters; ++i) {
            answered += s.answered[c][i];
            if (s.answered[c][i]) Append(line, sizeof line, n, " %s::%s %llu", kClassLabels[c], kGetterNames[i], s.answered[c][i]);
        }
    Log("cvars [%s]: mode=%s | flushes=%llu, capture %.4f ms each | cvars and getters asked for: %zu (first time inside "
        "a flush %llu; strings too long %llu) | calls %s=%llu%s | out of date in %llu flushes (sets inside the flush "
        "%llu) | from other threads %llu",
        when, ModeName(g.mode), s.flushes, double(s.captureCycles) / 3.0e6 / double(s.flushes ? s.flushes : 1),
        g.entries.size(), s.firstAsked, s.longStrings, g.verify ? "compared" : "answered",
        g.verify ? s.same + s.differ : answered, g.verify ? "" : line, s.stale, s.setsInFlush, s.otherThread);
    if (g.verify) Log("cvars [%s]: VERIFY calls=%llu: same=%llu differ=%llu", when, s.same + s.differ, s.same, s.differ);
    n = 0;
    line[0] = 0;
    for (const CvEntry& e : g.entries)
        Append(line, sizeof line, n, " %s::%s %p %llu;", kClassLabels[e.cls], kGetterNames[e.getter], e.cvar,
               g.verify ? e.same + e.differ : e.answered);
    Log("cvars [%s]: per cvar (class::getter object calls):%s", when, n ? line : " none");
    n = 0;
    line[0] = 0;
    for (const Caller& c : s.callers) {
        if (!c.ret) break;
        char where[160] = "";
        DescribeAddress(c.ret, where, sizeof where);
        Append(line, sizeof line, n, " %s %llu;", where, c.calls);
    }
    Log("cvars [%s]: calls inside the flush from outside the renderer (left to CrySystem): %llu%s%s", when, s.outside,
        n ? ", from" : "", line);
}
#endif  // _WIN64

}  // namespace

bool CvarSnapshot_Locate(char* sys, CvarTarget* out, const char** why) {
#ifndef _WIN64
    (void)sys;
    (void)out;
    *why = "64-bit game only";
    return false;
#else
    CvarTarget t;
    static const char* const kNoClass[kCvClasses] = {"CXConsoleVariableIntRef's vtable not found",
                                                     "CXConsoleVariableString's vtable not found"};
    for (int c = 0; c < kCvClasses; ++c) {
        t.vtables[c] = FindRttiVtable(sys, kClassNames[c]);
        if (!t.vtables[c] ||
            !IsInImage(sys, reinterpret_cast<const char*>(t.vtables[c]), (kFirstSetter + kCvSetters) * sizeof(void*)))
            return *why = kNoClass[c], false;
        for (int i = 0; i < kCvGetters; ++i) {
            t.getters[c][i] = Entry(sys, CallTrace_Original(t.vtables[c][kFirstGetter + i]));
            if (!Matches(sys, t.getters[c][i], kGetterCode[c][i])) {
                static char buf[96];
                snprintf(buf, sizeof buf, "CXConsoleVariable%s::%s is not the expected code", kClassLabels[c],
                         kGetterNames[i]);
                return *why = buf, false;
            }
        }
        for (int i = 0; i < kCvSetters; ++i)
            if (!IsInCode(sys, Entry(sys, CallTrace_Original(t.vtables[c][kFirstSetter + i])), 1))
                return *why = "a cvar class's setter is not in CrySystem's code", false;
    }
    *out = t;
    return true;
#endif
}

void CvarSnapshot_Install(const wchar_t* moduleName, char* rendererBase, size_t rendererSize) {
#ifndef _WIN64
    (void)rendererBase;
    (void)rendererSize;
    if (ReadMode() != CMode::Off) Log("%ls: cvar snapshot not applied: 64-bit game only", moduleName);
#else
    if (g.installed) return;
    g.mode = ReadMode();
    if (g.mode == CMode::Off) return;
    g.rendererBase = rendererBase;
    g.rendererSize = rendererSize;
    g.verify = g.mode == CMode::Verify;
    g.installed = true;
    Log("%ls: cvars: mode=%s (set up at the first flush)", moduleName, ModeName(g.mode));
#endif
}

void CvarSnapshot_BeforeFlush() {
#ifdef _WIN64
    if (!g.installed) return;
    if (!g.setupTried) Setup();
    if (!g.ready || GetCurrentThreadId() != g.mainThread) return;
    const uint64_t t0 = __rdtsc();
    ++g.st.flushes;
    for (CvEntry& e : g.entries) Capture(e);
    g.st.captureCycles += __rdtsc() - t0;
    g.inFlush = g.live = true;
#endif
}

void CvarSnapshot_AfterFlush() {
#ifdef _WIN64
    g.inFlush = g.live = false;
#endif
}

void CvarSnapshot_Report(const char* when) {
#ifdef _WIN64
    if (g.installed && g.ready) Report(when);
#else
    (void)when;
#endif
}

}  // namespace optiperf
