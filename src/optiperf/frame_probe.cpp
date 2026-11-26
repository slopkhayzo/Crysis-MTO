// A brightness probe for strobe hunts: see frame_probe.h.

#include "frame_probe.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#ifdef _WIN64
#include <d3d10.h>
#include <dxgi.h>
#endif

#include "config.h"
#include "log.h"

namespace optiperf {
namespace {

#ifdef _WIN64
constexpr int kCols = 16, kRows = 9, kPatch = 16, kPatches = kCols * kRows;
constexpr int kRing = 4;           // a frame's copy is read back kRing presents later
constexpr size_t kScan = 0x40000;  // the renderer object's bytes searched for the swap chain
constexpr unsigned kPresentSlot = 8;
constexpr size_t kFlushEvery = 256;  // records

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

#pragma pack(push, 1)
struct Header {
    char magic[4];
    uint32_t version, cols, rows, patch, width, height, format, ring;
};
constexpr int kExtras = 11, kFirstNext = 5;  // extra[5..10] set for the next present (FrameProbe_SetNext)
struct Record {
    uint32_t seq, tags;
    uint64_t tsc;
    uint32_t extra[kExtras];
    uint16_t luma[kPatches];
};
#pragma pack(pop)

struct State {
    bool wanted = false, tried = false, hooked = false, broken = false;
    PresentFn present = nullptr;
    IDXGISwapChain* chain = nullptr;
    size_t chainAt = 0;
    ID3D10Device* dev = nullptr;
    ID3D10Texture2D* staging[kRing] = {};
    ID3D10Texture2D* resolved = nullptr;  // (a multisampled back buffer, resolved first)
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    int order[3] = {0, 1, 2};  // the bytes of R, G and B in a texel
    bool tenBits = false;      // R10G10B10A2
    bool used[kRing] = {};
    uint32_t slotSeq[kRing] = {}, slotTags[kRing] = {};
    volatile uint32_t slotExtra[kRing][kExtras] = {};
    volatile uint32_t nextExtra[kExtras] = {};  // (kFirstNext and up: for the next present)
    uint64_t slotTsc[kRing] = {};
    uint32_t seq = 0;
    volatile int lastSlot = -1;  // the slot the latest present filled
    volatile LONG tags = 0;
    FILE* out = nullptr;
    size_t unflushed = 0;
    std::wstring path;
    uint64_t presents = 0, records = 0, mapWaits = 0, failures = 0, faults = 0, setups = 0, unknownFormat = 0;
    DWORD presentThread = 0;
};
State g;

bool InModule(const void* p, const char* base, size_t size) {
    return base && p >= base && static_cast<const char*>(p) < base + size;
}

size_t ImageSize(const char* base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

// the swap chain among the renderer object's pointers: an object whose vtable is dxgi.dll's and that answers
// QueryInterface for IDXGISwapChain (read safely; the object's own reference released)
IDXGISwapChain* FindChain(char* r, size_t* at) {
    const char* const dxgi = reinterpret_cast<const char*>(GetModuleHandleW(L"dxgi.dll"));
    if (!dxgi) return nullptr;
    const size_t dxgiSize = ImageSize(dxgi);
    for (size_t off = 0; off + 8 <= kScan; off += 8) {
        __try {
            char* const p = *reinterpret_cast<char**>(r + off);
            if (!p || uintptr_t(p) < 0x10000 || uintptr_t(p) >= 0x7FFFFFFF0000ull || uintptr_t(p) % 8) continue;
            void* const vt = *reinterpret_cast<void**>(p);
            if (!InModule(vt, dxgi, dxgiSize)) continue;
            IDXGISwapChain* chain = nullptr;
            if (SUCCEEDED(reinterpret_cast<IUnknown*>(p)->QueryInterface(__uuidof(IDXGISwapChain),
                                                                          reinterpret_cast<void**>(&chain))) &&
                chain) {
                chain->Release();
                *at = off;
                return chain;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return nullptr;
}

void ReleaseTextures() {
    for (auto*& t : g.staging)
        if (t) {
            t->Release();
            t = nullptr;
        }
    if (g.resolved) {
        g.resolved->Release();
        g.resolved = nullptr;
    }
    for (bool& u : g.used) u = false;
}

void WriteHeader() {
    if (!g.out) return;
    Header h{{'O', 'P', 'R', 'B'}, 4, kCols, kRows, kPatch, g.width, g.height, uint32_t(g.format), kRing};
    std::fwrite(&h, sizeof h, 1, g.out);
}

// the staging textures (and a resolve target for a multisampled back buffer), for this back buffer's size and format
bool Setup(ID3D10Texture2D* bb, const D3D10_TEXTURE2D_DESC& d) {
    ReleaseTextures();
    if (!g.dev) bb->GetDevice(&g.dev);
    if (!g.dev) return false;
    switch (d.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            g.order[0] = 0, g.order[1] = 1, g.order[2] = 2, g.tenBits = false;
            break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            g.order[0] = 2, g.order[1] = 1, g.order[2] = 0, g.tenBits = false;
            break;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            g.tenBits = true;
            break;
        default:
            ++g.unknownFormat;
            return false;
    }
    D3D10_TEXTURE2D_DESC s = {};
    s.Width = kCols * kPatch;
    s.Height = kRows * kPatch;
    s.MipLevels = 1;
    s.ArraySize = 1;
    s.Format = d.Format;
    s.SampleDesc.Count = 1;
    s.Usage = D3D10_USAGE_STAGING;
    s.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    for (auto*& t : g.staging)
        if (FAILED(g.dev->CreateTexture2D(&s, nullptr, &t))) return false;
    if (d.SampleDesc.Count > 1) {
        D3D10_TEXTURE2D_DESC rd = d;
        rd.SampleDesc.Count = 1;
        rd.SampleDesc.Quality = 0;
        rd.Usage = D3D10_USAGE_DEFAULT;
        rd.BindFlags = 0;
        rd.CPUAccessFlags = 0;
        rd.MiscFlags = 0;
        if (FAILED(g.dev->CreateTexture2D(&rd, nullptr, &g.resolved))) return false;
    }
    g.width = d.Width;
    g.height = d.Height;
    g.format = d.Format;
    ++g.setups;
    WriteHeader();
    return true;
}

// a slot's copy read back: each patch's mean luma, x64
void ReadSlot(int slot) {
    D3D10_MAPPED_TEXTURE2D m = {};
    HRESULT hr = g.staging[slot]->Map(0, D3D10_MAP_READ, D3D10_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        ++g.mapWaits;
        hr = g.staging[slot]->Map(0, D3D10_MAP_READ, 0, &m);
    }
    if (FAILED(hr)) {
        ++g.failures;
        return;
    }
    Record rec;
    rec.seq = g.slotSeq[slot];
    rec.tags = g.slotTags[slot];
    rec.tsc = g.slotTsc[slot];
    for (int k = 0; k < kExtras; ++k) rec.extra[k] = g.slotExtra[slot][k];
    for (int i = 0; i < kPatches; ++i) {
        const int cx = (i % kCols) * kPatch, cy = (i / kCols) * kPatch;
        double sum = 0;
        for (int y = 0; y < kPatch; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(cy + y) * m.RowPitch + size_t(cx) * 4;
            for (int x = 0; x < kPatch; ++x, row += 4) {
                double rr, gg, bb;
                if (g.tenBits) {
                    uint32_t v;
                    std::memcpy(&v, row, 4);
                    rr = double(v & 1023) / 4.0, gg = double((v >> 10) & 1023) / 4.0, bb = double((v >> 20) & 1023) / 4.0;
                } else {
                    rr = row[g.order[0]], gg = row[g.order[1]], bb = row[g.order[2]];
                }
                sum += 0.2126 * rr + 0.7152 * gg + 0.0722 * bb;
            }
        }
        const double mean = sum / double(kPatch * kPatch);
        rec.luma[i] = uint16_t(mean * 64.0 + 0.5);
    }
    g.staging[slot]->Unmap(0);
    if (g.out) {
        std::fwrite(&rec, sizeof rec, 1, g.out);
        if (++g.unflushed >= kFlushEvery) {
            std::fflush(g.out);
            g.unflushed = 0;
        }
    }
    ++g.records;
}

void Sample(IDXGISwapChain* chain) {
    ID3D10Texture2D* bb = nullptr;
    if (FAILED(chain->GetBuffer(0, __uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&bb))) || !bb) {
        ++g.failures;
        return;
    }
    D3D10_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    if ((!g.staging[0] || d.Width != g.width || d.Height != g.height || d.Format != g.format) && !Setup(bb, d)) {
        bb->Release();
        g.broken = true;
        Log("frame probe: off: no staging textures for the back buffer (%ux%u, format %u, %u samples)", d.Width,
            d.Height, unsigned(d.Format), d.SampleDesc.Count);
        return;
    }
    ID3D10Resource* src = bb;
    if (g.resolved) {
        g.dev->ResolveSubresource(g.resolved, 0, bb, 0, d.Format);
        src = g.resolved;
    }
    const int slot = int(g.seq % kRing);
    if (g.used[slot]) ReadSlot(slot);
    for (int i = 0; i < kPatches; ++i) {
        const int c = i % kCols, r = i / kCols;
        const UINT x = UINT((2 * c + 1) * int(g.width) / (2 * kCols) - kPatch / 2);
        const UINT y = UINT((2 * r + 1) * int(g.height) / (2 * kRows) - kPatch / 2);
        D3D10_BOX box{x, y, 0, x + kPatch, y + kPatch, 1};
        g.dev->CopySubresourceRegion(g.staging[slot], 0, UINT(c * kPatch), UINT(r * kPatch), 0, src, 0, &box);
    }
    g.used[slot] = true;
    g.slotSeq[slot] = g.seq++;
    g.slotTags[slot] = uint32_t(g.tags);
    g.slotTsc[slot] = __rdtsc();
    for (int k = 0; k < kExtras; ++k) g.slotExtra[slot][k] = k >= kFirstNext ? g.nextExtra[k] : 0xFFFFFFFFu;
    g.lastSlot = slot;
    bb->Release();
}

HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* chain, UINT sync, UINT flags) {
    if (!g.broken && chain == g.chain && !(flags & DXGI_PRESENT_TEST)) {
        ++g.presents;
        g.presentThread = GetCurrentThreadId();
        __try {
            Sample(chain);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ++g.faults;
            g.broken = true;
        }
    }
    return g.present(chain, sync, flags);
}

void Install(char* r) {
    g.chain = FindChain(r, &g.chainAt);
    if (!g.chain) {
        Log("frame probe: off: no swap chain among the renderer object's first 0x%zX bytes", kScan);
        return;
    }
    void** const vt = *reinterpret_cast<void***>(g.chain);
    DWORD old = 0;
    if (!VirtualProtect(&vt[kPresentSlot], sizeof(void*), PAGE_READWRITE, &old)) {
        Log("frame probe: off: the swap chain's vtable can't be written");
        return;
    }
    g.present = reinterpret_cast<PresentFn>(vt[kPresentSlot]);
    vt[kPresentSlot] = reinterpret_cast<void*>(&PresentHook);
    VirtualProtect(&vt[kPresentSlot], sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vt[kPresentSlot], sizeof(void*));
    std::wstring path = LogPath() ? LogPath() : L"";
    const size_t slash = path.find_last_of(L"\\/");
    g.path = slash == std::wstring::npos ? L"" : path.substr(0, slash + 1) + L"optiperf_probe.bin";
    if (!g.path.empty()) _wfopen_s(&g.out, g.path.c_str(), L"wb");
    g.hooked = true;
    Log("frame probe on: the swap chain at renderer +0x%zX, Present hooked in its vtable; %dx%d patches of %d pixels a "
        "frame, read back %d presents later, in %ls",
        g.chainAt, kCols, kRows, kPatch, kRing, g.out ? g.path.c_str() : L"(no file: the log is off)");
}
#endif

}  // namespace

void FrameProbe_OnFlush(char* renderer) {
#ifdef _WIN64
    if (g.tried) return;
    g.tried = true;
    const std::wstring v = ConfigString(L"Debug", L"FrameProbe", L"OPTIPERF_FRAME_PROBE", L"off");
    g.wanted = _wcsicmp(v.c_str(), L"on") == 0 || v == L"1";
    if (g.wanted && renderer) Install(renderer);
#else
    (void)renderer;
#endif
}

void FrameProbe_Tag(uint32_t mask, uint32_t value) {
#ifdef _WIN64
    if (!g.hooked) return;
    LONG was, now;
    do {
        was = g.tags;
        now = LONG((uint32_t(was) & ~mask) | (value & mask));
    } while (InterlockedCompareExchange(&g.tags, now, was) != was);
#else
    (void)mask;
    (void)value;
#endif
}

void FrameProbe_SetNext(int k, uint32_t value) {
#ifdef _WIN64
    if (!g.hooked || k < kFirstNext || k >= kExtras) return;
    g.nextExtra[k] = value;
#else
    (void)k;
    (void)value;
#endif
}

void FrameProbe_SetLatest(int k, uint32_t value) {
#ifdef _WIN64
    const int slot = g.lastSlot;
    if (!g.hooked || slot < 0 || k < 0 || k >= kFirstNext) return;
    g.slotExtra[slot][k] = value;
#else
    (void)k;
    (void)value;
#endif
}

bool FrameProbe_On() {
#ifdef _WIN64
    return g.hooked && !g.broken;
#else
    return false;
#endif
}

void FrameProbe_Report(const char* when) {
#ifdef _WIN64
    if (!g.hooked) return;
    if (g.out) std::fflush(g.out);
    Log("frame probe [%s]: presents %llu, records %llu (back buffer %ux%u, format %u, set up %llu times), waits for a "
        "copy %llu, failures %llu, faults %llu, unknown formats %llu%s",
        when, g.presents, g.records, g.width, g.height, unsigned(g.format), g.setups, g.mapWaits, g.failures, g.faults,
        g.unknownFormat, g.broken ? " (off since a failure)" : "");
#else
    (void)when;
#endif
}

}  // namespace optiperf
