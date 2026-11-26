#pragma once
// The console's gate (FlushThread=overlap; research/docs/render-thread.md §11.2): what CrySystem's console calls of
// the renderer's without a vtable, its cvars' change callbacks and its commands' functions, and the renderer's own
// cvars, which the flush reads where they are kept. On the main thread beside a flush, a cvar set (the setters of
// CrySystem's cvar classes, ICVar slots 4-7) and a command (CXConsole::ExecuteString, slot 31) join the flush first;
// so does the console's Update while commands it runs later are waiting. x64.

namespace optiperf {

// From the render queue's install (overlap), CrySystem loaded: the slots patched in its classes' vtables.
void ConsoleGate_Install();
void ConsoleGate_Report(const char* when);
// At each outer flush's entry, on the main thread (any mode). [Debug] Commands (env OPTIPERF_DEBUG_COMMANDS), a test's
// console commands run after so many flushes, e.g. "300:e_time_of_day 6.3;e_time_of_day_speed 0.02|900:...": counted
// here (the gate installed at the first, if it isn't), run from the console's Update, as a command typed there.
void ConsoleGate_OnFlushEntry();
// What a cvar class's setter slot leads to past the gate: the entry the gate found in the slot when `entry` is one of
// its hooks, else `entry`. The cvar part patches two of the same classes' setters at the first flush, after the gate
// (with the render thread on, from its install), and calls on to the gate's hooks; it checks the setters it finds are
// CrySystem's through this.
void* ConsoleGate_SetterBehind(void* entry);

}  // namespace optiperf
