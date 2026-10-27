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

}  // namespace optiperf
