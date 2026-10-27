#pragma once
// The renderer's gate (the render thread's groundwork, research/docs/render-thread.md §11): every slot of every vtable
// that MSVC RTTI describes in the renderer goes through a thunk of its own. A call on the gated thread (the main thread
// while a flush runs beside it, FlushThread=overlap) first calls back (onGated), which waits for the flush to end; on
// any other thread, and on the main thread otherwise, the thunk is a compare of the thread id and a jump. x64 only.

#include <vector>

#include "scan.h"

namespace optiperf {

// Puts the thunks into the given vtables (the renderer's: FindRttiVtables). onGated(index) runs on the gated thread,
// with the call's arguments saved, before the call goes on to its function. Once per process.
bool RenderGate_Install(const std::vector<RttiVtable>& vtables, void (*onGated)(unsigned index));
void RenderGate_Set(unsigned long threadId);  // the gated thread; 0: none
// The function behind one of the gate's thunks, else the entry itself (CallTrace_Original sees through them too).
void* RenderGate_Original(void* entry);
// The thunk of slot `index` goes on to fn instead of its function, which it returns (null if there is none): a call
// through the vtable reaches fn, while the function's code stays as it is (other parts read it).
void* RenderGate_Redirect(unsigned index, void* fn);
// What the thunk of slot `index` goes on to now: its function, or what a redirect put in its place (null for none).
void* RenderGate_Target(unsigned index);
// One more slot through a thunk of its own, of a vtable outside those installed (another module's, loaded later; the
// 3D engine's RenderWorld, the barrier): a few thunks are kept spare at the install. Returns its index (the same if
// it is already gated), or -1 (not installed, or no spare left).
int RenderGate_AddSlot(char** vtable, unsigned slot, const char* mangled);
// The thunk index of a gated vtable's slot, or -1.
int RenderGate_IndexOf(char** vtable, unsigned slot);
unsigned RenderGate_Slots();
unsigned RenderGate_Vtables();
// "class slot n" of a thunk index, for the reports.
void RenderGate_Describe(unsigned index, char* buffer, unsigned bufferSize);

}  // namespace optiperf
