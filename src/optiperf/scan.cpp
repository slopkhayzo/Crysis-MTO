#include "scan.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace optiperf {
namespace {

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = char(std::toupper(static_cast<unsigned char>(c)));
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

template <class F>
void ForEachSection(char* base, DWORD required, F&& f) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if ((s->Characteristics & required) != required) continue;
        const size_t size = s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
        if (!f(base + s->VirtualAddress, size)) return;
    }
}

template <class F>
void ForEachCodeSection(char* base, F&& f) {
    ForEachSection(base, IMAGE_SCN_MEM_EXECUTE, f);
}

}  // namespace

Pattern ParsePattern(const char* text) {
    Pattern p;
    for (const char* c = text; *c;) {
        if (*c == ' ') {
            ++c;
        } else if (c[0] == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(0);
            c += (c[1] == '?') ? 2 : 1;
        } else {
            const int hi = Hex(c[0]), lo = c[1] ? Hex(c[1]) : -1;
            if (hi < 0 || lo < 0) return Pattern{};  // malformed pattern: matches nothing
            p.bytes.push_back(uint8_t(hi << 4 | lo));
            p.mask.push_back(1);
            c += 2;
        }
    }
    return p;
}

bool MatchesAt(const char* at, const Pattern& p) {
    for (size_t i = 0; i < p.size(); ++i)
        if (p.mask[i] && uint8_t(at[i]) != p.bytes[i]) return false;
    return !p.bytes.empty();
}

std::vector<char*> FindInCode(char* base, const Pattern& p, size_t maxMatches) {
    std::vector<char*> out;
    if (p.bytes.empty() || !p.mask[0]) return out;  // patterns start with a fixed byte
    const uint8_t first = p.bytes[0];
    ForEachCodeSection(base, [&](char* start, size_t size) {
        if (size < p.size()) return true;
        const char* last = start + size - p.size();
        for (char* at = start; at <= last; ++at) {
            at = static_cast<char*>(std::memchr(at, first, size_t(last - at) + 1));
            if (!at) break;
            if (MatchesAt(at, p)) {
                out.push_back(at);
                if (out.size() >= maxMatches) return false;
            }
        }
        return true;
    });
    return out;
}

std::vector<char*> FindInRange(char* start, size_t size, const Pattern& p, size_t maxMatches) {
    std::vector<char*> out;
    if (p.bytes.empty() || !p.mask[0]) return out;
    for (char* at = start; at < start + size; ++at) {
        at = static_cast<char*>(std::memchr(at, p.bytes[0], size_t(start + size - at)));
        if (!at) break;
        if (MatchesAt(at, p)) {
            out.push_back(at);
            if (out.size() >= maxMatches) break;
        }
    }
    return out;
}

bool IsInCode(char* base, const char* address, size_t length) {
    bool inside = false;
    ForEachCodeSection(base, [&](char* start, size_t size) {
        if (address >= start && address + length <= start + size) inside = true;
        return !inside;
    });
    return inside;
}

bool IsInImage(char* base, const char* address, size_t length) {
    bool inside = false;
    ForEachSection(base, 0, [&](char* start, size_t size) {
        if (address >= start && address + length <= start + size) inside = true;
        return !inside;
    });
    return inside;
}

