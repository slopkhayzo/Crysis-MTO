#pragma once
// Byte-pattern search over a loaded module's code, used instead of version or hash checks: a patch looks for the code
// it was written against and verifies it once when hooking. Patterns are "53 56 8B ?? ?? 0C" ('??' = any byte).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace optiperf {

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;  // 1 = must match
    size_t size() const { return bytes.size(); }
};

Pattern ParsePattern(const char* text);
bool MatchesAt(const char* at, const Pattern& p);

// Every match inside the module's executable sections (stops after maxMatches).
std::vector<char*> FindInCode(char* moduleBase, const Pattern& p, size_t maxMatches = 4);

// Every match starting inside [start, start + size) (the pattern may run past the end; the caller checks bounds).
std::vector<char*> FindInRange(char* start, size_t size, const Pattern& p, size_t maxMatches = 4);

// Executable section range containing `address`, for bounds checks on code we follow (call targets, etc.).
bool IsInCode(char* moduleBase, const char* address, size_t length);
// Any section of the module, for globals and constants that code refers to.
bool IsInImage(char* moduleBase, const char* address, size_t length);

// The primary vtable (at offset 0 of the complete object) of the class MSVC RTTI names `mangled`, e.g.
// ".?AVCCharInstance@@", in a mapped x64 module, relocated or not. Null if the module has none or several.
void** FindRttiVtable(char* moduleBase, const char* mangled);

// Every vtable MSVC RTTI describes in a mapped, relocated module: a complete object locator pointer right before each.
// A vtable runs as long as its entries lead into the module's executable sections, through `resolve` if given (the
// function behind an entry that is one of optiperf's thunks: CallTrace_Original).
struct RttiVtable {
    const char* mangled;  // type descriptor name, e.g. ".?AVCD3D9Renderer@@"
    uint32_t offset;      // subobject offset of this vtable in the complete object
    char** vtable;
    uint32_t slots;
};
std::vector<RttiVtable> FindRttiVtables(char* moduleBase, size_t imageSize, void* (*resolve)(void*) = nullptr);

inline uint32_t ReadU32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
// Target of the E8/E9 rel32 instruction at `instr`.
inline char* RelTarget(char* instr) { return instr + 5 + int32_t(ReadU32(instr + 1)); }

}  // namespace optiperf
