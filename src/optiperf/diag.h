#pragma once
// Diagnostics for bug reports: logs the first few crash-type exceptions, ExitProcess calls and message boxes, each with
// the engine call chain recovered from the stack. Nothing is changed in how the game handles them.
//
// [Debug] Diagnostics = 1 (default) | 0      (env OPTIPERF_DIAGNOSTICS)

namespace optiperf {

void Diag_Init();

// "Module.dll+0x1234" for any code address (used in log lines).
const char* DescribeAddress(const void* address, char* buffer, unsigned bufferSize);

// Called once from the ExitProcess hook, before the process starts tearing down (the game does not always get as far
// as unloading DLLs, so statistics are written from here).
void Diag_SetExitCallback(void (*callback)());

// The game call chain, as in the crash lines, from `words` stack words (e.g. copied from another thread's stack).
void Diag_CallChain(const void* stack, unsigned words, char* buffer, unsigned bufferSize);
// The same with every module's return addresses (optiperf's and the system's too), the first `most` of them.
void Diag_FullCallChain(const void* stack, unsigned words, unsigned most, char* buffer, unsigned bufferSize);

// Name the engine gave a thread (through the debugger SetThreadName exception, seen since Diag_Init); false if none.
bool Diag_ThreadName(unsigned long threadId, char* buffer, unsigned bufferSize);

}  // namespace optiperf
