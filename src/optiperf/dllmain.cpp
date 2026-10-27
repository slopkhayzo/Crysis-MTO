// optiperf.asi — runtime patches for Crysis (2007). Loaded by an ASI loader (e.g. Ultimate ASI Loader as version.dll
// next to Crysis.exe) or, for development, injected by optiperf_launch.exe.
//
// The plugin can be loaded before or after the engine DLLs it patches: it patches what is already mapped and watches
// for later loads with a loader notification (delivered when a DLL is mapped, before its DllMain runs). Every patch
// locates its code by pattern and verifies it once when hooking; anything unexpected disables that patch only.
// Nothing in the game folder is modified.

#include <windows.h>
#include <winternl.h>

#include <cwchar>
#include <string>

#include "allocator.h"
#include "calltrace.h"
#include "config.h"
#include "diag.h"
#include "log.h"
#include "particle_batch.h"
#include "shadow_prepass.h"

namespace {

struct LdrDllLoadedNotificationData {
    ULONG Flags;
    const UNICODE_STRING* FullDllName;
    const UNICODE_STRING* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
union LdrDllNotificationData {
    LdrDllLoadedNotificationData Loaded;
    LdrDllLoadedNotificationData Unloaded;
};
using LdrDllNotificationFn = VOID(CALLBACK*)(ULONG reason, const LdrDllNotificationData* data, PVOID context);
using LdrRegisterDllNotificationFn = NTSTATUS(NTAPI*)(ULONG flags, LdrDllNotificationFn fn, PVOID context, PVOID* cookie);
using LdrUnregisterDllNotificationFn = NTSTATUS(NTAPI*)(PVOID cookie);
constexpr ULONG kLdrDllNotificationReasonLoaded = 1;

// Modules any patch (or the call tracer) is interested in, for the case where they are already loaded when the plugin
// starts
const wchar_t* const kWatchedModules[] = {
    L"CrySystem.dll",     L"CryRenderD3D9.dll",  L"CryRenderD3D10.dll", L"Cry3DEngine.dll",     L"CryAction.dll",
    L"CryGame.dll",       L"CryEntitySystem.dll", L"CryAnimation.dll",   L"CryPhysics.dll",      L"CryScriptSystem.dll",
    L"CrySoundSystem.dll", L"CryNetwork.dll",     L"CryFont.dll",        L"CryInput.dll",        L"CryMovie.dll",
    L"CryAISystem.dll"};

PVOID g_cookie = nullptr;

void DispatchModule(const wchar_t* baseName, char* base, size_t size) {
    optiperf::Allocator_OnModuleLoaded(baseName, base, size);
    optiperf::ParticleBatch_OnModuleLoaded(baseName, base, size);
    optiperf::ShadowPrepass_OnModuleLoaded(baseName, base, size);
    optiperf::CallTrace_OnModuleLoaded(baseName, base, size);
}

VOID CALLBACK OnDllNotification(ULONG reason, const LdrDllNotificationData* data, PVOID) {
    if (reason != kLdrDllNotificationReasonLoaded || !data || !data->Loaded.BaseDllName) return;
    // BaseDllName is not guaranteed to be NUL-terminated
    wchar_t name[MAX_PATH];
    const size_t n = data->Loaded.BaseDllName->Length / sizeof(wchar_t);
    if (n == 0 || n >= MAX_PATH) return;
    wmemcpy(name, data->Loaded.BaseDllName->Buffer, n);
    name[n] = 0;
    DispatchModule(name, static_cast<char*>(data->Loaded.DllBase), data->Loaded.SizeOfImage);
}

void ReportAll(const char* when) {
    optiperf::Allocator_Report(when);
    optiperf::ParticleBatch_Report(when);
    optiperf::ShadowPrepass_Report(when);
    optiperf::CallTrace_Report(when);
}

// From the ExitProcess hook (diagnostics) or DLL detach, whichever comes first: the game does not always unload DLLs.
volatile LONG g_finalReported = 0;
void ReportFinal() {
    if (InterlockedExchange(&g_finalReported, 1) == 0) ReportAll("final");
}

// The game ends its process without unloading DLLs, so statistics are logged periodically.
DWORD WINAPI Reporter(void*) {
    for (;;) {
        Sleep(10000);
        ReportAll("running");
    }
}

std::wstring LogPath(const std::wstring& dir) {
    std::wstring p = optiperf::ConfigString(L"Log", L"File", L"OPTIPERF_LOG", L"");
    if (_wcsicmp(p.c_str(), L"none") == 0 || p == L"0") return L"";
    if (p.empty()) return dir + L"\\optiperf.log";
    const bool absolute = p.size() > 1 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\'));
    return absolute ? p : dir + L"\\" + p;
}

// Only the first optiperf in a process works: a second copy (an ASI loader's from the game folder, besides the one the
// development launcher injects first) would patch the same code again. A named event per process tells it so; it
// stays off, and opens no log (OPTIPERF_LOG may name the first copy's).
bool g_active = false;
bool FirstInProcess() {
    wchar_t name[64];
    swprintf(name, 64, L"Local\\optiperf-%lu", GetCurrentProcessId());
    HANDLE marker = CreateEventW(nullptr, TRUE, FALSE, name);
    if (!marker) return true;  // (no way to tell: work, as before)
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(marker);
        return false;
    }
    return true;  // (the handle stays open for the process's lifetime, the name with it)
}

void Attach(HMODULE self) {
    if (!FirstInProcess()) return;
    g_active = true;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir = path;
    dir.erase(dir.find_last_of(L'\\'));
    optiperf::ConfigInit(dir);
    optiperf::LogInit(LogPath(dir).c_str());

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    optiperf::Log("optiperf loaded into pid %lu (%ls) from %ls", GetCurrentProcessId(), exe, path);
    optiperf::Diag_Init();
    optiperf::Diag_SetExitCallback(ReportFinal);
    optiperf::CallTrace_Init();

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto reg = reinterpret_cast<LdrRegisterDllNotificationFn>(GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (!reg || reg(0, OnDllNotification, nullptr, &g_cookie) < 0)
        optiperf::Log("WARNING: LdrRegisterDllNotification unavailable; only modules loaded so far can be patched");

    // modules that were mapped before the plugin was loaded
    for (const wchar_t* name : kWatchedModules) {
        if (HMODULE m = GetModuleHandleW(name)) {
            auto base = reinterpret_cast<char*>(m);
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            optiperf::Log("%ls was already loaded", name);
            DispatchModule(name, base, nt->OptionalHeader.SizeOfImage);
        }
    }
    // the thread starts running once the loader lock is released
    if (HANDLE t = CreateThread(nullptr, 0, Reporter, nullptr, 0, nullptr)) CloseHandle(t);
}

void Detach() {
    if (!g_active) return;
    ReportFinal();
    if (g_cookie) {
        auto unreg = reinterpret_cast<LdrUnregisterDllNotificationFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrUnregisterDllNotification"));
        if (unreg) unreg(g_cookie);
        g_cookie = nullptr;
    }
    optiperf::Log("optiperf unloading");
    optiperf::LogClose();
}

}  // namespace

// Called by Ultimate ASI Loader right after loading the plugin, outside the loader lock. Everything already happened
// in DllMain (so the plugin also works with loaders that don't call this); it only confirms how the plugin was loaded.
extern "C" __declspec(dllexport) void InitializeASI() {
    if (g_active) optiperf::Log("InitializeASI called by the ASI loader");
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    // thread notifications are kept: the static CRT and mimalloc use them for per-thread cleanup
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            Attach(module);
            break;
        case DLL_PROCESS_DETACH:
            Detach();
            break;
    }
    return TRUE;
}
