// Light snapshot: see light_snapshot.h; the copies and their bookkeeping are in light_record.h.
//
// Recording. EF_ADDDlight is hooked. On the main thread each light it is given is recorded with the recursion level it
// is added at: the traversal's at level 1, the reflections' dry run's at level 2. Recording starts after the first
// flush, once the main thread is known. Two records are used in turn: the hand-off takes one, and the main thread
// records into the other (on a render thread, its next traversal adds its lights while the flush runs).
//
// The hand-off, at the outer EF_EndEf3D's entry, after the other parts' captures and before the census:
//   - every recorded light is copied, then every shadow frustum the copied lights or the drawn objects point at (each
//     with a pristine copy, light_record.h);
//   - into an arena: each frustum's caster list, each light's frustum array and each drawn object's frustum list,
//     their pointers at the copies; the copies point at those;
//   - pointed at the copies: levels 1 and 2's light lists; the reflections' captured lights, which the flush puts
//     back into level 2's list when it replays them; the m_pLight and frustum list of every render object the flush
//     will draw (level 1's items, the shadow pre-pass's stashes, the reflections' captures); the renderer's sun, light
//     passes and current frustum; the shadow pre-pass's stashes, which PrepareDepthMap finds by their frustum and
//     checks by its caster list.
// After the flush, the bytes it wrote into the copies are written into the originals, a word at a time, but for words
// the original changed meanwhile (another thread: on a render thread, the next frame, whose writes come after the
// flush's in vanilla's order too), and the pointers are put back, including those the flush itself left at copies in
// the renderer's sun, light passes and current frustum.
//
// Lights the flush adds itself, on its thread (a traversal left in it, such as a reflection the dry run couldn't take),
// keep vanilla's pointers, and are counted.
//
// verify: as on, plus two searches. At the hand-off, for pointers into the lights and frustums that the redirect
// missed: in the items' objects and elements, the renderer object, the copies and the arena. After the flush, for
// pointers into the copies or the arena kept in the renderer object and the items' objects. Also which words of the
// copies the flush wrote, whether any original changed while it ran (none should: the flush works on the copies; the
// first that did are logged with what they are), and whether the copied lists still match their originals.

#include "light_snapshot.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#include "allocator.h"
#include "config.h"
#include "flush_thread.h"
#include "hooks.h"
#include "key_index.h"
#include "light_record.h"
#include "log.h"
#include "reflection_prepass.h"
#include "scan.h"
#include "shadow_prepass.h"
#include "shadow_prepass_lists.h"

