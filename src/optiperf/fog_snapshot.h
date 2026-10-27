#pragma once
// TraceFogVolumes from a snapshot of the frame's fog volumes (the environment snapshot, env_snapshot.cpp). The 3D
// engine's code (Cry3DEngine +0x9F630, CFogVolumeRenderNode::TraceFogVolumes, and the two contributions it sums:
// +0x9DD80 for an ellipsoid, +0x9E3C0 for a box) is transcribed from its machine code with the same single-precision
// operations in the same order, and reads copies of the visible fog volumes, the camera box and the camera position
// instead of the 3D engine's live state. It calls the same CRT functions (MSVCR80's sqrtf and expf, through the 3D
// engine's own import thunks) and the 3D engine's ray-box test (+0x7DA0), which reads only its arguments. All of that
// code is checked by pattern first. No engine state here: tests/shadow_prepass_test.cpp compares the contributions
// with the game's own functions.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace optiperf::fog {

using SqrtFn = float (*)(float);
using ExpFn = float (*)(float);
// ray: its start (x y z), then its direction; box: min x y z, max x y z. Writes the point where the ray enters the
// box to *hit; returns 0 if it misses the box, 1 if it hits it, 2 if it starts inside.
using RayBoxFn = uint8_t (*)(const float* ray, const float* box, float* hit);

struct Math {
    SqrtFn sqrt = nullptr;
    ExpFn exp = nullptr;
    RayBoxFn rayBox = nullptr;
};

constexpr size_t kNodeBytes = 0x1D0;  // what the loop and the contributions read of a CFogVolumeRenderNode
constexpr size_t kEntryBytes = 16;    // the visible fog volumes' entries: the node, then 8 bytes the loop doesn't read

// One fog volume's contribution at pos, seen from the camera at cam: its colour (rgb) and how much light gets through
// (a), as the game's +0x9DD80 (an ellipsoid) and +0x9E3C0 (a box) compute it.
void Ellipsoid(const uint8_t* node, const float* cam, const float* pos, float* out, const Math& m);
void Box(const uint8_t* node, const float* cam, const float* pos, float* out, const Math& m);

// The frame's fog volumes, as TraceFogVolumes reads them. The list and the camera box are the frame's (the 3D engine
// sets them in its outer traversal only); the camera position is the 3D engine's camera, which a reflection's
// traversal changes for its own flush.
struct Snapshot {
    float box[6] = {};  // the camera box: min x, y, z, then max x, y, z
    float cam[3] = {};  // the 3D engine's camera position when it was taken
    size_t count = 0;   // visible fog volumes (with both fog cvars set: else there is no snapshot)
    std::vector<uint8_t> nodes;  // kNodeBytes each, in the list's order
};
// TraceFogVolumes(pos) over a snapshot (taken while both fog cvars are set), with the 3D engine's camera at cam: the
// fog volumes whose box the ray from the camera box's centre towards pos hits
void Trace(const Snapshot& s, const float* cam, const float* pos, float* out, const Math& m);

// What the snapshot needs from a mapped Cry3DEngine.dll, found from TraceFogVolumes (`trace`, the static function).
struct Target {
    char* const* cvars = nullptr;  // the 3D engine's cvars: fog (+0x30) and fog volumes (+0x34) must both be set
    char* const* begin = nullptr;  // the visible fog volumes: a vector of 16-byte entries, the node first
    char* const* end = nullptr;
    const float* box = nullptr;    // the camera box (6 floats)
    const float* cam[3] = {};      // the camera position (3 floats, 16 bytes apart: a matrix's translation)
    char* ellipsoid = nullptr;     // the game's own contributions (the unit test calls them)
    char* boxFog = nullptr;
    Math math;
};
constexpr int kCvarFog = 0x30, kCvarFogVolumes = 0x34;
// Checks TraceFogVolumes' loop, the three functions it calls and the CRT thunks they use; on failure returns false
// and sets *why.
bool Locate(char* e3dBase, char* trace, Target* out, const char** why);

}  // namespace optiperf::fog