void** FindRttiVtable(char* base, const char* mangled) {
#ifdef _WIN64
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const uint64_t preferred = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew)->OptionalHeader.ImageBase;
    const size_t len = std::strlen(mangled) + 1;
    // the type descriptor: its vtable, a spare pointer, then the name
    uint32_t td = 0;
    int found = 0;
    ForEachSection(base, IMAGE_SCN_CNT_INITIALIZED_DATA, [&](char* s, size_t n) {
        for (char* p = s;; ++p) {
            const size_t left = size_t(s + n - p);
            if (left < len) break;
            p = static_cast<char*>(std::memchr(p, mangled[0], left - len + 1));
            if (!p) break;
            if (std::memcmp(p, mangled, len) == 0 && p - base >= 16) {
                td = uint32_t(p - 16 - base);
                ++found;
            }
        }
        return true;
    });
    if (found != 1) return nullptr;
    // its complete object locator for offset 0 (x64, image-relative): signature, offset, constructor displacement,
    // type descriptor, class hierarchy descriptor (signature 0, attributes, base count, base array). The hierarchy's
    // check tells it from a base class descriptor of the class that zeros happen to precede (type descriptor, then
    // the count of its bases).
    const size_t image = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew)->OptionalHeader.SizeOfImage;
    uint32_t col = 0;
    found = 0;
    ForEachSection(base, IMAGE_SCN_CNT_INITIALIZED_DATA, [&](char* s, size_t n) {
        for (size_t o = 0; o + 20 <= n; o += 4) {
            uint32_t f[5];
            std::memcpy(f, s + o, sizeof f);
            if (f[3] != td || f[1] != 0 || f[0] > 1 || f[4] < 0x1000 || f[4] > image - 16) continue;
            uint32_t chd[4];
            std::memcpy(chd, base + f[4], sizeof chd);
            if (chd[0] != 0 || chd[2] == 0 || chd[2] > 256 || chd[3] < 0x1000 || chd[3] > image - 4) continue;
            col = uint32_t(s + o - base);
            ++found;
        }
        return true;
    });
    if (found != 1) return nullptr;
    // the vtable: right after the pointer to that locator
    void** vt = nullptr;
    found = 0;
    const uint64_t relocated = uint64_t(uintptr_t(base)) + col, unrelocated = preferred + col;
    ForEachSection(base, IMAGE_SCN_CNT_INITIALIZED_DATA, [&](char* s, size_t n) {
        for (size_t o = 0; o + 16 <= n; o += 8) {
            uint64_t v;
            std::memcpy(&v, s + o, sizeof v);
            if (v == relocated || v == unrelocated) {
                vt = reinterpret_cast<void**>(s + o + 8);
                ++found;
            }
        }
        return true;
    });
    return found == 1 ? vt : nullptr;
#else
    (void)base;
    (void)mangled;
    return nullptr;
#endif
}

// (see scan.h) x64 modules of build 6156 (VS2005) use locators with signature 0 and image-relative fields; x86 ones
// use absolute addresses.
std::vector<RttiVtable> FindRttiVtables(char* base, size_t imageSize, void* (*resolve)(void*)) {
    std::vector<RttiVtable> out;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    std::vector<std::pair<char*, char*>> code, data;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char* s = base + sec->VirtualAddress;
        char* e = s + (sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData);
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            code.push_back({s, e});
        else if ((sec->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) && !(sec->Characteristics & IMAGE_SCN_MEM_WRITE))
            data.push_back({s, e});
    }
    auto inCode = [&](const char* p) {
        if (resolve) p = static_cast<const char*>(resolve(const_cast<char*>(p)));
        for (auto& r : code)
            if (p >= r.first && p < r.second) return true;
        return false;
    };
    auto inImage = [&](const char* p, size_t n) {  // without pointer arithmetic: p + n can wrap on x86
        const uintptr_t a = reinterpret_cast<uintptr_t>(p), lo = reinterpret_cast<uintptr_t>(base);
        return a >= lo && a - lo <= imageSize && n <= imageSize - (a - lo);
    };
    struct Col {
        uint32_t offset;
        const char* name;
    };
    std::unordered_map<uintptr_t, Col> cols;
    constexpr size_t ps = sizeof(void*);
    for (auto& r : data) {
        for (char* o = r.first; o + 24 <= r.second; o += 4) {
            uint32_t f[5];
            std::memcpy(f, o, sizeof f);
#ifdef _WIN64
            if (f[0] > 1 || f[1] > 0x100000) continue;
            const char* td = base + f[3];
            if (f[3] == 0 || f[4] == 0 || f[4] >= imageSize) continue;
#else
            if (f[0] != 0 || f[1] > 0x100000) continue;
            const char* td = reinterpret_cast<const char*>(uintptr_t(f[3]));
            if (!inImage(reinterpret_cast<const char*>(uintptr_t(f[4])), 16)) continue;
#endif
            if (!inImage(td, 2 * ps + 8)) continue;
            const char* name = td + 2 * ps;
            if (std::memcmp(name, ".?AV", 4) != 0 && std::memcmp(name, ".?AU", 4) != 0) continue;
            if (!std::memchr(name, 0, (std::min)(size_t(512), size_t(base + imageSize - name)))) continue;
            cols[reinterpret_cast<uintptr_t>(o)] = Col{f[1], name};
        }
    }
    for (auto& r : data) {
        for (char* o = r.first; o + 2 * ps <= r.second; o += ps) {
            uintptr_t v;
            std::memcpy(&v, o, ps);
            auto it = cols.find(v);
            if (it == cols.end()) continue;
            char** vt = reinterpret_cast<char**>(o + ps);
            uint32_t n = 0;
            while (reinterpret_cast<char*>(vt + n + 1) <= r.second && inCode(vt[n])) ++n;
            if (n) out.push_back({it->second.name, it->second.offset, vt, n});
        }
    }
    return out;
}

}  // namespace optiperf
