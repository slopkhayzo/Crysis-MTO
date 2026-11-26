#pragma once
// A brightness probe, to find a strobe without a video (a diagnostic; x64 DX10, any mode):
// [Debug] FrameProbe = off (default) | on   (env OPTIPERF_FRAME_PROBE)
//
// At the first flush's entry the swap chain is found among the renderer object's pointers (an object of dxgi.dll's that
// answers QueryInterface for IDXGISwapChain) and its Present is hooked in its vtable. At each present a grid of small
// patches of the back buffer (kCols x kRows patches of kPatch pixels, at the cells' centres) is copied into one of
// kRing staging textures; the copy made kRing presents earlier is read back then, and its patches' mean luma (Rec. 709
// weights on the stored values) written to optiperf_probe.bin next to the log, with the tags the frame was handed off
// with (FrameProbe_Tag). The file: a header (magic "OPRB", version 4, columns, rows, patch, width, height, format,
// ring), then per frame: uint32 sequence, uint32 tags, uint64 tsc, uint32 extra[11], uint16 luma x 64 per patch (row by
// row).
//
// Tags (set at the hand-off, read at the present that follows its flush): bits 0-7 the sun's maps (by their place in
// its frustum array) the flush renders, as predicted (the light part); bits 8-11 how the frame's main traversal ran (the
// traversal record: 1 answered beside the flush, 2 made with no flush beside it, 3 made after the flush beside it
// ended), bits 16-23 the share of it made before its join, in 1/255ths. With [Debug] LightsDrawCheck (the light part,
// set during the flush): bits 24-31 the objects its per-object shadow passes drew with a frustum list that isn't one of
// the hand-off's copies (up to 255), bits 12-15 those of them whose list was empty then (up to 15).
//
// Extra 0-4 (set later, FrameProbe_SetLatest, on the latest present's frame, by the next frame's traversal when it
// joins this frame's flush; 0xFFFFFFFF if it didn't run beside it): 0 the render objects it had handed out beside the
// flush, 1 the calls it had recorded; in microseconds from the flush's start: 2 the flush's end, 3 that traversal's
// start, 4 the worker done (the frame's end made too). Extra 5-10 (FrameProbe_SetNext, for the present that follows):
// at the hand-off, 5 the render items of the sun's maps' shadow pre-pass stashes, 6 their casters; in the flush, the
// far-tree sprites' draws (the reflection part's replay of DrawObjSprites, reset at the hand-off): 7 the z-pass draw's
// time and 8 the general draw's, in microseconds from the flush's start; 9 and 10 their state: bit 31 made, bits 0-15
// the sprite quads then, bit 16 the renderer's +0xAF8 bit 3 (DrawObjSprites then skips the z-pass draw), bit 17 not
// at recursion level 1 (it draws nothing).

#include <cstdint>

namespace optiperf {

constexpr uint32_t kProbeTagMaps = 0xFFu, kProbeTagTraversal = 0xF00u, kProbeTagJoin = 0xFF0000u;
constexpr uint32_t kProbeTagListsMissed = 0xFF000000u, kProbeTagListsEmpty = 0xF000u;

void FrameProbe_OnFlush(char* renderer);              // at an outer flush's entry, on the main thread: installs once
void FrameProbe_Tag(uint32_t mask, uint32_t value);  // the bits of `mask` set to `value`'s
void FrameProbe_SetLatest(int k, uint32_t value);    // extra[k] of the latest present's frame (k < 5)
void FrameProbe_SetNext(int k, uint32_t value);      // extra[k] of the next present's frame (k = 5..10)
bool FrameProbe_On();
void FrameProbe_Report(const char* when);

}  // namespace optiperf
