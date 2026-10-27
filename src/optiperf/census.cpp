// Pointer census: a diagnostic for the render thread (Phase 2 of research/docs/cpu-multithreading-plan.md; details in
// research/docs/render-thread.md). x64 CryRenderD3D10.dll only.
//
// A render thread runs frame N's flush while the main thread prepares frame N+1, so whatever the flush reads must stay
// as it was at the hand-off. Since steps 1-11 the flush calls no other module's code, but it still reads memory through
// pointers: the skinning poses' bones were one such case (now copied, skin_prepass.cpp). The call tracer sees calls,
// not reads. On every census flush (one in CensusEvery), right before the flush, after the other parts' captures, the
// census scans
//   - the render objects (CRenderObject, 0x130 bytes) and the render elements of the items the flush will draw: level
//     1's lists, the shadow pre-pass's stashes, the reflections' captures;
//   - level 1's dynamic lights (the CDLight pointers, and the lights themselves);
//   - the renderer object;
//   - the renderer's statics (its image's writable sections: global lists, such as the far-tree sprites' quads);
//   - and the renderer's own blocks those point at, one level further.
// Each 8-byte aligned field holding the address of memory another module owns is counted by where it is (the kind of
// object scanned: its RTTI class, or the code that allocated it; the field's offset) and where it points: a block of
// another module's (its RTTI class, or the code that allocated it: the allocator's owner tags, [Allocator] Owners=on),
// or a writable part of another module's image (the read-only parts are code, vtables, constants). Then, for that
// flush, the pages of the blocks found are guarded (PAGE_GUARD): the first access to each page raises an exception
// (vectored handler), which the census counts against the channels pointing at the block accessed, with the code that
// accessed it, before letting the access go on. So the report says which pointers the flush follows, and from where.
// Accesses from other threads to those pages meanwhile are counted apart. Pointers into the 3D engine's and the
// animation's writable statics are watched as well, with a window of kImageWindow after each (a static has no block
// to tell its size); each page gets its own protection back afterwards (an image's can be copy-on-write).
//
// CensusChanges=on: the census flush's target blocks (up to 4 KB of each) are also copied at the hand-off, before
// they are guarded, and compared twice: right after the flush (what the flush itself wrote), then at the next flush's
// hand-off (what the main thread wrote in between: its next frame's update, traversal and optiperf's work before the
// flush, all of which a render thread would run beside the flush that read them). For each followed channel the
// report gives how many targets changed in each window, at which offsets, and how many were freed or reused by then.
//
// CensusReads=on (turns CensusChanges on): every access the flush makes to the guarded pages in the census flush (on
// the flush's thread: the main thread, or the worker with FlushThread=worker) is recorded, not only each page's first:
// the access is single-stepped (the trap flag) and the page guarded again right after it, at most kMaxRearms times per
// census flush. Each target keeps which of its 8-byte words were
// accessed (by the address the fault gives: a wide access counts at its first word), and the report gives, per
// followed channel, the words the flush accessed and those of them the next hand-off had written: what a render thread
// would read while the main thread writes it. Slow (two exceptions and a VirtualProtect per access). A page another
// thread took the guard off meanwhile stays unguarded for the rest of that flush, as without CensusReads.
//
// [ShadowPrepass] Census = off (default) | on     (env OPTIPERF_SHADOW_PREPASS_CENSUS)
//                 CensusEvery = 64                (env OPTIPERF_SHADOW_PREPASS_CENSUS_EVERY)
//                 CensusChanges = off (default) | on   (env OPTIPERF_SHADOW_PREPASS_CENSUS_CHANGES)
//                 CensusReads = off (default) | on     (env OPTIPERF_SHADOW_PREPASS_CENSUS_READS)

#include "census.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "allocator.h"
#include "config.h"
#include "flush_thread.h"
#include "log.h"
#include "reflection_prepass.h"
#include "shadow_prepass.h"
#include "shadow_prepass_lists.h"

