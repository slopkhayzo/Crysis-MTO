#pragma once

#include <cstddef>

namespace optiperf {

void Allocator_OnModuleLoaded(const wchar_t* baseName, char* base, size_t size);
void Allocator_Report(const char* when);

// The owner tags ([Allocator] Owners=on, x64; the pointer census): whether they are on, and for an address inside a
// live engine block the code that allocated it (CryMalloc's caller, or CryRealloc's for a block it moved), the block's
// user start and its usable size. False for an address outside mimalloc's blocks, or with the tags off. A freed
// block's "code" is mimalloc's free-list link (a heap address), which the caller can tell from code.
bool Allocator_TracksOwners();
bool Allocator_OwnerOf(const void* p, const void** site, const char** start, size_t* size);
// (allocator_owners.cpp) the mimalloc block p points into, its start and size
bool Allocator_BlockOf(const void* p, char** block, size_t* size);

// Pins (the render thread, FlushThread=overlap: research/docs/render-thread.md §11). From a flush's hand-off to the
// join, the parts' steps after the flush will write into some of the 3D engine's objects (the lights' write-back, the
// particles' counters, the materials' notes), which the main thread, beside the flush, may free meanwhile. A pinned
// block's free is deferred to the end of the pins and the block marked dying, which the steps skip (its object is
// gone). A realloc of a pinned block calls the join callback first on the main thread (vanilla's order: the steps'
// writes, then the copy); on another thread it allocates, copies and defers the old block. Off, each costs a free
// and a realloc one flag test.
void Allocator_EnablePins(void (*join)());  // once, x64: pins can be set; join: at a main-thread realloc of a pinned block
void Allocator_BeginPins();                  // at a hand-off, before the parts pin
bool Allocator_Pin(const void* p);           // the block p points into; false if it isn't the allocator's
bool Allocator_Dying(const void* p);         // the block p points into was freed while pinned
void Allocator_EndPins();                    // at the join, after the steps: pins cleared, deferred frees made
void Allocator_PinReport(const char* when);

// Tests only: the replacement functions and their setup (with or without owner tags), without hooking the game.
bool Allocator_TestSetup(size_t lowMemoryMb, bool owners = false);
void* Allocator_TestMalloc(size_t size, size_t* allocated);
size_t Allocator_TestFree(void* p);
void* Allocator_TestRealloc(void* p, size_t size, size_t* allocated);

}  // namespace optiperf
