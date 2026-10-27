#include "diag.h"

#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

#include "config.h"
#include "hooks.h"
#include "log.h"

namespace optiperf {
namespace {

volatile LONG g_exceptionsLogged = 0;
constexpr LONG kMaxExceptions = 12;

// Thread names from the MSVC debugger convention: RaiseException(0x406D1388) with {0x1000, name, thread id, flags}
constexpr DWORD kSetThreadNameException = 0x406D1388;
SRWLOCK g_namesLock = SRWLOCK_INIT;
std::map<DWORD, std::string>* g_names = nullptr;

bool CopyName(const char* s, char* out, size_t size) {  // no C++ objects here: __try needs a frame without unwinding
    __try {
        for (size_t i = 0; s && i + 1 < size && s[i]; ++i) out[i] = s[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RememberThreadName(const EXCEPTION_RECORD* r) {
    // the record is copied as ULONG_PTRs: on x64 the 32-bit fields share slots with padding or each other
    if (r->NumberParameters < 3 || DWORD(r->ExceptionInformation[0]) != 0x1000) return;
    char name[64] = "";
    if (!CopyName(reinterpret_cast<const char*>(r->ExceptionInformation[1]), name, sizeof name)) return;
    const DWORD tid = DWORD(r->ExceptionInformation[2]) == DWORD(-1) ? GetCurrentThreadId() : DWORD(r->ExceptionInformation[2]);
    AcquireSRWLockExclusive(&g_namesLock);
    if (!g_names) g_names = new std::map<DWORD, std::string>;
    (*g_names)[tid] = name;
    ReleaseSRWLockExclusive(&g_namesLock);
}

using ExitProcessFn = VOID(WINAPI*)(UINT);
using MessageBoxAFn = int(WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
using MessageBoxWFn = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
ExitProcessFn g_exitProcess = nullptr;
void (*g_exitCallback)() = nullptr;
volatile LONG g_exitCalled = 0;
MessageBoxAFn g_messageBoxA = nullptr;
MessageBoxWFn g_messageBoxW = nullptr;

HMODULE ModuleOf(const void* address) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &m);
    return m;
}

// True if the bytes before `ret` decode as a call, i.e. `ret` is a plausible return address.
bool IsCallSite(const uint8_t* ret) {
    __try {
        if (ret[-5] == 0xE8) return true;                                              // call rel32
        if (ret[-6] == 0xFF && (ret[-5] == 0x15 || (ret[-5] & 0xF8) == 0x90)) return true;  // call [rip/abs] / [reg+d32]
        if (ret[-7] == 0xFF && ret[-6] == 0x94) return true;                          // call [sib+d32]
        if (ret[-3] == 0xFF && (ret[-2] & 0xF8) == 0x50) return true;                 // call [reg+d8]
        if (ret[-4] == 0xFF && ret[-3] == 0x54) return true;                          // call [sib+d8]
        if (ret[-2] == 0xFF && ((ret[-1] & 0xF8) == 0xD0 || (ret[-1] & 0xF8) == 0x10)) return true;  // call reg / [reg]
        if (ret[-3] == 0x41 && ret[-2] == 0xFF && (ret[-1] & 0xF8) == 0xD0) return true;  // call r8-r15
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool IsGameModule(HMODULE m) {
    if (!m) return false;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(m, path, MAX_PATH)) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsnicmp(name, L"Cry", 3) == 0;
}

bool SafeRead(const uintptr_t* p, uintptr_t* v) {
    __try {
        *v = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Return addresses into the game's own modules (or any module's, `all`) found on the stack above `sp` (heuristic,
// like a profiler's scan).
std::string GameCallChain(const void* sp, unsigned words = 2048, bool all = false, int most = 10) {
    std::string out;
    int found = 0;
    auto* p = static_cast<const uintptr_t*>(sp);
    for (unsigned i = 0; i < words && found < most; ++i) {
        uintptr_t v;
        if (!SafeRead(p + i, &v)) break;
        HMODULE m = ModuleOf(reinterpret_cast<const void*>(v));
        if (!m || (!all && !IsGameModule(m)) || v - reinterpret_cast<uintptr_t>(m) < 0x1000) continue;  // (header)
        if (!IsCallSite(reinterpret_cast<const uint8_t*>(v))) continue;
        char buf[160];
        out += out.empty() ? "" : " <- ";
        out += DescribeAddress(reinterpret_cast<const void*>(v), buf, sizeof buf);
        ++found;
    }
    return out.empty() ? "(none found)" : out;
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == kSetThreadNameException) {
        RememberThreadName(e->ExceptionRecord);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const bool crashType = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                           code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW ||
                           code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == STATUS_HEAP_CORRUPTION;
    if (!crashType) return EXCEPTION_CONTINUE_SEARCH;
    static const HMODULE self = ModuleOf(reinterpret_cast<const void*>(&OnException));
    if (ModuleOf(e->ExceptionRecord->ExceptionAddress) == self) return EXCEPTION_CONTINUE_SEARCH;  // our own guarded reads
    if (InterlockedIncrement(&g_exceptionsLogged) > kMaxExceptions) return EXCEPTION_CONTINUE_SEARCH;
    char where[160];
    DescribeAddress(e->ExceptionRecord->ExceptionAddress, where, sizeof where);
#ifdef _WIN64
    const void* sp = reinterpret_cast<const void*>(e->ContextRecord->Rsp);
#else
    const void* sp = reinterpret_cast<const void*>(e->ContextRecord->Esp);
#endif
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR kind = e->ExceptionRecord->ExceptionInformation[0];
        Log("EXCEPTION (first chance) access violation at %s: %s 0x%p | thread %lu | game call chain: %s", where,
            kind == 0 ? "read of" : kind == 1 ? "write to" : "execute at",
            reinterpret_cast<void*>(e->ExceptionRecord->ExceptionInformation[1]), GetCurrentThreadId(),
            GameCallChain(sp).c_str());
    } else {
        Log("EXCEPTION (first chance) 0x%08lX at %s | thread %lu | game call chain: %s", code, where, GetCurrentThreadId(),
            GameCallChain(sp).c_str());
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

VOID WINAPI MyExitProcess(UINT code) {
    char who[160];
    Log("ExitProcess(%u) called from %s | game call chain: %s", code, DescribeAddress(_ReturnAddress(), who, sizeof who),
        GameCallChain(_AddressOfReturnAddress()).c_str());
    if (g_exitCallback && InterlockedExchange(&g_exitCalled, 1) == 0) g_exitCallback();
    g_exitProcess(code);
}

// Automated test runs set OPTIPERF_NO_DIALOGS=1: an error box would otherwise wait forever for a click.
bool NoDialogs() {
    static const bool v = ConfigString(L"Debug", L"NoDialogs", L"OPTIPERF_NO_DIALOGS", L"0") == L"1";
    return v;
}

int WINAPI MyMessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type) {
    Log("MessageBox \"%s\": %s | game call chain: %s", caption ? caption : "", text ? text : "",
        GameCallChain(_AddressOfReturnAddress()).c_str());
    if (NoDialogs()) return IDOK;
    return g_messageBoxA(w, text, caption, type);
}

int WINAPI MyMessageBoxW(HWND w, LPCWSTR text, LPCWSTR caption, UINT type) {
    Log("MessageBox \"%ls\": %ls | game call chain: %s", caption ? caption : L"", text ? text : L"",
        GameCallChain(_AddressOfReturnAddress()).c_str());
    if (NoDialogs()) return IDOK;
    return g_messageBoxW(w, text, caption, type);
}

}  // namespace

const char* DescribeAddress(const void* address, char* buffer, unsigned size) {
    HMODULE m = ModuleOf(address);
    wchar_t path[MAX_PATH];
    if (m && GetModuleFileNameW(m, path, MAX_PATH)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        snprintf(buffer, size, "%ls+0x%llX", name ? name + 1 : path,
                 static_cast<unsigned long long>(static_cast<const char*>(address) - reinterpret_cast<const char*>(m)));
    } else {
        snprintf(buffer, size, "0x%p", address);
    }
    return buffer;
}

void Diag_SetExitCallback(void (*callback)()) { g_exitCallback = callback; }

void Diag_CallChain(const void* stack, unsigned words, char* buffer, unsigned bufferSize) {
    snprintf(buffer, bufferSize, "%s", GameCallChain(stack, words).c_str());
}

void Diag_FullCallChain(const void* stack, unsigned words, unsigned most, char* buffer, unsigned bufferSize) {
    snprintf(buffer, bufferSize, "%s", GameCallChain(stack, words, true, int(most)).c_str());
}

bool Diag_ThreadName(unsigned long threadId, char* buffer, unsigned bufferSize) {
    bool found = false;
    AcquireSRWLockShared(&g_namesLock);
    if (g_names) {
        auto it = g_names->find(threadId);
        if (it != g_names->end()) {
            snprintf(buffer, bufferSize, "%s", it->second.c_str());
            found = true;
        }
    }
    ReleaseSRWLockShared(&g_namesLock);
    return found;
}

void Diag_Init() {
    const std::wstring on = ConfigString(L"Debug", L"Diagnostics", L"OPTIPERF_DIAGNOSTICS", L"1");
    if (on == L"0" || _wcsicmp(on.c_str(), L"off") == 0) return;
    AddVectoredExceptionHandler(1, OnException);
    if (void* p = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitProcess"))
        InstallHook(p, reinterpret_cast<void*>(&MyExitProcess), reinterpret_cast<void**>(&g_exitProcess), "ExitProcess");
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (void* p = GetProcAddress(user32, "MessageBoxA"))
            InstallHook(p, reinterpret_cast<void*>(&MyMessageBoxA), reinterpret_cast<void**>(&g_messageBoxA), "MessageBoxA");
        if (void* p = GetProcAddress(user32, "MessageBoxW"))
            InstallHook(p, reinterpret_cast<void*>(&MyMessageBoxW), reinterpret_cast<void**>(&g_messageBoxW), "MessageBoxW");
    }
}

}  // namespace optiperf