namespace optiperf {
namespace {

#ifdef _WIN64
constexpr size_t kObjectBytes = 0x130;      // CRenderObject (shadow_prepass_lists.h compares up to here)
constexpr size_t kElementBytes = 0x100;     // a render element outside the allocator's blocks
constexpr size_t kLightBytes = 0x258;       // CDLight: 600 bytes (light_record.h)
constexpr size_t kRendererBytes = 0x2D000;  // the renderer object (its fields go to +0x2CEEC)
constexpr size_t kBlockBytes = 0x10000;     // at most this much of a renderer block, one level further
constexpr size_t kMaxBlocks = 40000;        // renderer blocks scanned per census
constexpr size_t kWatchBytes = 0x10000;     // at most this much of a target block guarded
constexpr size_t kMaxPages = 30000;         // pages guarded per census flush
constexpr size_t kPage = 0x1000;
constexpr int kRLights = 0xF40;  // the renderer's TArray<CDLight*> per recursion level, 16 bytes each
constexpr int kListsPerSide = 15, kRefractList = 14;
constexpr int kReaders = 6;  // reading code kept per channel
constexpr size_t kChangeBytes = 0x1000, kChangeWords = kChangeBytes / 8;  // at most this much of a target compared
constexpr size_t kMaxSnapshot = size_t(64) << 20;                       // the targets' bytes kept meanwhile
constexpr int kChangeOffsets = 12;                                       // offsets reported per channel and window
constexpr uint64_t kMaxRearms = 200000;  // CensusReads: pages guarded again per census flush, at most
constexpr size_t kImageWindow = 0x400;   // an image target (a static): this much after the pointer watched
constexpr int kPending = 8;              // CensusReads: pages one instruction can take the guard off
constexpr DWORD kTrapFlag = 0x100;       // EFLAGS.TF: a single-step exception right after the next instruction

const wchar_t* const kModuleNames[] = {
    L"CryRenderD3D10.dll", L"Cry3DEngine.dll", L"CryAnimation.dll", L"CryPhysics.dll", L"CrySystem.dll",
    L"CryEntitySystem.dll", L"CryAction.dll", L"CryGame.dll", L"CryAISystem.dll", L"CryScriptSystem.dll",
    L"CrySoundSystem.dll", L"CryNetwork.dll", L"CryFont.dll", L"CryInput.dll", L"CryMovie.dll", L"Crysis.exe"};
constexpr int kModules = int(sizeof kModuleNames / sizeof kModuleNames[0]);
constexpr int kRenderer = 0, kSelf = kModules;  // (the plugin: blocks it allocates for the renderer are the renderer's)

enum Src : uint8_t { kObject, kElement, kLightList, kLight, kRendererObject, kBlock, kStatic, kSrcKinds };
const char* const kSrcNames[kSrcKinds] = {"CRenderObject", "element",  "light list",     "CDLight",
                                          "renderer",      "block",    "renderer static"};

struct Module {
    const char* base = nullptr;
    size_t size = 0;
    char name[32] = "";
    std::vector<std::pair<uint32_t, uint32_t>> writable;  // its writable sections (RVA ranges)
};

// what a field points at: a block (its class, or the code that allocated it) or a writable part of a module's image
struct Target {
    int module = -1;  // the allocating code's module, or the image's; -1: unknown (a free block, another allocator)
    bool image = false;
    const char* id = nullptr;     // a block's vtable or allocating code; an image address (rounded to 16 bytes)
    bool vtable = false;          // id is a vtable (RTTI class)
    const char* block = nullptr;  // the block's start and size
    size_t size = 0;
};

struct Key {
    uint8_t src;
    int8_t module;
    bool image, vtable;
    uint32_t offset;
    const void* srcId;  // the source's vtable (elements, polymorphic blocks) or allocating code (other blocks)
    const char* dstId;  // Target::id
    bool operator==(const Key& o) const {
        return src == o.src && module == o.module && image == o.image && vtable == o.vtable && offset == o.offset &&
               srcId == o.srcId && dstId == o.dstId;
    }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        uint64_t h = uint64_t(uintptr_t(k.srcId)) * 0x9E3779B97F4A7C15ull;
        h ^= uint64_t(uintptr_t(k.dstId)) + 0x7F4A7C15ull + (h << 6) + (h >> 2);
        h ^= (uint64_t(k.offset) << 16 | uint64_t(k.src) << 8 | uint64_t(uint8_t(k.module))) + (h << 6) + (h >> 2);
        return size_t(h ^ (k.image ? 0x5555 : 0) ^ (k.vtable ? 0xAAAA0000 : 0));
    }
};
struct Value {
    uint64_t count = 0;
    uint32_t flushes = 0, lastFlush = 0;  // census flushes it was seen in
    const char* example = nullptr;
    // its targets read in the flush (the guarded pages): census flushes, accesses, and by which code
    uint64_t reads = 0, writes = 0;
    uint32_t readFlushes = 0, lastReadFlush = 0;
    const void* reader[kReaders] = {};
    uint64_t readerCount[kReaders] = {};
    // CensusChanges: its targets compared after census flushes that followed it; changed by the flush, changed by the
    // next hand-off, freed or reused by then; per 8-byte word (by the flush, then by the next hand-off), in how many
    uint64_t cTargets = 0, cFlush = 0, cBetween = 0, cGone = 0;
    std::vector<uint32_t> words;
    // CensusReads: of those, the ones the flush accessed, and the ones it accessed at words the next hand-off had
    // written; per word (accessed, then accessed and written), in how many
    uint64_t aTargets = 0, aBetween = 0;
    std::vector<uint32_t> accessWords;
};

struct Stats {
    uint64_t census = 0, sources[kSrcKinds] = {}, bytes = 0, fields = 0, foreign = 0, unknownOwner = 0, faults = 0;
    uint64_t cycles = 0, blocksCapped = 0, imageConst = 0, queueCapped = 0, staticBytes = 0;
    uint64_t pages = 0, pagesCapped = 0, guardFailed = 0, armCycles = 0;
    uint64_t mainFaults = 0, mainUnattributed = 0;
    uint64_t rearms = 0, rearmCapped = 0, stepsLost = 0;  // CensusReads
    std::atomic<uint64_t> otherFaults{0};
};

struct State {
    bool installed = false, on = false;
    uint32_t every = 64;
    const ShadowPrepassTarget* shadow = nullptr;
    DWORD mainThread = 0;
    uint64_t flushes = 0;
    Module mods[kModules + 1];
    std::unordered_map<Key, Value, KeyHash> channels;
    std::unordered_set<const char*> seen;               // this census's sources
    std::vector<std::pair<const char*, Target>> queue;  // renderer blocks to scan, one level further
    std::vector<uint8_t> buffer;
    // the watch: this census flush's target blocks (start -> the channel pointing at it), their pages (sorted)
    std::unordered_map<const char*, Value*> targets;
    std::map<const char*, Value*> imageTargets;  // pointers into the 3D engine's and the animation's statics
    std::vector<uintptr_t> pages;
    std::vector<DWORD> pageProtect;  // each guarded page's protection before (put back after)
    std::atomic<bool> watching{false};
    void* handler = nullptr;
    // CensusChanges: the census flush's targets, their bytes, and which comparison is due
    bool changes = false;
    int changesDue = 0;  // 1: after the census flush; 2: at the next flush's hand-off
    struct Watched {
        const char* block;
        size_t bytes, at;  // compared; where in the snapshot
        Value* channel;
        const void* site;  // its allocating code then (a block freed and reused since has another)
        uint64_t accessed[kChangeWords / 64];  // CensusReads: its words the census flush accessed
    };
    std::vector<Watched> watched;
    std::vector<uint8_t> snapshot;
    // CensusReads: a target's start -> its entry in watched (for the census flush); the pages the flush thread's
    // current instruction took the guard off; how many pages this census flush has guarded again
    bool reads = false;
    std::unordered_map<const char*, uint32_t> watchIndex;
    uintptr_t pending[kPending] = {};
    int pendingCount = 0;
    uint64_t rearms = 0;
    Stats st;
};
State g;

int ModuleOf(const char* p) {
    for (int i = 0; i <= kModules; ++i)
        if (g.mods[i].base && size_t(p - g.mods[i].base) < g.mods[i].size) return i;
    return -1;
}

bool Writable(int m, const char* p) {
    const uint32_t rva = uint32_t(p - g.mods[m].base);
    for (const auto& s : g.mods[m].writable)
        if (rva >= s.first && rva < s.second) return true;
    return false;
}

bool RendererSide(int module) { return module == kRenderer || module == kSelf; }

// the modules whose statics are watched: the 3D engine's and the animation's (not CrySystem's, whose globals every
// thread reads)
bool WatchedImage(int module) { return module == 1 || module == 2; }

// how much of an image target is watched: up to kImageWindow, inside its writable section
size_t ImageBytes(const char* p) {
    const int m = ModuleOf(p);
    if (m < 0) return 0;
    const uint32_t rva = uint32_t(p - g.mods[m].base);
    for (const auto& s : g.mods[m].writable)
        if (rva >= s.first && rva < s.second) return (std::min)(kImageWindow, size_t(s.second - rva));
    return 0;
}

// a copy of [p, p+n) into the buffer, or false if part of it can't be read
bool SafeCopy(const char* p, size_t n) {
    if (g.buffer.size() < n) g.buffer.resize(n);
    __try {
        std::memcpy(g.buffer.data(), p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t SafeQword(const char* p) {
    uintptr_t v = 0;
    __try {
        std::memcpy(&v, p, sizeof v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0;
    }
    return v;
}

// The class an object's MSVC RTTI names, from its vtable (x64 locators: image-relative); null if it isn't one.
const char* RttiName(const void* vt) {
    __try {
        const auto* col = static_cast<const uint32_t*>(static_cast<void* const*>(vt)[-1]);
        const int m = ModuleOf(reinterpret_cast<const char*>(col));
        if (m < 0 || col[0] > 1 || col[3] == 0 || col[3] >= g.mods[m].size) return nullptr;
        const char* name = g.mods[m].base + col[3] + 16;  // the type descriptor: vtable, spare, name
        return name[0] == '.' && name[1] == '?' ? name : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// a polymorphic object's vtable (its first field, pointing into a module's image at an RTTI-described vtable), or null
const char* VtableOf(const char* object) {
    const auto v = reinterpret_cast<const char*>(SafeQword(object));
    return v && ModuleOf(v) >= 0 && RttiName(v) ? v : nullptr;
}

bool Classify(uintptr_t v, Target* t) {
    if (v < 0x10000 || (v >> 47) != 0 || (v & 7) != 0) return false;  // (a pointer here is 8-byte aligned: not floats)
    const char* p = reinterpret_cast<const char*>(v);
    const int image = ModuleOf(p);
    if (image >= 0) {
        t->module = image;
        t->image = true;
        t->id = reinterpret_cast<const char*>(v & ~uintptr_t(15));
        if (!RendererSide(image) && !Writable(image, p)) {  // code, vtables, constants: never change
            ++g.st.imageConst;
            return false;
        }
        return true;
    }
    const void* site = nullptr;
    const char* start = nullptr;
    size_t size = 0;
    if (!Allocator_OwnerOf(p, &site, &start, &size)) return false;
    t->image = false;
    t->module = ModuleOf(static_cast<const char*>(site));
    t->block = start;
    t->size = size;
    const char* vt = t->module >= 0 ? VtableOf(start) : nullptr;
    t->vtable = vt != nullptr;
    t->id = vt ? vt : static_cast<const char*>(site);
    return true;
}

void Count(Src src, const void* srcId, uint32_t offset, const Target& t, const char* example) {
    Key k{uint8_t(src), int8_t(t.module), t.image, t.vtable, offset, srcId, t.id};
    Value& v = g.channels[k];
    ++v.count;
    if (v.lastFlush != uint32_t(g.st.census)) {
        v.lastFlush = uint32_t(g.st.census);
        ++v.flushes;
    }
    if (!v.example) v.example = example;
    if (!t.image && t.block) g.targets.emplace(t.block, &v);  // (watched: the first channel found pointing at it)
    else if (t.image && WatchedImage(t.module)) g.imageTargets.emplace(example, &v);
}

// one object: each aligned field pointing at another module's memory counted (at its offset, plus offsetBase: a
// static's is its RVA); the renderer's blocks it points at queued for the next level
void Scan(const char* p, size_t n, Src src, const void* srcId, bool queue, uint32_t offsetBase = 0) {
    if (!p || !g.seen.insert(p).second) return;
    ++g.st.sources[src];
    if (!SafeCopy(p, n)) {
        ++g.st.faults;
        return;
    }
    g.st.bytes += n;
    const uint8_t* b = g.buffer.data();
    for (size_t off = 0; off + 8 <= n; off += 8) {
        uintptr_t v;
        std::memcpy(&v, b + off, 8);
        Target t;
        if (!Classify(v, &t)) continue;
        ++g.st.fields;
        if (t.module < 0) {
            ++g.st.unknownOwner;
            continue;
        }
        if (RendererSide(t.module)) {
            if (queue && !t.image && t.block) g.queue.emplace_back(t.block, t);
            continue;
        }
        ++g.st.foreign;
        Count(src, srcId, uint32_t(off) + offsetBase, t, reinterpret_cast<const char*>(v));
    }
}

// an element's size: its block's, if the allocator has it
size_t ElementBytes(const char* e) {
    const void* site;
    const char* start;
    size_t size;
    if (Allocator_OwnerOf(e, &site, &start, &size) && start == e) return (std::min)(size, kBlockBytes);
    return kElementBytes;
}

void ScanItem(const sp::RendItem& it) {
    if (it.obj) Scan(it.obj, kObjectBytes, kObject, nullptr, true);
    if (it.elem) Scan(it.elem, ElementBytes(it.elem), kElement, VtableOf(it.elem), true);
}

void ScanStash(const sp::Stash& s, void* ctx) {
    const bool reflection = ctx != nullptr;
    const sp::RendItem* it = s.items.data();
    for (int k = 0; k < sp::kLists; ++k) {
        const uint32_t n = s.counts[k];
        if (!(reflection && k % kListsPerSide == kRefractList))
            for (uint32_t i = 0; i < n; ++i) ScanItem(it[i]);
        it += n;
    }
}

// The dynamic lights of the level (pointers, then each light): the list's slot is the field
void ScanLights(char* r, int level) {
    char* list = r + kRLights + level * 16;
    char* const* elems = *reinterpret_cast<char* const* const*>(list);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(list + 8);
    if (!elems || count > 4096) return;
    ++g.st.sources[kLightList];
    for (uint32_t i = 0; i < count; ++i) {
        Target t;
        if (Classify(uintptr_t(elems[i]), &t) && t.module >= 0 && !RendererSide(t.module)) {
            ++g.st.foreign;
            Count(kLightList, nullptr, 0, t, elems[i]);
        }
        Scan(elems[i], kLightBytes, kLight, nullptr, true);
    }
}

// --- the watch: the target blocks' pages guarded for the flush ------------------------------------------------------
bool Guarded(uintptr_t page) { return std::binary_search(g.pages.begin(), g.pages.end(), page); }

void Attribute(const char* data, const void* ip, bool write) {
    char* block = nullptr;
    size_t size = 0;
    Value* v = nullptr;
    const char* start = nullptr;  // the target's
    if (Allocator_BlockOf(data, &block, &size)) {
        const auto it = g.targets.find(block + 16);  // (the user start: the allocator's header is 16 bytes)
        if (it != g.targets.end()) {
            v = it->second;
            start = block + 16;
        }
    } else if (!g.imageTargets.empty()) {  // a static: the nearest image target at or below it
        auto it = g.imageTargets.upper_bound(data);
        if (it != g.imageTargets.begin() && size_t(data - (--it)->first) < kImageWindow) {
            v = it->second;
            start = it->first;
        }
    }
    ++g.st.mainFaults;
    if (!v) {
        ++g.st.mainUnattributed;  // another block on a guarded page
        return;
    }
    if (g.reads) {  // CensusReads: the word accessed
        const auto wi = g.watchIndex.find(start);
        const size_t o = size_t(data - start) / 8;
        if (wi != g.watchIndex.end() && o < kChangeWords)
            g.watched[wi->second].accessed[o / 64] |= uint64_t(1) << (o % 64);
    }
    if (write)
        ++v->writes;
    else
        ++v->reads;
    if (v->lastReadFlush != uint32_t(g.st.census)) {
        v->lastReadFlush = uint32_t(g.st.census);
        ++v->readFlushes;
    }
    for (int i = 0; i < kReaders; ++i) {
        if (v->reader[i] == ip || !v->reader[i]) {
            v->reader[i] = ip;
            ++v->readerCount[i];
            return;
        }
    }
}

// CensusReads: the flush's instruction that took the guard off census pages is done: they are guarded again (while
// the watch is on). Any single step on the flush's thread is taken for this one's: nothing else here sets the trap
// flag (no debugger), and one left to the system would end the game.
LONG OnStep(CONTEXT* c) {
    if (!g.reads || !OnFlushThread()) return EXCEPTION_CONTINUE_SEARCH;
    if (g.watching.load(std::memory_order_acquire))
        for (int i = 0; i < g.pendingCount; ++i) {
            const auto at = std::lower_bound(g.pages.begin(), g.pages.end(), g.pending[i]);
            const DWORD protect = at != g.pages.end() && *at == g.pending[i]
                                      ? g.pageProtect[size_t(at - g.pages.begin())]
                                      : DWORD(PAGE_READWRITE);
            DWORD old;
            if (VirtualProtect(reinterpret_cast<void*>(g.pending[i]), kPage, protect | PAGE_GUARD, &old)) ++g.rearms;
        }
    g.pendingCount = 0;
    c->EFlags &= ~kTrapFlag;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// A guard fault on one of the census's pages is always let go on (whether or not the watch still attributes it: one
// unhandled would end the thread); the page list changes only in Arm, a census later, never while pages are guarded.
// With CensusReads, the flush's faulting instruction is single-stepped, and the page guarded again after it. The flush's
// thread is the main thread unless FlushThread=worker; the census's own work stays on the main thread.
LONG CALLBACK OnGuard(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode == STATUS_SINGLE_STEP) return OnStep(e->ContextRecord);
    if (r->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t data = uintptr_t(r->ExceptionInformation[1]);
    const uintptr_t page = data & ~uintptr_t(kPage - 1);
    if (!Guarded(page)) return EXCEPTION_CONTINUE_SEARCH;  // (someone else's guard page)
    if (g.watching.load(std::memory_order_acquire)) {  // (else a fault from before the disarm, handled late)
        if (OnFlushThread()) {
            Attribute(reinterpret_cast<const char*>(data), r->ExceptionAddress, r->ExceptionInformation[0] == 1);
            if (g.reads && g.rearms < kMaxRearms && g.pendingCount < kPending) {
                g.pending[g.pendingCount++] = page;
                e->ContextRecord->EFlags |= kTrapFlag;
            }
        } else {
            g.st.otherFaults.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;  // (the system has taken the guard off that page)
}

// The pages of this census's target blocks (from their allocator block start, at most kWatchBytes of each), guarded.
void Arm() {
    const uint64_t t0 = __rdtsc();
    g.pages.clear();
    for (const auto& t : g.targets) {
        const void* site;
        const char* start;
        size_t bytes;
        if (!Allocator_OwnerOf(t.first, &site, &start, &bytes)) continue;
        const uintptr_t from = (uintptr_t(start) - 16) & ~uintptr_t(kPage - 1);
        const uintptr_t to = uintptr_t(start) + (std::min)(bytes, kWatchBytes);
        for (uintptr_t p = from; p < to; p += kPage) g.pages.push_back(p);
    }
    for (const auto& t : g.imageTargets) {
        const size_t n = ImageBytes(t.first);
        if (!n) continue;
        for (uintptr_t p = uintptr_t(t.first) & ~uintptr_t(kPage - 1); p < uintptr_t(t.first) + n; p += kPage)
            g.pages.push_back(p);
    }
    std::sort(g.pages.begin(), g.pages.end());
    g.pages.erase(std::unique(g.pages.begin(), g.pages.end()), g.pages.end());
    if (g.pages.size() > kMaxPages) {
        g.st.pagesCapped += g.pages.size() - kMaxPages;
        g.pages.resize(kMaxPages);
    }
    g.pageProtect.assign(g.pages.size(), DWORD(PAGE_READWRITE));
    MEMORY_BASIC_INFORMATION mbi;
    for (size_t i = 0; i < g.pages.size(); ++i)  // (a heap page's is read-write; an image's can be copy-on-write)
        if (VirtualQuery(reinterpret_cast<void*>(g.pages[i]), &mbi, sizeof mbi) == sizeof mbi)
            g.pageProtect[i] = mbi.Protect & ~DWORD(PAGE_GUARD);
    g.watching.store(true, std::memory_order_release);
    for (size_t i = 0; i < g.pages.size(); ++i) {
        DWORD old;
        if (VirtualProtect(reinterpret_cast<void*>(g.pages[i]), kPage, g.pageProtect[i] | PAGE_GUARD, &old))
            ++g.st.pages;
        else
            ++g.st.guardFailed;
    }
    g.st.armCycles += __rdtsc() - t0;
}

void Disarm() {
    if (!g.watching.load(std::memory_order_acquire)) return;
    for (size_t i = 0; i < g.pages.size(); ++i) {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(g.pages[i]), kPage, g.pageProtect[i], &old);
    }
    g.watching.store(false, std::memory_order_release);
    if (g.reads) {  // CensusReads
        g.st.rearms += g.rearms;
        if (g.rearms >= kMaxRearms) ++g.st.rearmCapped;
        if (g.pendingCount) ++g.st.stepsLost;  // (an instruction whose single step never came)
        g.rearms = 0;
        g.pendingCount = 0;
    }
}

// CensusChanges: the target blocks' bytes, at the hand-off (before they are guarded)
void SnapshotTargets() {
    g.watched.clear();
    g.watchIndex.clear();
    if (g.reads) g.watchIndex.reserve(g.targets.size());
    if (g.snapshot.size() < kMaxSnapshot) g.snapshot.resize(kMaxSnapshot);
    size_t used = 0;
    for (const auto& t : g.targets) {
        const void* site;
        const char* start;
        size_t bytes;
        if (!Allocator_OwnerOf(t.first, &site, &start, &bytes) || start != t.first) continue;
        const size_t n = (std::min)(bytes, kChangeBytes) & ~size_t(7);
        if (!n || used + n > g.snapshot.size() || !SafeCopy(t.first, n)) continue;
        std::memcpy(g.snapshot.data() + used, g.buffer.data(), n);
        if (g.reads) g.watchIndex.emplace(t.first, uint32_t(g.watched.size()));
        g.watched.push_back({t.first, n, used, t.second, site, {}});
        used += n;
    }
    for (const auto& t : g.imageTargets) {  // (a static: no allocating code, never freed)
        const size_t n = ImageBytes(t.first) & ~size_t(7);
        if (!n || used + n > g.snapshot.size() || !SafeCopy(t.first, n)) continue;
        std::memcpy(g.snapshot.data() + used, g.buffer.data(), n);
        if (g.reads) g.watchIndex.emplace(t.first, uint32_t(g.watched.size()));
        g.watched.push_back({t.first, n, used, t.second, nullptr, {}});
        used += n;
    }
    g.changesDue = 1;
}

// CensusChanges: the targets against the snapshot. Window 0, right after the census flush: those of the channels it
// followed (the others are dropped), whose bytes then become the snapshot. Window 1, at the next flush's hand-off.
// CensusReads: in window 0 the words each target had accessed, in window 1 those of them that changed.
void CompareTargets(int window) {
    size_t kept = 0;
    for (size_t i = 0; i < g.watched.size(); ++i) {
        const State::Watched w = g.watched[i];
        Value& v = *w.channel;
        if (window == 0) {
            if (v.lastReadFlush != uint32_t(g.st.census)) continue;
            ++v.cTargets;
        } else if (w.site) {  // (a static has none: it stays)
            const void* site;
            const char* start;
            size_t bytes;
            if (!Allocator_OwnerOf(w.block, &site, &start, &bytes) || start != w.block || site != w.site) {
                ++v.cGone;
                continue;
            }
        }
        if (!SafeCopy(w.block, w.bytes)) {
            ++v.cGone;
            continue;
        }
        if (v.words.empty()) v.words.assign(2 * kChangeWords, 0);
        if (g.reads && v.accessWords.empty()) v.accessWords.assign(2 * kChangeWords, 0);
        const uint8_t* now = g.buffer.data();
        uint8_t* was = g.snapshot.data() + w.at;
        bool changed = false, accessed = false;
        for (size_t o = 0; o + 8 <= w.bytes; o += 8) {
            const bool read = (w.accessed[o / 512] >> (o / 8 % 64) & 1) != 0;
            if (window == 0 && read) {
                ++v.accessWords[o / 8];
                accessed = true;
            }
            if (std::memcmp(now + o, was + o, 8) != 0) {
                ++v.words[size_t(window) * kChangeWords + o / 8];
                changed = true;
                if (window == 1 && read) {
                    ++v.accessWords[kChangeWords + o / 8];
                    accessed = true;
                }
            }
        }
        if (changed) ++(window == 0 ? v.cFlush : v.cBetween);
        if (accessed) ++(window == 0 ? v.aTargets : v.aBetween);
        if (window == 0) {
            std::memcpy(was, now, w.bytes);
            g.watched[kept++] = w;
        }
    }
    if (window == 0) {
        g.watched.resize(kept);
        g.watchIndex.clear();
        g.changesDue = 2;
    } else {
        g.watched.clear();
        g.changesDue = 0;
    }
}

void Census(char* r) {
    const uint64_t t0 = __rdtsc();
    ++g.st.census;
    g.seen.clear();
    g.queue.clear();
    g.targets.clear();
    g.imageTargets.clear();
    // level 1's part of the render lists, the shadow pre-pass's stashes, the reflections' captures
    const auto* lists = reinterpret_cast<const sp::ItemList*>(g.shadow->lists);
    const uint32_t* starts = g.shadow->starts;
    for (int k = 0; k < sp::kLists; ++k)
        for (uint32_t i = starts[k]; i < lists[k].count; ++i) ScanItem(lists[k].elems[i]);
    ShadowPrepass_ForEachStash(&ScanStash, nullptr);
    static int reflection = 1;
    ReflectionPrepass_ForEachCapture(&ScanStash, &reflection);
    ScanLights(r, *g.shadow->level);
    Scan(r, kRendererBytes, kRendererObject, nullptr, true);
    // the renderer's statics: its image's writable sections (a field's offset is its RVA)
    for (const auto& w : g.mods[kRenderer].writable) {
        Scan(g.mods[kRenderer].base + w.first, w.second - w.first, kStatic, nullptr, true, w.first);
        g.st.staticBytes += w.second - w.first;
    }
    // the renderer's blocks those point at: one level further
    size_t done = 0;
    for (size_t i = 0; i < g.queue.size(); ++i) {
        const Target& t = g.queue[i].second;
        if (g.seen.count(t.block)) continue;
        if (done == kMaxBlocks) {
            ++g.st.queueCapped;
            continue;
        }
        ++done;
        if (t.size > kBlockBytes) ++g.st.blocksCapped;
        const char* vt = VtableOf(t.block);
        Scan(t.block, (std::min)(t.size, kBlockBytes), kBlock, vt ? static_cast<const void*>(vt) : t.id, false);
    }
    if (g.changes) SnapshotTargets();
    g.st.cycles += __rdtsc() - t0;
    if (g.handler) Arm();  // (never without the handler: a guard fault nobody takes ends the thread)
}

void Append(char* line, size_t size, size_t& n, const char* fmt, ...) {
    if (n >= size) return;
    va_list a;
    va_start(a, fmt);
    const int k = vsnprintf(line + n, size - n, fmt, a);
    va_end(a);
    if (k > 0) n = (std::min)(size - 1, n + size_t(k));
}

// "Module+0xRVA" for an address in one of the modules
void Where(const void* p, char* out, size_t size) {
    const int m = ModuleOf(static_cast<const char*>(p));
    if (m < 0)
        snprintf(out, size, "%p", p);
    else
        snprintf(out, size, "%s+0x%X", g.mods[m].name, unsigned(static_cast<const char*>(p) - g.mods[m].base));
}

// a label for a source or target id: its RTTI class (a vtable), or the module and offset
void Label(const void* id, char* out, size_t size) {
    const char* name = id && ModuleOf(static_cast<const char*>(id)) >= 0 ? RttiName(id) : nullptr;
    if (name) {
        char where[96];
        Where(id, where, sizeof where);
        snprintf(out, size, "%s (%s)", name, where);
    } else {
        Where(id, out, size);
    }
}

// the most counted words of a channel's kChangeWords counts, " +0xOFF:count" each
void HotWords(const uint32_t* counts, char* out, size_t size) {
    std::vector<std::pair<uint32_t, size_t>> hot;
    for (size_t o = 0; o < kChangeWords; ++o)
        if (counts[o]) hot.push_back({counts[o], o});
    std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    size_t q = 0;
    out[0] = 0;
    for (size_t h = 0; h < hot.size() && h < size_t(kChangeOffsets); ++h)
        Append(out, size, q, " +0x%zX:%u", hot[h].second * 8, hot[h].first);
    if (hot.size() > size_t(kChangeOffsets)) Append(out, size, q, " (%zu offsets)", hot.size());
}

void Report(const char* when) {
    const Stats& s = g.st;
    const double n = double(s.census ? s.census : 1);
    Log("census [%s]: census flushes %llu of %llu (one in %u) | scanned: objects %llu, elements %llu, light lists %llu, "
        "lights %llu, renderer %llu, renderer statics %.1f MB, renderer blocks %llu (capped at 64 KB: %llu; left out "
        "past %zu per census: %llu); %.1f MB, %llu fields pointing at engine memory (writable parts of images, blocks), "
        "%llu of them at other modules' (no owner known %llu); pointers at read-only image parts left out %llu; "
        "unreadable %llu | %.2f ms per census",
        when, s.census, g.flushes, g.every, s.sources[kObject], s.sources[kElement], s.sources[kLightList],
        s.sources[kLight], s.sources[kRendererObject], double(s.staticBytes) / 1048576.0 / n, s.sources[kBlock],
        s.blocksCapped, kMaxBlocks, s.queueCapped, double(s.bytes) / 1048576.0, s.fields, s.foreign, s.unknownOwner,
        s.imageConst, s.faults, double(s.cycles) / 3.0e6 / n);
    Log("census [%s]: the watch: %.0f pages guarded per census flush (capped %llu, failed %llu; %.2f ms to arm) | %s "
        "in the flush %llu (on other blocks of those pages %llu); from other threads meanwhile %llu",
        when, double(s.pages) / n, s.pagesCapped, s.guardFailed, double(s.armCycles) / 3.0e6 / n,
        g.reads ? "accesses" : "first accesses", s.mainFaults, s.mainUnattributed, s.otherFaults.load());
    if (g.reads)
        Log("census [%s]: every access (CensusReads): pages guarded again %.0f per census flush; census flushes that "
            "stopped at %llu: %llu; single steps that never came %llu",
            when, double(s.rearms) / n, static_cast<unsigned long long>(kMaxRearms), s.rearmCapped, s.stepsLost);
    std::vector<std::pair<Key, Value>> top(g.channels.begin(), g.channels.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
        if ((a.second.reads + a.second.writes > 0) != (b.second.reads + b.second.writes > 0))
            return a.second.reads + a.second.writes > 0;  // (the ones the flush follows first)
        return a.second.count > b.second.count;
    });
    size_t followed = 0;
    for (const auto& c : top)
        if (c.second.reads + c.second.writes) ++followed;
    const size_t shown = (std::min)(top.size(), (std::max)(followed, size_t(60)) + size_t(20));
    Log("census [%s]: %zu channels, %zu of them followed in the flush (the guarded blocks accessed); the %zu shown: "
        "where -> what it points at: per census flush, in how many | followed: %s (reads/writes), in how many census "
        "flushes, by which code",
        when, top.size(), followed, shown, g.reads ? "accesses" : "first accesses");
    for (size_t i = 0; i < shown; ++i) {
        const Key& k = top[i].first;
        const Value& v = top[i].second;
        char from[224] = "", to[224] = "", by[512] = "";
        size_t m = 0;
        if (k.src == kElement || k.src == kBlock) {
            char label[200];
            Label(k.srcId, label, sizeof label);
            Append(from, sizeof from, m, "%s %s", kSrcNames[k.src], label);
        } else {
            Append(from, sizeof from, m, "%s", kSrcNames[k.src]);
        }
        Append(from, sizeof from, m, " +0x%X", k.offset);
        char label[200];
        Label(k.dstId, label, sizeof label);
        snprintf(to, sizeof to, "%s %s", k.image ? "static" : k.vtable ? "object" : "block from", label);
        m = 0;
        if (v.reads + v.writes) {
            Append(by, sizeof by, m, " | followed: %llu/%llu in %u", v.reads, v.writes, v.readFlushes);
            for (int r = 0; r < kReaders && v.reader[r]; ++r) {
                char where[96];
                Where(v.reader[r], where, sizeof where);
                Append(by, sizeof by, m, "%s %s %llu", r ? "," : " by", where, v.readerCount[r]);
            }
        }
        Log("census [%s]:   %s -> %s: %.2f, in %u of %llu (e.g. %p)%s", when, from, to, double(v.count) / n, v.flushes,
            s.census, static_cast<const void*>(v.example), by);
        if (v.cTargets && !v.words.empty()) {  // CensusChanges
            char words[2][400];
            HotWords(v.words.data(), words[0], sizeof words[0]);
            HotWords(v.words.data() + kChangeWords, words[1], sizeof words[1]);
            Log("census [%s]:     changes: of %llu targets compared, %llu written by the flush (%s ), %llu by the next "
                "hand-off (%s ), %llu freed or reused by then",
                when, v.cTargets, v.cFlush, words[0][0] ? words[0] : " none", v.cBetween, words[1][0] ? words[1] : " none",
                v.cGone);
        }
        if (v.cTargets && !v.accessWords.empty()) {  // CensusReads
            char words[2][400];
            HotWords(v.accessWords.data(), words[0], sizeof words[0]);
            HotWords(v.accessWords.data() + kChangeWords, words[1], sizeof words[1]);
            Log("census [%s]:     accesses: %llu of those targets accessed by the flush (%s ), %llu of them at words the "
                "next hand-off had written (%s )",
                when, v.aTargets, words[0][0] ? words[0] : " none", v.aBetween, words[1][0] ? words[1] : " none");
        }
    }
}
#endif  // _WIN64

}  // namespace

void Census_Install(const wchar_t* moduleName, char* rendererBase, size_t rendererSize, const ShadowPrepassTarget& sp) {
    (void)rendererBase;
    (void)rendererSize;
#ifndef _WIN64
    (void)sp;
    if (ConfigString(L"ShadowPrepass", L"Census", L"OPTIPERF_SHADOW_PREPASS_CENSUS", L"off") != L"off")
        Log("%ls: pointer census not run: 64-bit game only", moduleName);
#else
    if (g.installed) return;
    const std::wstring v = ConfigString(L"ShadowPrepass", L"Census", L"OPTIPERF_SHADOW_PREPASS_CENSUS", L"off");
    if (!(_wcsicmp(v.c_str(), L"on") == 0 || v == L"1")) return;
    if (!Allocator_TracksOwners()) {
        Log("%ls: pointer census not run: needs the allocator's owner tags ([Allocator] Owners=on)", moduleName);
        return;
    }
    const std::wstring every =
        ConfigString(L"ShadowPrepass", L"CensusEvery", L"OPTIPERF_SHADOW_PREPASS_CENSUS_EVERY", L"64");
    g.every = (std::max)(1u, unsigned(wcstoul(every.c_str(), nullptr, 10)));
    g.shadow = &sp;
    const std::wstring changes =
        ConfigString(L"ShadowPrepass", L"CensusChanges", L"OPTIPERF_SHADOW_PREPASS_CENSUS_CHANGES", L"off");
    g.changes = _wcsicmp(changes.c_str(), L"on") == 0 || changes == L"1";
    const std::wstring reads =
        ConfigString(L"ShadowPrepass", L"CensusReads", L"OPTIPERF_SHADOW_PREPASS_CENSUS_READS", L"off");
    g.reads = _wcsicmp(reads.c_str(), L"on") == 0 || reads == L"1";
    g.changes = g.changes || g.reads;
    g.installed = true;
    Log("%ls: pointer census: on, one flush in %u%s (the modules are found at the first flush)", moduleName, g.every,
        g.reads ? ", with the targets' changes and every access" : g.changes ? ", with the targets' changes" : "");
#endif
}

void Census_BeforeFlush(char* renderer) {
#ifdef _WIN64
    if (!g.installed) return;
    if (!g.on) {  // the first flush: the modules (all loaded by now), their writable sections, the thread
        g.on = true;
        g.mainThread = GetCurrentThreadId();
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Census_BeforeFlush), &self);
        for (int i = 0; i <= kModules; ++i) {
            HMODULE h = i < kModules ? GetModuleHandleW(kModuleNames[i]) : self;
            if (!h) continue;
            auto* base = reinterpret_cast<char*>(h);
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
            Module& m = g.mods[i];
            m.base = base;
            m.size = nt->OptionalHeader.SizeOfImage;
            if (i < kModules)
                snprintf(m.name, sizeof m.name, "%ls", kModuleNames[i]);
            else
                snprintf(m.name, sizeof m.name, "optiperf.asi");
            const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
            for (unsigned k = 0; k < nt->FileHeader.NumberOfSections; ++k, ++s)
                if (s->Characteristics & IMAGE_SCN_MEM_WRITE)
                    m.writable.emplace_back(s->VirtualAddress, s->VirtualAddress + (std::max)(s->Misc.VirtualSize, s->SizeOfRawData));
        }
        g.channels.reserve(8192);
        g.buffer.resize(kRendererBytes);
        g.handler = AddVectoredExceptionHandler(1, &OnGuard);
        if (!g.handler) Log("pointer census: no exception handler: the watch is off");
    }
    if (GetCurrentThreadId() != g.mainThread) return;
    if (g.changesDue == 2) CompareTargets(1);  // (the census flush before this one: what was written since)
    if (g.flushes++ % g.every != g.every / 2) return;  // (off the level's first flushes)
    Census(renderer);
#else
    (void)renderer;
#endif
}

void Census_AfterFlush() {
#ifdef _WIN64
    if (!g.installed || GetCurrentThreadId() != g.mainThread) return;
    Disarm();
    if (g.changesDue == 1) CompareTargets(0);
#endif
}

void Census_Report(const char* when) {
#ifdef _WIN64
    if (g.installed && g.on) Report(when);
#else
    (void)when;
#endif
}

}  // namespace optiperf
