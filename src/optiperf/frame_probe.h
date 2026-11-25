#pragma once
// A brightness probe, to find a strobe without a video (a diagnostic; x64 DX10, any mode):
// [Debug] FrameProbe = off (default) | on   (env OPTIPERF_FRAME_PROBE)
//
// At the first flush's entry the swap chain is found among the renderer object's pointers (an object of dxgi.dll's that
// answers QueryInterface for IDXGISwapChain) and its Present is hooked in its vtable. At each present a grid of small
// patches of the back buffer (kCols x kRows patches of kPatch pixels, at the cells' centres) is copied into one of
// kRing staging textures; the copy made kRing presents earlier is read back then, and its patches' mean luma (Rec. 709
// weights on the stored values) written to optiperf_probe.bin next to the log, with the tags the frame was handed off
// with (FrameProbe_Tag). The file: a header (magic "OPRB", version, columns, rows, patch, width, height, format, ring),
// then per frame: uint32 sequence, uint32 tags, uint64 tsc, uint16 luma x 64 per patch (row by row).
//
// Tags (set at the hand-off, read at the present that follows its flush): bits 0-7 the sun's maps (by their place in
// its frustum array) the flush renders, as predicted (the light part); bits 8-11 how the frame's main traversal ran (the
// traversal record: 1 answered beside the flush, 2 made with no flush beside it, 3 made after the flush beside it
// ended), bits 16-23 the share of it made before its join, in 1/255ths.

#include <cstdint>

namespace optiperf {

constexpr uint32_t kProbeTagMaps = 0xFFu, kProbeTagTraversal = 0xF00u, kProbeTagJoin = 0xFF0000u;

void FrameProbe_OnFlush(char* renderer);              // at an outer flush's entry, on the main thread: installs once
void FrameProbe_Tag(uint32_t mask, uint32_t value);  // the bits of `mask` set to `value`'s
bool FrameProbe_On();
void FrameProbe_Report(const char* when);

}  // namespace optiperf
