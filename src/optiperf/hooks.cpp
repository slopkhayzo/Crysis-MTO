#include "hooks.h"

#include <windows.h>

#include <MinHook.h>

#include <cstring>

#include "log.h"

namespace optiperf {
namespace {

bool EnsureInit() {
    static const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MinHook init failed (%s)", MH_StatusToString(init));
        return false;
    }
    return true;
}

}  // namespace

bool InstallHook(void* target, void* detour, void** original, const char* name) {
    const HookSpec spec{name, target, detour, original};
    return InstallHooks(&spec, 1);
}

bool InstallHooks(const HookSpec* hooks, size_t count) {
    if (!EnsureInit()) return false;
    size_t created = 0;
    MH_STATUS s = MH_OK;
    for (; created < count; ++created) {
        void* trampoline = nullptr;
        s = MH_CreateHook(hooks[created].target, hooks[created].detour, &trampoline);
        if (s != MH_OK) break;
        if (hooks[created].original) *hooks[created].original = trampoline;
        s = MH_QueueEnableHook(hooks[created].target);
        if (s != MH_OK) {
            ++created;
            break;
        }
    }
    if (s == MH_OK) s = MH_ApplyQueued();
    if (s == MH_OK) return true;
    Log("hooks not installed: %s failed (%s)", hooks[created ? created - 1 : 0].name, MH_StatusToString(s));
    for (size_t i = 0; i < created; ++i) MH_RemoveHook(hooks[i].target);
    return false;
}

void* FindExport(char* base, const char* name) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return nullptr;
    auto exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
    auto ordinals = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
    auto functions = reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        if (std::strcmp(base + names[i], name) != 0) continue;
        const DWORD rva = functions[ordinals[i]];
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) return nullptr;  // forwarder
        return base + rva;
    }
    return nullptr;
}

}  // namespace optiperf
