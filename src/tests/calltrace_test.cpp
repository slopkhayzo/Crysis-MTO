// Call tracer outside the game: traces calltrace_testlib.dll, calls its methods in every way the engine might (stack and
// register arguments, float/64-bit/struct returns, variadic, callbacks into the caller, SEH exceptions crossing a
// traced call, calls and tail calls from inside the module, recursion deeper than the timing stack, several threads),
// checks every result, then checks the counts the tracer wrote against the calls made.
//
//   calltrace_test.exe        (needs calltrace_testlib.dll next to it; writes into .\calltrace_test_out\)

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../optiperf/calltrace.h"
#include "../optiperf/config.h"
#include "../optiperf/diag.h"
#include "../optiperf/log.h"
#include "calltrace_testlib.h"

namespace {

int g_failures = 0;
uint64_t g_expected[kSlots] = {};
std::atomic<uint64_t> g_threadCalls{0};
std::atomic<int> g_threadFailures{0};

void Check(bool ok, const char* what) {
    if (!ok && ++g_failures <= 20) std::printf("FAIL: %s\n", what);
}

int ReferenceCall(int depth) {  // what ITarget::Call(&callback, depth) must return
    return (depth + 1) + 100 + 5 + (depth > 0 ? ReferenceCall(depth - 1) : 0) + 1;
}

struct Callback : ICallback {
    ITarget* t = nullptr;
    int Run(int depth) override {
        int r = t->Add(depth, 1);
        ++g_expected[kAdd];
        ++g_expected[kThrow];
        __try {
            t->Throw(depth);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            r += 100;
        }
        r += t->Add(2, 3);  // the first traced call after a frame went stale
        ++g_expected[kAdd];
        if (depth > 0) {
            ++g_expected[kCall];
            r += t->Call(this, depth - 1);
        }
        return r;
    }
};

int ThrowAndCatch(ITarget* t, int code) {  // no C++ objects here: __try needs a frame without unwinding
    __try {
        t->Throw(code);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return int(GetExceptionCode() & 0xFFFF);
    }
}

DWORD WINAPI Worker(void* p) {
    ITarget* t = static_cast<ITarget*>(p);
    for (int i = 0; i < 100000; ++i) {
        if (t->Add(i, 7) != i + 7 || t->Sum(2, i, 1) != i + 1) g_threadFailures.fetch_add(1);
    }
    g_threadCalls.fetch_add(100000);
    return 0;
}

std::vector<std::vector<std::string>> ReadTsv(const std::wstring& path) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, '\t')) cols.push_back(c);
        rows.push_back(cols);
    }
    return rows;
}

LONG CALLBACK PrintCrash(EXCEPTION_POINTERS* e) {  // the plugin's diagnostics ignore faults in their own module (here: us)
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG reported = 0;
    if (InterlockedIncrement(&reported) > 2) return EXCEPTION_CONTINUE_SEARCH;
    char where[160];
    optiperf::Log("TEST CRASH 0x%08lX at %s (thread %lu)", code,
                  optiperf::DescribeAddress(e->ExceptionRecord->ExceptionAddress, where, sizeof where), GetCurrentThreadId());
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) optiperf::Log("    %s", optiperf::DescribeAddress(frames[i], where, sizeof where));
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

