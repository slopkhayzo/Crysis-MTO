#pragma once
// Function hooks (MinHook): other threads are frozen while hooks are written, and `original` receives a callable
// trampoline to the unhooked function. Works on x86 and x64.

#include <cstddef>

namespace optiperf {

struct HookSpec {
    const char* name;
    void* target;
    void* detour;
    void** original;  // may be null
};

bool InstallHook(void* target, void* detour, void** original, const char* name);

// All or nothing: either every hook is enabled (in one thread freeze) or none is.
bool InstallHooks(const HookSpec* hooks, size_t count);

// Address of an exported function, read from the export directory (works before the module is initialised).
void* FindExport(char* moduleBase, const char* name);

}  // namespace optiperf