namespace optiperf {
namespace {

enum class LMode { Off, On, Verify };

LMode ReadMode() {
    const std::wstring v = ConfigString(L"ShadowPrepass", L"Lights", L"OPTIPERF_SHADOW_PREPASS_LIGHTS", L"off");
    if (_wcsicmp(v.c_str(), L"on") == 0 || v == L"1") return LMode::On;
    if (_wcsicmp(v.c_str(), L"verify") == 0) return LMode::Verify;
    return LMode::Off;
}

#ifdef _WIN64
const char* ModeName(LMode m) { return m == LMode::On ? "on" : m == LMode::Verify ? "verify" : "off"; }

// EF_ADDDlight's start: a null light (a warning through the log), EF_IsFakeDLight (slot +0x3A0), then the light's
// index: the current level's count (renderer +0xF48 + level x 16), at most 32
const char* const kAddLightCode =
    "40 56 57 48 83 EC 78 48 85 D2 48 8B FA 48 8B F1 75 1B 48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B 01 48 83 C4 "
    "78 5F 5E 48 FF 60 10 48 8B 01 48 89 9C 24 90 00 00 00 48 89 AC 24 98 00 00 00 4C 89 A4 24 A0 00 00 00 FF 90 A0 03 "
    "00 00 45 33 E4 84 C0 74 08 C7 07 FF FF FF FF EB ?? 48 63 05 ?? ?? ?? ?? 48 03 C0 8B 8C C6 48 0F 00 00 83 F9 20";
// its coronas' objects: m_pObject[level] at the light's +0x118 (level x 0x20)...
const char* const kObjectsCode = "48 8D 9C 39 18 01 00 00";
// ... and on each object the light's matrix (+0xE4, 48 bytes, to the object's +0x14), m_pLight (+0x128) and the light's
// bit (1 << m_Id, +0x8C)
const char* const kCoronaCode =
    "48 8D 97 E4 00 00 00 41 B8 30 00 00 00 48 83 C1 14 E8 ?? ?? ?? ?? 4C 8B 1B BA 01 00 00 00 49 89 BB 28 01 00 00 8B "
    "0F 48 8B 03 D3 E2 89 90 8C 00 00 00";
constexpr size_t kAddLightSearch = 0x400;  // how far into EF_ADDDlight the two are looked for (at +0x1F9, +0x2DD)
// the per-list setup's sun: the level's first light (renderer +0xF40 + level x 16) with DLF_SUN (bit 18) and not
// DLF_FAKE (bit 17), kept at renderer +0x141B0
const char* const kSunCode =
    "48 8D 81 F4 00 00 00 48 03 C0 48 8B 04 C3 48 8B 0C 02 8B 81 D4 01 00 00 0F BA E0 11 72 ?? 0F BA E0 12 73 ?? 48 89 "
    "8B B0 41 01 00";
// the light passes the flush groups an item's lights into: 32 groups of 0x40 bytes at renderer +0xF80, each up to 4
// CDLight pointers and their count at +0x24
const char* const kGroupsBase = "4C 8D A3 80 0F 00 00 89 BC 24 C0 02 00 00 0F 84";
const char* const kGroupsLoop =
    "8B BC 24 C0 02 00 00 41 83 C5 01 89 55 24 83 C7 01 48 83 C6 08 48 83 C5 40 44 2B F2 41 83 FD 20";
// the renderer's CDLight constructor: 600 bytes cleared, after the shader item (+0x1B0); then the frustum angle
// (+0x1A0), the radius (+0x50), DLF_LIGHTSOURCE (+0x1D4), m_NumCM (+0x68), m_n3DEngineLightId (+0x200)
const char* const kLightCtorCode =
    "48 89 5C 24 08 57 48 83 EC 20 33 FF 33 D2 41 B8 58 02 00 00 48 8B D9 C7 81 C0 01 00 00 FF FF FF FF 48 89 B9 B0 01 "
    "00 00 48 89 B9 B8 01 00 00 89 B9 C4 01 00 00 E8 ?? ?? ?? ?? C7 83 A0 01 00 00 00 00 34 42 89 BB A4 01 00 00 C7 43 "
    "50 00 00 80 40 C7 43 58 00 00 80 3F C7 43 3C 00 00 80 3F C7 83 D4 01 00 00 00 00 01 00 C7 43 68 FF FF FF FF C7 83 "
    "00 02 00 00 FF FF FF FF";
constexpr size_t kGroupsSearch = 0x400;  // how far after the base the loops are looked for (at +0xC0, +0x1D0)
constexpr int kRSun = 0x141B0;
constexpr int kRGroups = 0xF80, kGroups = 32, kGroupBytes = 0x40, kGroupLights = 4;
constexpr int kRFrustum = 0x1788;  // the frustum PrepareDepthMap is drawing (set by it, read by the shadow flush)

using AddLightFn = void (*)(char* renderer, char* light);

constexpr size_t kMaxLights = 512, kMaxFrustums = 512;  // copied per flush (the lists hold at most 32 lights a level)
constexpr size_t kArenaBytes = 8u << 20;                // the copied lists
constexpr uint32_t kMaxListed = 64;                     // a longer light list is not one
constexpr uint32_t kMaxCasters = 1u << 16, kMaxObjectFrustums = 64;
constexpr size_t kRendererBytes = 0x2CEF0;  // the renderer object's fields (the census found them up to +0x2CEEC)
constexpr size_t kElementBytes = 0x100;     // a render element, as far as verify searches it
constexpr int kKinds = 16;                  // distinct (source, offset) pairs a search keeps for the report
constexpr int kLoggedChanged = 10;          // originals that changed while the flush ran, logged with what they are

enum Src { kSrcObject, kSrcElement, kSrcRenderer, kSrcLight, kSrcFrustum, kSrcArena, kSrcs };
const char* const kSrcNames[kSrcs] = {"CRenderObject", "element", "renderer", "CDLight", "frustum", "arena"};
// what a found pointer points into
enum Into { kNone = -1, kIntoLight, kIntoFrustum, kIntoLightCopy, kIntoFrustumCopy, kIntoArena, kIntos };
const char* const kIntoNames[kIntos] = {"a light", "a frustum", "a light's copy", "a frustum's copy", "the arena"};
constexpr int kExamples = 8;  // found pointers logged one by one

struct Found {  // pointers a search found, by where they were and what they point into
    uint64_t total = 0;
    int kinds = 0, logged = 0;
    struct {
        int src;
        uint32_t offset;
        int into;
        uint64_t count;
    } at[kKinds] = {};
    void Add(int src, uint32_t offset, int into) {
        ++total;
        for (int i = 0; i < kinds; ++i)
            if (at[i].src == src && at[i].offset == offset && at[i].into == into) {
                ++at[i].count;
                return;
            }
        if (kinds < kKinds) at[kinds++] = {src, offset, into, 1};
    }
};

// lights or frustums: their copies, and what the flushes did with them
struct Kind {
    const char* name;
    lt::Copies copies;
    uint64_t recorded = 0, maxRecorded = 0, copied = 0, notCopied = 0, writtenBytes = 0;
    uint64_t keptWords = 0;  // words the flush wrote that the original changed meanwhile: the original's kept
    uint64_t vChanged = 0, vChangedWords = 0, vWritten = 0, vWrittenWords = 0;
    uint32_t written[lt::kMaxWords] = {}, changed[lt::kMaxWords] = {};
    int logged = 0;
};

struct Stats {
    uint64_t flushes = 0, unlisted = 0;
    uint64_t inLists = 0, inCaptures = 0, objects = 0, fields = 0;  // pointed at the copies
    uint64_t fromLights = 0, fromObjects = 0, objectLists = 0, stashes = 0, casterLists = 0, lightArrays = 0;
    uint64_t listsNotCopied = 0, objectListsChanged = 0, unreadable = 0;
    uint64_t addedInFlush = 0, otherThread = 0, onStack = 0, fromCaptures = 0;
    uint64_t recordedInFlush = 0;  // the main thread's while the flush ran (the next frame's, on a render thread)
    uint64_t listsAfter = 0, fieldsAfter = 0;  // put back after the flush, beside what was redirected
    uint64_t translated = 0;                   // pointers into the copies the flush wrote, written back as the originals'
    uint64_t arenaMax = 0;
    uint64_t copyCycles = 0, afterCycles = 0;
    // verify
    uint64_t vNotRecorded = 0, vUnreadable = 0, vListsDiffer = 0;
    Found missed, kept;
};

struct ObjectList {  // a drawn object's frustum list, pointed at a copy
    char* object;
    char* list;
    lt::PtrList* copy;
};

struct State {
    LMode mode = LMode::Off;
    bool installed = false, ready = false, active = false, inFlush = false;
    LightTarget t;
    const ShadowPrepassTarget* sp = nullptr;
    AddLightFn addLight = nullptr;
    DWORD mainThread = 0;
    char* renderer = nullptr;
    Kind lights{"light"}, frustums{"frustum"};
    // the lights the main thread gave the renderer since the last hand-off: two records in turn, the hand-off takes rec
    struct Recorded {
        char* light;
        uint32_t levels;
    };
    std::vector<Recorded> record[2];
    int rec = 0;
    lt::Arena arena;
    KeyIndex listIndex;  // an original list (a caster or object frustum list) -> its copy, in listCopies
    std::vector<std::pair<const lt::PtrList*, lt::PtrList*>> listCopies;
    std::vector<std::pair<const char*, char**>> arrayCopies;  // a light's frustum array -> its copy
    std::vector<ObjectList> objectLists;
    struct Range {
        const char* start;
        size_t size;
        int into;
        bool operator<(const Range& o) const { return start < o.start; }
    };
    std::vector<Range> sorted;  // verify: the copied originals, by address
    std::vector<char> buffer;                            // verify: what a search reads
    char probe[lt::kFrustumBytes];                       // a frustum read to check it can be
    Stats st;
};
State g;

void AddLightHook(char* r, char* light) {
    g.addLight(r, light);
    if (!light || !g.ready) return;
    const DWORD t = GetCurrentThreadId();
    if (g.inFlush && t == FlushThread()) {  // by the flush itself: a scene traversal left in it
        ++g.st.addedInFlush;
        return;
    }
    if (t != g.mainThread) {
        ++g.st.otherThread;
        return;
    }
    ULONG_PTR low = 0, high = 0;  // a temporary on the stack (a reflection's sun): the reflection part copies it
    GetCurrentThreadStackLimits(&low, &high);
    if (uintptr_t(light) >= low && uintptr_t(light) < high) {
        ++g.st.onStack;
        return;
    }
    // the main thread's: for the next hand-off (while a flush runs on another thread, the next frame's)
    const int level = *g.sp->level;
    g.record[g.rec].push_back({light, level >= 0 && level < 32 ? 1u << level : 0});
    if (g.inFlush) ++g.st.recordedInFlush;
}

// --- the items the flush will draw ------------------------------------------------------------------------------------
using ItemFn = void (*)(char* obj, char* elem);

void StashItems(const sp::Stash& s, void* ctx) {
    const ItemFn fn = reinterpret_cast<ItemFn>(ctx);
    const sp::RendItem* it = s.items.data();
    for (int k = 0; k < sp::kLists; ++k) {
        for (uint32_t i = 0; i < s.counts[k]; ++i) fn(it[i].obj, it[i].elem);
        it += s.counts[k];
    }
}

// level 1's part of the render lists, the shadow pre-pass's stashes, the reflections' captures (as the census walks
// them; a capture's refraction list too, which a reflection doesn't draw: its objects are put back all the same)
void ForEachItem(ItemFn fn) {
    const auto* lists = reinterpret_cast<const sp::ItemList*>(g.sp->lists);
    const uint32_t* starts = g.sp->starts;
    for (int k = 0; k < sp::kLists; ++k)
        for (uint32_t i = starts[k]; i < lists[k].count; ++i) fn(lists[k].elems[i].obj, lists[k].elems[i].elem);
    ShadowPrepass_ForEachStash(&StashItems, reinterpret_cast<void*>(fn));
    ReflectionPrepass_ForEachCapture(&StashItems, reinterpret_cast<void*>(fn));
}

// The renderer's own fields that keep CDLight pointers between flushes: the sun and the light passes' lights. The
// reflections' dry run fills them with the lights before the hand-off, the flush with the copies.
template <class Fn>
void ForEachLightField(char* r, Fn fn) {
    fn(r + kRSun);
    for (int i = 0; i < kGroups; ++i)
        for (int k = 0; k < kGroupLights; ++k) fn(r + kRGroups + i * kGroupBytes + k * 8);
}

// --- the frustums and the lists between them -------------------------------------------------------------------------
bool SafeRead(const char* p, char* out, size_t n) {
    __try {
        std::memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A light's frustums, as the renderer reads them: only a light that casts shadow maps has them (on another, the field
// can hold anything), up to 8, up to the first null. False if it has none.
bool LightFrustums(const char* light, char* (&out)[lt::kLightFrustums]) {
    uint32_t flags;
    std::memcpy(&flags, light + lt::kLFlags, sizeof flags);
    const char* const array = lt::Get(light + lt::kLFrustums);
    if (!(flags & lt::kCastShadowMaps) || !array || !SafeRead(array, reinterpret_cast<char*>(out), sizeof out))
        return false;
    bool ended = false;
    for (char*& f : out) {
        ended = ended || !f;
        if (ended) f = nullptr;
    }
    return out[0] != nullptr;
}

lt::PtrList* CopiedList(const void* list) {
    const uint32_t* i = g.listIndex.Find(uint64_t(reinterpret_cast<uintptr_t>(list)));
    return i ? g.listCopies[*i].second : nullptr;
}

// a list copied into the arena (once: read safely, its elements straight there), its pointers mapped through `map`
// (null: kept as they are)
lt::PtrList* CopyListOnce(const lt::PtrList* list, const lt::Copies* map, uint32_t maxCount) {
    if (lt::PtrList* c = CopiedList(list)) return c;
    lt::PtrList head;
    char* at = nullptr;
    if (SafeRead(reinterpret_cast<const char*>(list), reinterpret_cast<char*>(&head), sizeof head) &&
        head.count <= maxCount && (head.elems || !head.count))
        at = g.arena.Alloc(sizeof(lt::PtrList) + size_t(head.count) * sizeof(char*));
    auto* c = reinterpret_cast<lt::PtrList*>(at);
    if (c) {
        c->elems = reinterpret_cast<char**>(at + sizeof(lt::PtrList));
        c->count = c->capacity = head.count;
        if (head.count && !SafeRead(reinterpret_cast<const char*>(head.elems), reinterpret_cast<char*>(c->elems),
                                    size_t(head.count) * sizeof(char*)))
            c = nullptr;  // (its room in the arena stays used until the arena is emptied)
    }
    if (!c) {
        ++g.st.listsNotCopied;
        return nullptr;
    }
    if (map)
        for (uint32_t i = 0; i < c->count; ++i) c->elems[i] = map->Map(c->elems[i]);
    g.listIndex.Insert(uint64_t(reinterpret_cast<uintptr_t>(list)), uint32_t(g.listCopies.size()));
    g.listCopies.push_back({list, c});
    return c;
}

// a frustum to copy, if it is readable
void AddFrustum(char* f, uint64_t& from) {
    if (!f || g.frustums.copies.IsRecorded(f)) return;
    if (!SafeRead(f, g.probe, lt::kFrustumBytes)) {
        ++g.st.unreadable;
        return;
    }
    g.frustums.copies.Add(f);
    ++from;
}

// a copied light's frustums, and a drawn object's (from its list, before it is pointed at a copy)
void AddLightFrustums(const char* lightCopy) {
    char* array[lt::kLightFrustums];
    if (!LightFrustums(lightCopy, array)) return;
    for (int j = 0; j < lt::kLightFrustums && array[j]; ++j) AddFrustum(array[j], g.st.fromLights);
}

void AddObjectFrustums(char* obj, char*) {
    if (!obj) return;
    const char* const list = lt::Get(obj + lt::kOFrustums);
    lt::PtrList head;
    char* elems[kMaxObjectFrustums];
    if (!list || !SafeRead(list, reinterpret_cast<char*>(&head), sizeof head) || !head.elems ||
        head.count > kMaxObjectFrustums ||
        !SafeRead(reinterpret_cast<const char*>(head.elems), reinterpret_cast<char*>(elems),
                  size_t(head.count) * sizeof(char*)))
        return;
    for (uint32_t k = 0; k < head.count; ++k) AddFrustum(elems[k], g.st.fromObjects);
}

void RedirectObject(char* obj, char*) {
    if (!obj) return;
    if (g.lights.copies.RedirectField(obj + lt::kOLight)) ++g.st.objects;
    char* const list = lt::Get(obj + lt::kOFrustums);
    if (!list || g.arena.Contains(list)) return;  // (none, or the object was met before)
    lt::PtrList* copy =
        CopyListOnce(reinterpret_cast<const lt::PtrList*>(list), &g.frustums.copies, kMaxObjectFrustums);
    if (!copy) return;
    lt::Set(obj + lt::kOFrustums, reinterpret_cast<char*>(copy));
    g.objectLists.push_back({obj, list, copy});
    Allocator_Pin(obj);  // (FlushThread=overlap: kept until the join, which puts the list back)
    ++g.st.objectLists;
}

// --- verify's searches ------------------------------------------------------------------------------------------------
// what v points into among the copied originals (a light or a frustum), else kNone
int IntoOriginal(const char* v) {
    auto it = std::upper_bound(g.sorted.begin(), g.sorted.end(), State::Range{v, 0, kNone});
    if (it == g.sorted.begin()) return kNone;
    --it;
    return v >= it->start && v < it->start + it->size ? it->into : kNone;
}

int IntoCopies(const char* v) {
    if (g.lights.copies.InCopies(v)) return kIntoLightCopy;
    if (g.frustums.copies.InCopies(v)) return kIntoFrustumCopy;
    return g.arena.Contains(v) ? kIntoArena : kNone;
}

const char* ClassOf(const char* object);

// the 8-byte-aligned pointers in [p, p + n) that `match` places, into `found`; the first ones logged, with the class of
// what holds them (an object's: its render element's)
template <class Match>
void Search(const char* p, size_t n, int src, Match match, Found& found, const char* what) {
    if (!p || n > g.buffer.size()) return;
    if (!SafeRead(p, g.buffer.data(), n)) {
        ++g.st.vUnreadable;
        return;
    }
    for (size_t o = 0; o + 8 <= n; o += 8) {
        const char* v;
        std::memcpy(&v, g.buffer.data() + o, sizeof v);
        if (reinterpret_cast<uintptr_t>(v) & 7) continue;  // (a pointer here is 8-byte aligned: not floats or shorts)
        const int into = match(v);
        if (into == kNone) continue;
        found.Add(src, uint32_t(o), into);
        if (found.logged < kExamples) {
            ++found.logged;
            const char* holder = src == kSrcObject && n >= 0xB0 ? lt::Get(g.buffer.data() + 0xA8)  // (m_pRE)
                                 : src == kSrcElement           ? p
                                                                : nullptr;
            const char* cls = holder ? ClassOf(holder) : nullptr;
            Log("lights: VERIFY flush %llu: %s: %s %p +0x%zX (%s) points into %s: %p", g.st.flushes, what,
                kSrcNames[src], p, o, cls ? cls : "-", kIntoNames[into], v);
        }
    }
}

// at the hand-off, after the redirect: pointers into the originals that are left
void FindMissed(char* r) {
    g.sorted.clear();
    for (const Kind* k : {&g.lights, &g.frustums})
        for (size_t i = 0; i < k->copies.Recorded(); ++i)
            if (k->copies.Copy(i))
                g.sorted.push_back({k->copies.Object(i), k->copies.Bytes(), k == &g.lights ? kIntoLight : kIntoFrustum});
    std::sort(g.sorted.begin(), g.sorted.end());
    if (g.sorted.empty()) return;
    static const char* const kMissed = "missed";
    ForEachItem([](char* obj, char* elem) {
        Search(obj, lt::kObjectBytes, kSrcObject, IntoOriginal, g.st.missed, kMissed);
        Search(elem, kElementBytes, kSrcElement, IntoOriginal, g.st.missed, kMissed);
    });
    Search(r, kRendererBytes, kSrcRenderer, IntoOriginal, g.st.missed, kMissed);
    for (size_t i = 0; i < g.lights.copies.Recorded(); ++i)
        Search(g.lights.copies.Copy(i), lt::kLightBytes, kSrcLight, IntoOriginal, g.st.missed, kMissed);
    for (size_t i = 0; i < g.frustums.copies.Recorded(); ++i)
        Search(g.frustums.copies.Copy(i), lt::kFrustumBytes, kSrcFrustum, IntoOriginal, g.st.missed, kMissed);
    // the copied lists: an original left there is one the copies' capacity couldn't take
    for (const auto& l : g.listCopies)
        Search(reinterpret_cast<const char*>(l.second->elems), size_t(l.second->count) * 8, kSrcArena, IntoOriginal,
               g.st.missed, kMissed);
}

// after the flush, once the pointers are put back: pointers into the copies or the arena that are kept
void FindKept(char* r) {
    Search(r, kRendererBytes, kSrcRenderer, IntoCopies, g.st.kept, "kept");
    ForEachItem([](char* obj, char*) { Search(obj, lt::kObjectBytes, kSrcObject, IntoCopies, g.st.kept, "kept"); });
}

// after the flush: whether each copied list still holds what its original holds
void CompareLists() {
    for (const auto& l : g.listCopies) {
        const lt::PtrList* o = l.first;
        const lt::PtrList* c = l.second;
        bool same = o->count == c->count;
        for (uint32_t i = 0; same && i < c->count; ++i)
            same = g.frustums.copies.Unmap(c->elems[i]) == o->elems[i];
        if (!same) ++g.st.vListsDiffer;
    }
}

// The class MSVC RTTI names for a polymorphic object (x64 locators: image-relative), or null.
const char* ClassOf(const char* object) {
    __try {
        const void* const* vt = *reinterpret_cast<const void* const* const*>(object);
        const auto* col = static_cast<const uint32_t*>(vt[-1]);
        HMODULE m;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(col), &m) ||
            col[0] > 1)
            return nullptr;
        const char* name = reinterpret_cast<const char*>(m) + col[3] + 16;  // the type descriptor: vtable, spare, name
        return name[0] == '.' && name[1] == '?' ? name : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void Append(char* line, size_t size, size_t& n, const char* fmt, ...) {
    if (n >= size) return;
    va_list a;
    va_start(a, fmt);
    const int k = vsnprintf(line + n, size - n, fmt, a);
    va_end(a);
    if (k > 0) n = (std::min)(size - 1, n + size_t(k));
}

// verify, after the flush: the first originals that changed while it ran, with what they are (a light: its id, levels,
// flags, owner and name; a frustum: its light's id and owner)
void LogChanged(Kind& k) {
    const bool light = &k == &g.lights;
    const size_t bytes = k.copies.Bytes();
    for (size_t i = 0; i < k.copies.Recorded() && k.logged < kLoggedChanged; ++i) {
        const char* p = k.copies.Pristine(i);
        char* o = k.copies.Object(i);
        char now[lt::kFrustumBytes];
        if (!p || !SafeRead(o, now, bytes) || std::memcmp(now, p, bytes) == 0) continue;
        ++k.logged;
        char words[400] = "";
        size_t n = 0;
        int shown = 0, all = 0;
        for (size_t w = 0; w < bytes / 4; ++w) {
            uint32_t a, b;
            std::memcpy(&a, p + w * 4, 4);
            std::memcpy(&b, now + w * 4, 4);
            if (a == b) continue;
            ++all;
            if (shown++ < 12) Append(words, sizeof words, n, " +0x%zX:%08X>%08X", w * 4, a, b);
        }
        int32_t id;
        uint32_t flags = 0;
        std::memcpy(&id, p + (light ? 0 : 0x368), 4);
        if (light) std::memcpy(&flags, p + 0x1D4, 4);
        const char* owner = lt::Get(p + (light ? 0x90 : 0x370));
        const char* cls = owner ? ClassOf(owner) : nullptr;
        char name[32] = "-";
        if (light) {
            const char* np = lt::Get(p + 0x208);
            name[0] = '?';
            name[1] = 0;
            if (np && SafeRead(np, name, sizeof name - 1)) name[sizeof name - 1] = 0;
        }
        Log("lights: VERIFY flush %llu: %s %p (id %d, tags 0x%X, flags 0x%08X, owner %p %s, '%s') changed while the "
            "flush ran, %d words:%s",
            g.st.flushes, k.name, o, id, k.copies.Tags(i), flags, owner, cls ? cls : "?", name, all, words);
    }
}

void Where(const Found& f, char* line, size_t size) {
    size_t n = 0;
    line[0] = 0;
    for (int i = 0; i < f.kinds; ++i)
        Append(line, size, n, "%s%s+0x%X into %s:%llu", i ? ", " : "", kSrcNames[f.at[i].src], f.at[i].offset,
               kIntoNames[f.at[i].into], f.at[i].count);
    if (!f.kinds) Append(line, size, n, "none");
}

void Words(const uint32_t* perWord, size_t words, char* line, size_t size) {
    size_t n = 0;
    line[0] = 0;
    for (size_t w = 0; w < words; ++w)
        if (perWord[w]) Append(line, size, n, "%s+0x%zX:%u", n ? " " : "", w * 4, perWord[w]);
    if (!n) Append(line, size, n, "none");
}

// --- the hand-off and after it ---------------------------------------------------------------------------------------
void Copy(Kind& k) {
    const size_t n = k.copies.Recorded();
    k.recorded += n;
    k.maxRecorded = (std::max)(k.maxRecorded, uint64_t(n));
    const size_t c = k.copies.CopyAll();
    k.copied += c;
    k.notCopied += n - c;
}

void HandOff(char* r) {
    Stats& s = g.st;
    lt::Copies& L = g.lights.copies;
    lt::Copies& F = g.frustums.copies;
    // the lights the main thread recorded since the last hand-off; it records into the other record from here on
    for (const State::Recorded& x : g.record[g.rec]) L.Add(x.light, x.levels);
    g.record[g.rec].clear();
    g.rec ^= 1;
    // the lights, with those of the reflections' captures (their copies of lights that were on the stack) (m_Id -1: a
    // fake light, or one past its list's 32)
    ReflectionPrepass_ForEachCapturedLights(
        [](void** p, size_t k, void*) {
            for (size_t i = 0; i < k; ++i)
                if (p[i] && !g.lights.copies.IsRecorded(static_cast<char*>(p[i]))) {
                    g.lights.copies.Add(static_cast<char*>(p[i]), 1u << 2);
                    ++g.st.fromCaptures;
                }
        },
        nullptr);
    Copy(g.lights);
    for (size_t i = 0; i < L.Recorded(); ++i)
        if (const char* c = L.Copy(i)) {
            int32_t id;
            std::memcpy(&id, c, sizeof id);
            if (id == -1) ++s.unlisted;
        }
    // the frustums: the copied lights' and the drawn objects'
    for (size_t i = 0; i < L.Recorded(); ++i)
        if (const char* c = L.Copy(i)) AddLightFrustums(c);
    ForEachItem(&AddObjectFrustums);
    Copy(g.frustums);
    // the frustums' caster lists, as they are
    for (size_t i = 0; i < F.Recorded(); ++i) {
        const char* c = F.Copy(i);
        const auto* list = c ? reinterpret_cast<const lt::PtrList*>(lt::Get(c + lt::kFCasters)) : nullptr;
        if (!list) continue;
        if (lt::PtrList* copy = CopyListOnce(list, nullptr, kMaxCasters)) {
            F.SetField(i, lt::kFCasters, reinterpret_cast<char*>(copy));
            ++s.casterLists;
        }
    }
    // the lights' frustum arrays, at the copies
    for (size_t i = 0; i < L.Recorded(); ++i) {
        const char* c = L.Copy(i);
        char* array[lt::kLightFrustums];
        if (!c || !LightFrustums(c, array)) continue;
        auto* copy = reinterpret_cast<char**>(g.arena.Alloc(lt::kLightFrustums * sizeof(char*)));
        if (!copy) {
            ++s.listsNotCopied;
            continue;
        }
        for (int j = 0; j < lt::kLightFrustums; ++j) copy[j] = array[j] ? F.Map(array[j]) : nullptr;
        g.arrayCopies.push_back({lt::Get(c + lt::kLFrustums), copy});
        L.SetField(i, lt::kLFrustums, reinterpret_cast<char*>(copy));
        ++s.lightArrays;
    }
    // the light lists
    for (int level = 1; level <= 2; ++level) {
        auto* list = reinterpret_cast<lt::PtrList*>(r + lt::kRLights + level * 16);
        if (!list->elems || list->count > kMaxListed) continue;
        if (g.mode == LMode::Verify)
            for (uint32_t i = 0; i < list->count; ++i)
                if (!L.IsRecorded(list->elems[i])) ++s.vNotRecorded;
        s.inLists += uint64_t(L.RedirectList(list));
    }
    ReflectionPrepass_ForEachCapturedLights(
        [](void** p, size_t k, void*) { g.st.inCaptures += uint64_t(g.lights.copies.RedirectPointers(p, k)); },
        nullptr);
    // the drawn objects' m_pLight and frustum lists
    ForEachItem(&RedirectObject);
    // the renderer's sun, light passes and current frustum
    ForEachLightField(r, [](char* field) {
        if (g.lights.copies.RedirectField(field)) ++g.st.fields;
    });
    if (F.RedirectField(r + kRFrustum)) ++s.fields;
    // the shadow pre-pass's stashes
    ShadowPrepass_MapStashes(
        [](char** frustum, void** casters, void*) {
            char* const m = g.frustums.copies.Map(*frustum);
            if (m != *frustum) {
                *frustum = m;
                ++g.st.stashes;
            }
            if (lt::PtrList* c = CopiedList(*casters)) *casters = c;
        },
        nullptr);
    s.arenaMax = (std::max)(s.arenaMax, uint64_t(g.arena.Used()));
}

// A pointer the flush wrote into a copy, written back into its original: one into a copy or the arena is put back to
// the original it stands for.
char* Original(char* v) {
    if (g.lights.copies.InCopies(v)) return g.lights.copies.Unmap(v);
    if (g.frustums.copies.InCopies(v)) return g.frustums.copies.Unmap(v);
    if (!g.arena.Contains(v)) return v;
    for (const auto& l : g.listCopies)
        if (v == reinterpret_cast<char*>(l.second)) return reinterpret_cast<char*>(const_cast<lt::PtrList*>(l.first));
    for (const auto& a : g.arrayCopies)
        if (v == reinterpret_cast<char*>(a.second)) return const_cast<char*>(a.first);
    return v;  // (elsewhere in the arena: left, and found by verify's search)
}

void TranslateWritten(Kind& k) {
    for (size_t i = 0; i < k.copies.Recorded(); ++i) {
        const char* c = k.copies.Copy(i);
        const char* p = k.copies.Pristine(i);
        if (!c) continue;
        char* o = k.copies.Object(i);
        for (size_t off = 0; off + 8 <= k.copies.Bytes(); off += 8) {
            if (std::memcmp(c + off, p + off, 8) == 0) continue;  // (only what the flush wrote)
            char* const v = lt::Get(o + off);
            char* const m = Original(v);
            if (m != v) {
                lt::Set(o + off, m);
                ++g.st.translated;
            }
        }
    }
}

void Compare(Kind& k) {
    lt::Diff written, changed;
    k.copies.Compare(written, changed);
    k.vWritten += written.objects;
    k.vWrittenWords += written.words;
    k.vChanged += changed.objects;
    k.vChangedWords += changed.words;
    for (size_t w = 0; w < lt::kMaxWords; ++w) {
        k.written[w] += written.perWord[w];
        k.changed[w] += changed.perWord[w];
    }
    if (changed.objects) LogChanged(k);
}

void AfterFlush() {
    Stats& s = g.st;
    if (g.mode == LMode::Verify) {
        Compare(g.lights);
        Compare(g.frustums);
        CompareLists();
    }
    size_t kept = 0;
    g.lights.writtenBytes += g.lights.copies.WriteBack(&kept);
    g.lights.keptWords += kept;
    g.frustums.writtenBytes += g.frustums.copies.WriteBack(&kept);
    g.frustums.keptWords += kept;
    TranslateWritten(g.lights);
    TranslateWritten(g.frustums);
    g.lights.copies.Restore();
    g.frustums.copies.Restore();
    // the objects' frustum lists; the light lists and the renderer's fields again (the flush itself fills them with the
    // copies: a replayed reflection puts level 2's list back, the per-list setup and the light passes take theirs from
    // the lists, PrepareDepthMap sets the current frustum)
    for (const ObjectList& o : g.objectLists) {
        if (Allocator_Dying(o.object)) continue;  // (freed beside the flush)
        if (lt::Get(o.object + lt::kOFrustums) == reinterpret_cast<char*>(o.copy))
            lt::Set(o.object + lt::kOFrustums, o.list);
        else
            ++s.objectListsChanged;
    }
    for (int level = 1; level <= 2; ++level) {
        auto* list = reinterpret_cast<lt::PtrList*>(g.renderer + lt::kRLights + level * 16);
        if (list->elems && list->count <= kMaxListed)
            s.listsAfter += g.lights.copies.UnmapRange(reinterpret_cast<char*>(list->elems), size_t(list->count) * 8);
    }
    ForEachLightField(g.renderer, [](char* field) { g.st.fieldsAfter += g.lights.copies.UnmapRange(field, 8); });
    s.fieldsAfter += g.frustums.copies.UnmapRange(g.renderer + kRFrustum, 8);
    if (g.mode == LMode::Verify) FindKept(g.renderer);
    g.lights.copies.Clear();
    g.frustums.copies.Clear();
    g.arena.Reset();
    g.listIndex.Clear();
    g.listCopies.clear();
    g.arrayCopies.clear();
    g.objectLists.clear();
}

void ReportKind(const char* when, const Kind& k, double f) {
    const size_t words = k.copies.Bytes() / 4;
    char written[1500], changed[3000];
    Words(k.written, words, written, sizeof written);
    Words(k.changed, words, changed, sizeof changed);
    Log("lights [%s]: VERIFY %ss changed while the flush ran: %llu, %llu words (%s) | %ss the flush wrote: %llu, "
        "%llu words (offset:objects %s) | written back %.1f bytes per flush",
        when, k.name, k.vChanged, k.vChangedWords, changed, k.name, k.vWritten, k.vWrittenWords, written,
        double(k.writtenBytes) / f);
}

void Report(const char* when) {
    const Stats& s = g.st;
    const double f = double(s.flushes ? s.flushes : 1);
    const Kind& L = g.lights;
    const Kind& F = g.frustums;
    Log("lights [%s]: mode=%s, flushes %llu | lights per flush: %.1f given to the renderer (most %llu), %.1f in no list "
        "(fake, or past a list's 32), copied %.1f (%llu not: the arena holds %zu); pointed at the copies per flush: %.1f "
        "list entries, %.1f of the reflections' captured lights, %.1f render objects' m_pLight | frustums per flush: "
        "%.1f (most %llu; %.1f from the lights, %.1f more from the objects' lists), copied %.1f (%llu not); per flush "
        "pointed at copies: %.1f caster lists, %.1f lights' frustum arrays, %.1f objects' frustum lists, %.1f stashes; "
        "lists not copied (unreadable, too long or the arena full) %llu (most used %.2f MB); frustums unreadable %llu",
        when, ModeName(g.mode), s.flushes, double(L.recorded) / f, L.maxRecorded, double(s.unlisted) / f,
        double(L.copied) / f, L.notCopied, kMaxLights, double(s.inLists) / f, double(s.inCaptures) / f,
        double(s.objects) / f, double(F.recorded) / f, F.maxRecorded, double(s.fromLights) / f,
        double(s.fromObjects) / f, double(F.copied) / f, F.notCopied, double(s.casterLists) / f,
        double(s.lightArrays) / f, double(s.objectLists) / f, double(s.stashes) / f, s.listsNotCopied,
        double(s.arenaMax) / 1048576.0, s.unreadable);
    Log("lights [%s]: lights added by the flush (vanilla's pointers kept): %llu, on other threads: %llu, on the main "
        "thread while the flush ran (for the next hand-off) %llu, on the stack (left to the reflections' captures) "
        "%llu; added from the captures %llu | the renderer's sun, light passes and current frustum: %.1f pointed at the "
        "copies per flush, %.1f put back after it; light list entries put back after it %.1f; objects' frustum lists the "
        "flush changed %llu | bytes the flush wrote, written back per flush: lights %.1f, frustums %.1f (pointers into "
        "the copies among them, put back to the originals: %llu; words the originals changed meanwhile, theirs kept: "
        "lights %llu, frustums %llu) | %.3f ms per flush before it, %.3f ms after",
        when, s.addedInFlush, s.otherThread, s.recordedInFlush, s.onStack, s.fromCaptures, double(s.fields) / f,
        double(s.fieldsAfter) / f, double(s.listsAfter) / f, s.objectListsChanged, double(L.writtenBytes) / f,
        double(F.writtenBytes) / f, s.translated, L.keptWords, F.keptWords, double(s.copyCycles) / 3.0e6 / f,
        double(s.afterCycles) / 3.0e6 / f);
    if (g.mode != LMode::Verify) return;
    char missed[700], kept[700];
    Where(s.missed, missed, sizeof missed);
    Where(s.kept, kept, sizeof kept);
    Log("lights [%s]: VERIFY light list entries not given to the renderer this frame %llu | pointers into the "
        "originals the redirect missed: %llu (%s) | pointers into the copies or the arena kept after the flush: %llu "
        "(%s) | copied lists no longer matching their originals after the flush: %llu | unreadable memory skipped: %llu",
        when, s.vNotRecorded, s.missed.total, missed, s.kept.total, kept, s.vListsDiffer, s.vUnreadable);
    ReportKind(when, L, f);
    ReportKind(when, F, f);
}
#endif  // _WIN64

}  // namespace

bool LightSnapshot_Locate(char* base, LightTarget* out, const char** why) {
#ifndef _WIN64
    (void)base;
    *out = LightTarget{};
    *why = "64-bit game only";
    return false;
#else
    *out = LightTarget{};
    const std::vector<char*> adds = FindInCode(base, ParsePattern(kAddLightCode), 2);
    if (adds.size() != 1) {
        *why = adds.empty() ? "EF_ADDDlight not found" : "EF_ADDDlight's code found more than once";
        return false;
    }
    char* const fn = adds[0];
    if (!IsInCode(base, fn, kAddLightSearch) ||
        FindInRange(fn, kAddLightSearch, ParsePattern(kObjectsCode), 2).size() != 1 ||
        FindInRange(fn, kAddLightSearch, ParsePattern(kCoronaCode), 2).size() != 1) {
        *why = "EF_ADDDlight: its coronas' objects are not as expected";
        return false;
    }
    const std::vector<char*> ctors = FindInCode(base, ParsePattern(kLightCtorCode), 4);
    if (ctors.empty()) {
        *why = "the CDLight constructor not found: the light's layout is not the expected one";
        return false;
    }
    if (FindInCode(base, ParsePattern(kSunCode), 2).size() != 1) {
        *why = "the per-list setup's sun is not as expected";
        return false;
    }
    // (the function fills the passes in two loops, one per kind of light: either will do)
    const std::vector<char*> groups = FindInCode(base, ParsePattern(kGroupsBase), 2);
    if (groups.size() != 1 || !IsInCode(base, groups[0], kGroupsSearch) ||
        FindInRange(groups[0], kGroupsSearch, ParsePattern(kGroupsLoop), 4).empty()) {
        *why = "the light passes are not as expected";
        return false;
    }
    out->addLight = fn;
    out->lightCtor = ctors[0];
    return true;
#endif
}

void LightSnapshot_Install(const wchar_t* moduleName, char* base, size_t size, const ShadowPrepassTarget& sp) {
    (void)size;
#ifndef _WIN64
    (void)base;
    (void)sp;
    if (ReadMode() != LMode::Off) Log("%ls: light snapshot not applied: 64-bit game only", moduleName);
#else
    if (g.installed) return;
    g.mode = ReadMode();
    if (g.mode == LMode::Off) return;
    const char* why = nullptr;
    if (!LightSnapshot_Locate(base, &g.t, &why)) {
        Log("%ls: lights: off: %s", moduleName, why);
        return;
    }
    if (!InstallHook(g.t.addLight, reinterpret_cast<void*>(&AddLightHook), reinterpret_cast<void**>(&g.addLight),
                     "EF_ADDDlight")) {
        Log("%ls: lights: off: could not hook EF_ADDDlight", moduleName);
        return;
    }
    g.sp = &sp;
    // the copies and the arena near the top of the address space: no small integer or float is a pointer into them
    auto top = [](size_t bytes) {
        return static_cast<char*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
    };
    char* const lightMemory = top(lt::Copies::ArenaBytes(kMaxLights, lt::kLightBytes));
    char* const frustumMemory = top(lt::Copies::ArenaBytes(kMaxFrustums, lt::kFrustumBytes));
    char* const arenaMemory = top(kArenaBytes + 16);
    if (!lightMemory || !frustumMemory || !arenaMemory) {
        Log("%ls: lights: off: no memory for the copies", moduleName);
        return;
    }
    g.lights.copies.Init(kMaxLights, lt::kLightBytes, lightMemory);
    g.frustums.copies.Init(kMaxFrustums, lt::kFrustumBytes, frustumMemory);
    // (FlushThread=overlap: the write-back runs at the join, after the main thread may have freed a light's owner)
    g.lights.copies.SetPins(&Allocator_Pin, &Allocator_Dying);
    g.frustums.copies.SetPins(&Allocator_Pin, &Allocator_Dying);
    g.arena.Init(kArenaBytes, arenaMemory);
    if (g.mode == LMode::Verify) g.buffer.resize(kRendererBytes);
    g.installed = true;
    Log("%ls: lights: mode=%s, EF_ADDDlight +0x%X, CDLight constructor +0x%X (600 bytes; shadow frustums 0x388); "
        "recording starts after the first flush",
        moduleName, ModeName(g.mode), unsigned(g.t.addLight - base), unsigned(g.t.lightCtor - base));
#endif
}

void LightSnapshot_BeforeFlush(char* r) {
#ifdef _WIN64
    if (!g.installed) return;
    if (!g.ready) {  // the first flush: from now on the main thread's lights are recorded
        g.mainThread = GetCurrentThreadId();
        g.lights.copies.Clear();
        g.record[0].clear();
        g.record[1].clear();
        g.ready = true;
        return;
    }
    if (g.active || GetCurrentThreadId() != g.mainThread) return;
    const uint64_t t0 = __rdtsc();
    ++g.st.flushes;
    g.renderer = r;
    HandOff(r);
    if (g.mode == LMode::Verify) FindMissed(r);
    g.active = g.inFlush = true;
    g.st.copyCycles += __rdtsc() - t0;
#else
    (void)r;
#endif
}

void LightSnapshot_AfterFlush() {
#ifdef _WIN64
    if (!g.active) return;
    g.active = g.inFlush = false;
    const uint64_t t0 = __rdtsc();
    AfterFlush();
    g.st.afterCycles += __rdtsc() - t0;
#endif
}

void LightSnapshot_Report(const char* when) {
#ifdef _WIN64
    if (g.installed && g.ready) Report(when);
#else
    (void)when;
#endif
}

}  // namespace optiperf
