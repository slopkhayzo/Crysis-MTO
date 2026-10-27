#pragma once
// The ocean's height at a position (I3DEngine::GetOceanWaterLevel, and through it GetWaterLevel's) for the environment
// snapshot (env_snapshot.cpp). With the FFT ocean, the 3D engine's code (Cry3DEngine +0xF39D0, and the waves it adds:
// +0xC3F40, COcean::GetWave) samples the displacement grid of the ocean's render element, a renderer object
// (CREWaterOcean: its vtable's +0xC0), which the renderer steps inside the flush when it draws the ocean
// (CREWaterOcean::mfDraw, with this frame's time): calls after that see this frame's grid, calls before it the last
// one's, and a capture before the flush is a frame behind. So the height is computed when it is asked for: GetWave's
// FFT branch transcribed from its machine code with the same single-precision operations in the same order (the
// grid's four heights around the position, blended bilinearly), over the grid as it is then and the 3D engine's
// parameters as the capture took them (the grid's scale over the world, the waves' size, the terrain's water level).
// Which branch GetWave takes is worked out at the capture, as GetWave does; with its analytic waves, which move with the
// frame time only, the capture's height holds for the whole flush. All of that code is checked by pattern first. No
// engine state here: tests/shadow_prepass_test.cpp compares the transcription with the game's own GetWave.

#include <cstdint>

namespace optiperf::ocean {

using FloorFn = float (*)(float);

constexpr int kGrid = 64;                 // the displacement grid: 64 x 64 Vec4, the height in .z
constexpr int kVfDisplaceGrid = 0xC0;     // CREWaterOcean: the grid (null while it has none)
constexpr int kVfShaderQuality = 0x310;   // IRenderer::EF_GetShaderQuality(eST_Water = 7)
constexpr int kVfFeatures = 0x38;         // IRenderer::GetFeatures: bit 31, vertex textures
constexpr int kEngineScale = 0x164, kEngineSize = 0x168;  // the 3D engine's ocean: the grid's scale, the waves' size
constexpr int kCvarFft = 0xD8;            // the 3D engine's cvars: the FFT ocean

// What GetWave reads, found from GetOceanWaterLevel in a mapped Cry3DEngine.dll.
struct Target {
    void* const* renderer = nullptr;  // the 3D engine's IRenderer
    void* const* timer = nullptr;     // GetWave makes no waves without it
    char* const* engine = nullptr;    // the 3D engine (kEngineScale, kEngineSize)
    char* const* cvars = nullptr;     // its cvars (kCvarFft)
    void* const* element = nullptr;   // the ocean's render element (CREWaterOcean)
    const float* level = nullptr;     // GetOceanWaterLevel's terrain water level (updated once a frame)
    char* wave = nullptr;             // COcean::GetWave (the unit test calls it)
    FloorFn floor = nullptr;          // MSVCR80's floorf, through the 3D engine's import thunk
};
// Checks GetOceanWaterLevel (`fn`) after its profiler, GetWave up to its analytic branch and the floorf thunk; on
// failure returns false and sets *why.
bool Locate(char* e3dBase, char* fn, Target* out, const char** why);

// The branch GetWave takes this frame, and the FFT branch's inputs.
enum class Branch : uint8_t { None, Analytic, Fft };
struct Inputs {
    Branch branch = Branch::None;  // None: no waves (no renderer, no timer, or water shaders off)
    void* element = nullptr;
    float scale = 0.0f, size = 0.0f, level = 0.0f;
};
// Before the flush, right after a GetOceanWaterLevel call (which updates the cached level): the branch, as GetWave
// decides it (it calls the renderer's EF_GetShaderQuality and GetFeatures, as GetWave does), and its inputs.
Inputs Capture(const Target& t);

// GetWave's FFT branch at pos over a grid of 64 x 64 Vec4.
float FftWave(const float* grid, const float* pos, float scale, float size, FloorFn floor);
// GetOceanWaterLevel(pos) on the FFT branch, over the render element's grid as it is now.
float Height(const Inputs& in, const float* pos, FloorFn floor);

}  // namespace optiperf::ocean
