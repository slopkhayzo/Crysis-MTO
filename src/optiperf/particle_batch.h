#pragma once

#include <cstddef>
#include <cstdint>

namespace optiperf {

// Where the particle batch merge lives in a loaded renderer, as read from the code itself.
struct ParticleBatchTarget {
    char* merge = nullptr;         // the patched function
    uint32_t listOffset = 0;       // renderer member: PodArray<CRenderObject*> {elements, count, capacity}
    uint32_t freeListOffset = 0;   // renderer member: PodArray<CRenderObject*> of reusable temp objects
    char* podAdd = nullptr;        // PodArray<T*>::Add (thiscall)
    char* colorEq = nullptr;       // ColorF compare (thiscall)
};

// Finds and validates the code in a mapped renderer DLL. On failure returns false and sets *why.
bool ParticleBatch_Locate(char* moduleBase, ParticleBatchTarget* out, const char** why);

void ParticleBatch_OnModuleLoaded(const wchar_t* baseName, char* base, size_t size);
void ParticleBatch_Report(const char* when);
// The batch list was changed behind the merge (traversal_record.cpp puts its entries back after a verify replay): the
// index is rebuilt from it at the next call.
void ParticleBatch_ResetIndex();
// The merge as located when the fix was installed (its code is patched since, so ParticleBatch_Locate no longer
// matches it); false when the fix isn't installed.
bool ParticleBatch_Installed(ParticleBatchTarget* out);

// Tests only: forget the installed hook so another renderer module can be patched in the same process.
void ParticleBatch_ResetForTest();

}  // namespace optiperf
