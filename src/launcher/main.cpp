// optiperf_launch.exe — development/test launcher: starts Crysis with optiperf.asi injected, without installing an
// ASI loader in the game folder. (Players use an ASI loader instead; see src/README.md.)
//
// usage: optiperf_launch.exe "<path>\Bin32\Crysis.exe" [game arguments...]
//
// The game is created suspended, optiperf.asi (next to this exe) is loaded into it with a remote LoadLibraryW, then
// the game is resumed. The launcher waits for the game to exit and returns its exit code, so scripts can wait on it.
// Build the launcher with the same bitness as the game (x86 for Bin32, x64 for Bin64).

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

std::wstring Quote(const std::wstring& s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    return out + L"\"";
}

WORD ExeMachine(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    IMAGE_DOS_HEADER dos{};
    IMAGE_FILE_HEADER fh{};
    DWORD sig = 0, n = 0;
    WORD machine = 0;
    if (ReadFile(f, &dos, sizeof dos, &n, nullptr) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        SetFilePointer(f, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        ReadFile(f, &sig, sizeof sig, &n, nullptr) && sig == IMAGE_NT_SIGNATURE && ReadFile(f, &fh, sizeof fh, &n, nullptr))
        machine = fh.Machine;
    CloseHandle(f);
    return machine;
}

int Fail(const char* what) {
    fprintf(stderr, "optiperf_launch: %s (error %lu)\n", what, GetLastError());
    return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: optiperf_launch.exe <Crysis.exe> [game arguments...]\n");
        return 2;
    }
    const std::wstring exe = argv[1];
#ifdef _WIN64
    const WORD ourMachine = IMAGE_FILE_MACHINE_AMD64;
#else
    const WORD ourMachine = IMAGE_FILE_MACHINE_I386;
#endif
    const WORD machine = ExeMachine(exe);
    if (machine == 0) return Fail("cannot read the game executable");
    if (machine != ourMachine) {
        fprintf(stderr, "optiperf_launch: %ls is %s but this launcher is %s; use the matching build\n", exe.c_str(),
                machine == IMAGE_FILE_MACHINE_I386 ? "x86" : "x64", ourMachine == IMAGE_FILE_MACHINE_I386 ? "x86" : "x64");
        return 2;
    }

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dll = self;
    dll = dll.substr(0, dll.find_last_of(L'\\') + 1) + L"optiperf.asi";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return Fail("optiperf.asi not found next to the launcher");

    std::wstring cmd = Quote(exe);
    for (int i = 2; i < argc; ++i) cmd += L" " + Quote(argv[i]);
    const std::wstring dir = exe.substr(0, exe.find_last_of(L"\\/"));

    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi))
        return Fail("CreateProcess failed");

    // remote LoadLibraryW(dll); kernel32 is mapped at the same address in every process of the same bitness
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    auto loadLibrary =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = nullptr;
    DWORD loaded = 0;
    if (remote && WriteProcessMemory(pi.hProcess, remote, dll.c_str(), bytes, nullptr) &&
        (thread = CreateRemoteThread(pi.hProcess, nullptr, 0, loadLibrary, remote, 0, nullptr)) != nullptr &&
        WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0)
        GetExitCodeThread(thread, &loaded);
    if (thread) CloseHandle(thread);
    if (remote) VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    if (!loaded) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return Fail("injecting optiperf.asi failed");
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return int(code);
}