int main(int argc, char** argv) {
    // "notrace": run without the tracer; "diag": also install the plugin's crash diagnostics (its ExitProcess hook
    // faults inside ntdll at the end of this test's own CRT exit, so it is off by default; the game is not affected)
    bool trace = true, diag = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "notrace") == 0) trace = false;
        if (std::strcmp(argv[i], "diag") == 0) diag = true;
    }
    AddVectoredExceptionHandler(1, PrintCrash);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.erase(dir.find_last_of(L'\\'));
    const std::wstring out = dir + L"\\calltrace_test_out";
    CreateDirectoryW(out.c_str(), nullptr);
    SetEnvironmentVariableW(L"OPTIPERF_TRACE", L"calltrace_testlib.dll");
    SetEnvironmentVariableW(L"OPTIPERF_LOG", (out + L"\\optiperf.log").c_str());
    optiperf::ConfigInit(dir);
    optiperf::LogInit((out + L"\\optiperf.log").c_str());
    if (diag) optiperf::Diag_Init();
    if (trace) optiperf::CallTrace_Init();

    HMODULE lib = LoadLibraryW(L"calltrace_testlib.dll");
    if (!lib) {
        std::printf("FAIL: calltrace_testlib.dll not found\n");
        return 1;
    }
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<char*>(lib) +
                                                        reinterpret_cast<const IMAGE_DOS_HEADER*>(lib)->e_lfanew);
    optiperf::CallTrace_OnModuleLoaded(L"calltrace_testlib.dll", reinterpret_cast<char*>(lib), nt->OptionalHeader.SizeOfImage);
    auto create = reinterpret_cast<ITarget* (*)()>(GetProcAddress(lib, "CreateTarget"));
    ITarget* t = create();
    char what[128];

    // calling conventions and return types
    const int kN = 200000;
    for (int i = 0; i < kN; ++i) {
        Check(t->Add(i, 3) == i + 3, "Add");
        Check(t->Scale(float(i), 0.5f) == float(i) * 0.5f, "Scale (float return)");
        Check(t->Mul(double(i), 1.5) == double(i) * 1.5, "Mul (double arguments and return)");
        Check(t->Wide(0x100000000LL + i, 3) == (0x100000000LL + i) * 3, "Wide (64-bit return)");
        const Vec3 v = t->Make(float(i), 2.0f, 3.0f);
        Check(v.x == float(i) && v.y == 2.0f && v.z == 3.0f, "Make (struct return)");
        Check(t->Sum(3, i, 2, 3) == i + 5, "Sum (variadic)");
        Check(t->Outer(i) == (i + 1) * 2, "Outer (calls Inner inside the module)");
        Check(t->Tail(i) == i + 2, "Tail (tail-calls Inner)");
    }
    for (TargetSlot s : {kAdd, kScale, kMul, kWide, kMake, kSum, kOuter, kTail}) g_expected[s] += kN;

    // SEH exceptions out of traced calls, caught by the caller; traced calls must keep working afterwards
    for (int i = 1; i <= 1000; ++i) {
        snprintf(what, sizeof what, "exception %d caught", i);
        Check(ThrowAndCatch(t, i) == i, what);
        Check(t->Add(i, i) == 2 * i, "Add after an exception");
    }
    g_expected[kThrow] += 1000;
    g_expected[kAdd] += 1000;

    // callbacks into this module that call back into the traced one (nested traced calls, exceptions in between)
    Callback cb;
    cb.t = t;
    for (int i = 0; i < 1000; ++i) {
        ++g_expected[kCall];
        Check(t->Call(&cb, 3) == ReferenceCall(3), "nested callbacks");
    }
    // deeper than the timing stack (128 frames): those calls are counted but not timed
    ++g_expected[kCall];
    Check(t->Call(&cb, 150) == ReferenceCall(150), "recursion deeper than the timing stack");

    // several threads at once
    HANDLE threads[4];
    for (HANDLE& h : threads) h = CreateThread(nullptr, 0, Worker, t, 0, nullptr);
    WaitForMultipleObjects(4, threads, TRUE, INFINITE);
    for (HANDLE h : threads) CloseHandle(h);
    Check(g_threadFailures == 0, "results on worker threads");
    g_expected[kAdd] += g_threadCalls;
    g_expected[kSum] += g_threadCalls;

    optiperf::CallTrace_Report("final");

    // what the tracer wrote
    std::map<int, int> slotOf;  // Target vtable index -> trace slot id
    for (auto& r : ReadTsv(out + L"\\optiperf_trace_slots.tsv"))
        if (r.size() >= 8 && r[3].rfind(".?AUTarget@", 0) == 0 && r[4] == "0")  // anonymous namespace: .?AUTarget@?A0x...@@
            slotOf[std::stoi(r[6])] = std::stoi(r[0]);
    Check(slotOf.size() >= kSlots, "Target vtable hooked");
    std::map<int, uint64_t> ext, internal, incl, excl;
    for (auto& r : ReadTsv(out + L"\\optiperf_trace_series.tsv")) {
        if (r.size() < 7) continue;
        const int thread = std::stoi(r[2]), slot = std::stoi(r[3]);
        if (thread >= 0) {
            ext[slot] += std::stoull(r[4]);
            incl[slot] += std::stoull(r[5]);
            excl[slot] += std::stoull(r[6]);
        } else if (thread == -1) {
            internal[slot] += std::stoull(r[4]);
        }
    }
    const char* names[kSlots] = {"Add", "Scale", "Mul", "Wide", "Make", "Sum", "Call", "Throw", "Outer", "Inner", "Tail"};
    for (int s = 0; s < kSlots; ++s) {
        const int id = slotOf.count(s) ? slotOf[s] : -1;
        std::printf("  %-6s external %8llu (expected %8llu)  internal %7llu  incl %12llu  excl %12llu cycles\n", names[s],
                    ext[id], g_expected[s], internal[id], incl[id], excl[id]);
        snprintf(what, sizeof what, "external count of %s", names[s]);
#ifdef _WIN64
        if (s == kInner) continue;  // see below
#endif
        Check(ext[id] == g_expected[s], what);
    }
#ifdef _WIN64
    // without timing a tail call keeps the original caller's return address: Tail's jump to Inner looks external
    Check(internal[slotOf[kInner]] + ext[slotOf[kInner]] == 2ull * kN, "Inner calls (Outer + Tail) all counted");
#else
    Check(internal[slotOf[kInner]] == 2ull * kN, "Inner counted as called from inside the module (Outer + Tail)");
#endif
    uint64_t stale = 0, untimed = 0;
    for (auto& r : ReadTsv(out + L"\\optiperf_trace_threads.tsv"))
        if (r.size() >= 7) untimed += std::stoull(r[5]), stale += std::stoull(r[6]);
#ifndef _WIN64
    // calls made while Call was running (from the callback) are also listed under Call: ret = Call's slot + 1
    uint64_t nestedAdd = 0;
    for (auto& r : ReadTsv(out + L"\\optiperf_trace_sites.tsv"))
        if (r.size() >= 4 && std::stoull(r[2], nullptr, 16) == uint64_t(slotOf[kCall]) + 1 && std::stoi(r[1]) == slotOf[kAdd])
            nestedAdd += std::stoull(r[3]);
    std::printf("Add calls made inside Call: %llu\n", nestedAdd);
    Check(nestedAdd == 2ull * g_expected[kCall], "Add calls inside Call recorded as nested (two per callback)");
    Check(incl[slotOf[kAdd]] > 0 && excl[slotOf[kAdd]] <= incl[slotOf[kAdd]], "Add timed");
    Check(incl[slotOf[kCall]] > excl[slotOf[kCall]], "Call's nested traced calls subtracted from its exclusive time");
    Check(stale >= 1000, "frames of calls left by exceptions dropped");
    Check(untimed > 0, "calls deeper than the timing stack counted as untimed");
#endif
    std::printf("stale frames dropped: %llu, untimed calls: %llu\n", stale, untimed);
    std::printf(g_failures ? "calltrace_test: FAILED (%d)\n" : "calltrace_test: all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
