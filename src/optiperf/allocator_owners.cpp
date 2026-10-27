// The block an engine address is in, for the allocator's owner tags (allocator.cpp): mimalloc's segment and page of
// the address, and the block in it. This reads mimalloc's internal layout (v2.5.2 with the build's release settings),
// so allocator.cpp checks it on blocks of its own before it turns the tags on. Its own file: mimalloc's internal
// headers stay out of the rest of the plugin.
//
// mimalloc itself only ever looks up block starts (mi_free), and two of its shortcuts don't hold for addresses deep
// inside a block: a segment is found by aligning down to 32 MiB, but a huge segment spans several of those; and a
// page's slices point back to its first slice only for about the first 16 MiB of it.

#include <mimalloc.h>

#pragma warning(push, 0)
#include <mimalloc/internal.h>
#pragma warning(pop)

#include <cstddef>
#include <cstdint>

namespace optiperf {
namespace {

// whether a page holds q in one of its blocks
bool Holds(const mi_page_t* page, const uint8_t* q) {
    const size_t bsize = page->block_size;
    const uint8_t* start = page->page_start;
    return bsize >= 16 && start && page->reserved && q >= start && size_t(q - start) < size_t(page->reserved) * bsize;
}

}  // namespace

// The mimalloc block p points into (its start and size), or false. p must be in one of mimalloc's arenas
// (mi_is_in_heap_region); a part of an arena that no segment uses can't be read, and gives false.
bool Allocator_BlockOf(const void* p, char** block, size_t* size) {
    __try {
        const auto* q = static_cast<const uint8_t*>(p);
        // its segment: aligned down to 32 MiB, and further down while that is no segment's start (its cookie)
        uintptr_t s = (uintptr_t(q) - 1) & ~MI_SEGMENT_MASK;
        const mi_segment_t* seg = nullptr;
        for (int k = 0; k < 64 && s; ++k, s -= MI_SEGMENT_SIZE) {
            const auto* c = reinterpret_cast<const mi_segment_t*>(s);
            if (c->cookie == _mi_ptr_cookie(c)) {
                seg = c;
                break;
            }
        }
        if (!seg || uintptr_t(q) >= uintptr_t(seg) + seg->segment_slices * MI_SEGMENT_SLICE_SIZE) return false;
        // its page: a huge segment's only one; else the slice's back pointer, or further back the page head (a slice
        // with a count and no offset) whose blocks hold q
        const mi_page_t* page = nullptr;
        if (seg->kind == MI_SEGMENT_HUGE) {
            page = &seg->slices[seg->segment_info_slices];
        } else {
            size_t idx = size_t(q - reinterpret_cast<const uint8_t*>(seg)) >> MI_SEGMENT_SLICE_SHIFT;
            if (idx >= seg->slice_entries) idx = seg->slice_entries - 1;
            const mi_slice_t* slice = &seg->slices[idx];
            const auto* first = reinterpret_cast<const mi_slice_t*>(reinterpret_cast<const uint8_t*>(slice) -
                                                                    slice->slice_offset);
            if (first >= seg->slices && Holds(first, q)) page = first;
            for (ptrdiff_t i = ptrdiff_t(idx); !page && i >= 0; --i) {
                const mi_slice_t* head = &seg->slices[i];
                if (head->slice_offset == 0 && head->slice_count > 0 && Holds(head, q)) page = head;
            }
        }
        if (!page || !Holds(page, q)) return false;
        const size_t bsize = page->block_size;
        const uint8_t* start = page->page_start;
        *block = const_cast<char*>(reinterpret_cast<const char*>(start + size_t(q - start) / bsize * bsize));
        *size = bsize;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace optiperf
