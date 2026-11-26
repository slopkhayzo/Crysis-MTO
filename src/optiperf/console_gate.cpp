// The console's gate: see console_gate.h.
//
// CrySystem's cvar classes (CXConsoleVariableInt, IntRef, Float, FloatRef, String, CVarGroup) keep their value in the
// object or where the owning module asked (the renderer's statics, which the flush reads), and call their change
// callback from the setter (the renderer's, for its own cvars). Their setters' slots (ICVar 4 Set(int), 5
// Set(float), 6 Set(const char*), 7 ForceSet; MSVC lays the overloads out reversed) are patched in the six vtables;
// the cvar part (cvar_snapshot.cpp), which patches two of the classes' setters at the first flush, calls on to these
// hooks as to the entries it found. CXConsole::ExecuteString (slot 31) runs a command line, or queues it for the
// console's Update (slot 26), which runs what was queued. Beside the flush, each joins it first: a cvar set, a command,
// and an Update while queued commands wait (a queued command that waits frames itself, with `wait`, may run in a
// later Update beside a flush: not seen).
#include "console_gate.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "flush_thread.h"
#include "log.h"
#include "scan.h"

namespace optiperf {
namespace {

#ifdef _WIN64
constexpr int kClasses = 6, kSetters = 4;
const char* const kClassNames[kClasses] = {".?AVCXConsoleVariableInt@@",      ".?AVCXConsoleVariableIntRef@@",
                                           ".?AVCXConsoleVariableFloat@@",    ".?AVCXConsoleVariableFloatRef@@",
                                           ".?AVCXConsoleVariableString@@", ".?AVCXConsoleVariableCVarGroup@@"};
constexpr unsigned kFirstSetter = 4;   // ICVar: 4 Set(int), 5 Set(float), 6 Set(const char*), 7 ForceSet
constexpr unsigned kSlotGetName = 12;  // ICVar::GetName
constexpr unsigned kSlotUpdate = 26, kSlotExecuteString = 31;  // IConsole
constexpr int kLoggedNames = 8;

using SetIntFn = void (*)(void*, int);
using SetFloatFn = void (*)(void*, float);
using SetStringFn = void (*)(void*, const char*);
using NameFn = const char* (*)(void*);
using ExecuteFn = void (*)(void*, const char*, bool, bool);
using UpdateFn = void (*)(void*);

struct Pending {  // [Debug] Commands: a command, and the outer flushes after which it runs
    uint64_t atFlush;
    std::string command;
};
struct State {
    bool installed = false;
    void* orig[kClasses][kSetters] = {};  // the entries found in the slots
    void* execute = nullptr;
    void* update = nullptr;
    unsigned queued = 0;  // commands queued for the console's Update, since its last
    uint64_t setJoins = 0, commandJoins = 0, updateJoins = 0;
    int loggedNames = 0;
    bool commandsRead = false;
    std::vector<Pending> commands;
    size_t nextCommand = 0;
    volatile uint64_t flushes = 0;  // outer flushes so far (ConsoleGate_OnFlushEntry)
};
State g;
void ExecuteHook(void* console, const char* command, bool silent, bool defer);

// beside the flush: join it first (a cvar's name logged, the first ones)
void GateSet(void* var) {
    if (!FlushThread_Beside()) return;
    ++g.setJoins;
    if (g.loggedNames < kLoggedNames) {
        ++g.loggedNames;
        const char* const name = (*reinterpret_cast<NameFn* const*>(var))[kSlotGetName](var);
        Log("console gate: the cvar %s set beside the flush: it joins first", name ? name : "?");
    }
    FlushThread_JoinNow();
}
template <int C>
void SetIntHook(void* var, int v) {
    GateSet(var);
    reinterpret_cast<SetIntFn>(g.orig[C][0])(var, v);
}
template <int C>
void SetFloatHook(void* var, float v) {
    GateSet(var);
    reinterpret_cast<SetFloatFn>(g.orig[C][1])(var, v);
}
template <int C>
void SetStringHook(void* var, const char* v) {
    GateSet(var);
    reinterpret_cast<SetStringFn>(g.orig[C][2])(var, v);
}
template <int C>
void ForceSetHook(void* var, const char* v) {
    GateSet(var);
    reinterpret_cast<SetStringFn>(g.orig[C][3])(var, v);
}
template <int C>
void* const kHooks[kSetters] = {reinterpret_cast<void*>(&SetIntHook<C>), reinterpret_cast<void*>(&SetFloatHook<C>),
                                reinterpret_cast<void*>(&SetStringHook<C>), reinterpret_cast<void*>(&ForceSetHook<C>)};
void* const* const kClassHooks[kClasses] = {kHooks<0>, kHooks<1>, kHooks<2>, kHooks<3>, kHooks<4>, kHooks<5>};

void ExecuteHook(void* console, const char* command, bool silent, bool defer) {
    if (FlushThread_Beside()) {
        ++g.commandJoins;
        FlushThread_JoinNow();
    }
    if (defer) ++g.queued;
    reinterpret_cast<ExecuteFn>(g.execute)(console, command, silent, defer);
}
void UpdateHook(void* console) {
    while (g.nextCommand < g.commands.size() && g.commands[g.nextCommand].atFlush <= g.flushes) {
        const Pending& p = g.commands[g.nextCommand++];  // ([Debug] Commands: as typed in the console, joining first)
        Log("console gate: [Debug] Commands, after %llu flushes: \"%s\"", static_cast<unsigned long long>(g.flushes),
            p.command.c_str());
        ExecuteHook(console, p.command.c_str(), false, false);
    }
    if (g.queued && FlushThread_Beside()) {  // (it runs the queued commands)
        ++g.updateJoins;
        FlushThread_JoinNow();
    }
    g.queued = 0;
    reinterpret_cast<UpdateFn>(g.update)(console);
}

// a vtable slot pointed at fn (one aligned store), its entry kept
bool PatchSlot(void** slot, void* fn, void** old) {
    DWORD protect;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
    *old = *slot;
    InterlockedExchangePointer(slot, fn);
    VirtualProtect(slot, sizeof(void*), protect, &protect);
    return true;
}
#endif

}  // namespace

void ConsoleGate_Install() {
#ifdef _WIN64
    if (g.installed) return;
    char* const sys = reinterpret_cast<char*>(GetModuleHandleW(L"CrySystem.dll"));
    if (!sys) {
        Log("console gate: not installed (CrySystem.dll not loaded): the console's calls don't join the flush");
        return;
    }
    void** vts[kClasses];
    for (int c = 0; c < kClasses; ++c) {
        vts[c] = FindRttiVtable(sys, kClassNames[c]);
        if (!vts[c]) {
            Log("console gate: not installed (%s's vtable not found)", kClassNames[c]);
            return;
        }
    }
    void** const console = FindRttiVtable(sys, ".?AVCXConsole@@");
    if (!console) {
        Log("console gate: not installed (CXConsole's vtable not found)");
        return;
    }
    for (int c = 0; c < kClasses; ++c)
        for (int i = 0; i < kSetters; ++i)
            if (!PatchSlot(&vts[c][kFirstSetter + i], kClassHooks[c][i], &g.orig[c][i])) {
                Log("console gate: could not write %s's vtable: the cvar sets after it don't join the flush",
                    kClassNames[c]);
                return;
            }
    if (!PatchSlot(&console[kSlotExecuteString], reinterpret_cast<void*>(&ExecuteHook), &g.execute) ||
        !PatchSlot(&console[kSlotUpdate], reinterpret_cast<void*>(&UpdateHook), &g.update)) {
        Log("console gate: could not write CXConsole's vtable: its commands don't join the flush");
        return;
    }
    g.installed = true;
    Log("console gate: the setters of CrySystem's six cvar classes, CXConsole's ExecuteString and Update join the "
        "flush first beside it");
#endif
}

void ConsoleGate_OnFlushEntry() {
#ifdef _WIN64
    if (!g.commandsRead) {
        g.commandsRead = true;
        const std::wstring v = ConfigString(L"Debug", L"Commands", L"OPTIPERF_DEBUG_COMMANDS", L"");
        std::string s;  // (ASCII: console commands)
        for (const wchar_t ch : v) s.push_back(ch < 0x80 ? char(ch) : '?');
        size_t at = 0;
        while (at < s.size()) {  // "N:cmd;cmd|M:cmd"
            size_t end = s.find('|', at);
            if (end == std::string::npos) end = s.size();
            const std::string group = s.substr(at, end - at);
            const size_t colon = group.find(':');
            if (colon != std::string::npos) {
                const uint64_t n = std::strtoull(group.substr(0, colon).c_str(), nullptr, 10);
                size_t c = colon + 1;
                while (c <= group.size()) {
                    size_t semi = group.find(';', c);
                    if (semi == std::string::npos) semi = group.size();
                    if (semi > c) g.commands.push_back({n, group.substr(c, semi - c)});
                    c = semi + 1;
                }
            }
            at = end + 1;
        }
        if (!g.commands.empty()) {
            ConsoleGate_Install();  // (with the render thread off too: its joins are then nothing)
            Log("console gate: [Debug] Commands: %zu commands, the first after %llu flushes%s", g.commands.size(),
                static_cast<unsigned long long>(g.commands.front().atFlush),
                g.installed ? "" : " (not run: the gate isn't installed)");
        }
    }
    ++g.flushes;
#endif
}

void* ConsoleGate_SetterBehind(void* entry) {
#ifdef _WIN64
    for (int c = 0; c < kClasses; ++c)
        for (int i = 0; i < kSetters; ++i)
            if (entry == kClassHooks[c][i] && g.orig[c][i]) return g.orig[c][i];
#endif
    return entry;
}

void ConsoleGate_Report(const char* when) {
#ifdef _WIN64
    if (!g.installed) return;
    Log("console gate [%s]: joined beside the flush at a cvar set %llu, at a command %llu, at the console's Update "
        "with queued commands %llu",
        when, g.setJoins, g.commandJoins, g.updateJoins);
#else
    (void)when;
#endif
}

}  // namespace optiperf
