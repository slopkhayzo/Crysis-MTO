// The ocean's height, transcribed from Cry3DEngine's machine code (see ocean_snapshot.h). Every expression keeps the
// code's operations in its order (x64 MSVC /fp:precise keeps them); the grid's cell comes from cvttss2si as in the
// code (through the intrinsic: casting a float outside int's range is undefined in C++).

#include "ocean_snapshot.h"

#include <xmmintrin.h>

#include <cstring>

#include "scan.h"

namespace optiperf::ocean {
namespace {

// --- the code this follows (x64 Cry3DEngine.dll, build 6156) --------------------------------------------------------
// GetOceanWaterLevel after its profiler (+0xF3AEF, 0x11F into it): the terrain's water level (+0x1E0) cached once a
// 3D-engine frame, then GetWave's height on top of it; 72 bytes
constexpr const char kLevelCode[] =
    "8B 05 ?? ?? ?? ?? 39 05 ?? ?? ?? ?? 74 22 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 16 F3 0F 10 81 E0 01 "
    "00 00 89 05 ?? ?? ?? ?? F3 0F 11 05 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 39 7C 24 30 0F 28 F0 "
    "F3 0F 58 35 ?? ?? ?? ?? ";
constexpr int kLevelStore = 0x2C, kLevelWave = 0x34, kLevelAdd = 0x44;  // the cached level (stored, added); GetWave
constexpr size_t kLevelSearch = 0x180;  // how far into GetOceanWaterLevel that is looked for

// COcean::GetWave (+0xC3F40) up to its analytic branch: no renderer, no timer or water shaders off: 0; vertex textures,
// the FFT cvar and water shaders of at least medium quality: the render element's grid at the position, blended
// bilinearly; 1094 bytes
constexpr const char kWaveCode[] =
    "40 53 56 48 81 EC 18 01 00 00 48 8B 1D ?? ?? ?? ?? 48 8B F1 48 85 DB 75 0D 0F 57 C0 48 81 C4 18 "
    "01 00 00 5E 5B C3 48 8B 03 BA 07 00 00 00 48 8B CB 48 89 BC 24 10 01 00 00 FF 90 10 03 00 00 48 "
    "83 3D ?? ?? ?? ?? 00 8B F8 0F 84 ?? ?? ?? ?? 85 C0 0F 84 ?? ?? ?? ?? 48 8B 13 4C 89 A4 24 08 01 "
    "00 00 4C 8B 25 ?? ?? ?? ?? 0F 29 B4 24 F0 00 00 00 0F 29 BC 24 E0 00 00 00 44 0F 29 84 24 D0 00 "
    "00 00 48 8B CB 44 0F 29 8C 24 C0 00 00 00 44 0F 29 94 24 B0 00 00 00 44 0F 29 9C 24 A0 00 00 00 "
    "44 0F 29 A4 24 90 00 00 00 44 0F 29 AC 24 80 00 00 00 44 0F 29 74 24 70 44 0F 29 7C 24 60 FF 52 "
    "38 0F BA E0 1F 0F 83 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 83 B8 D8 00 00 00 00 0F 84 ?? ?? ?? ?? 83 "
    "FF 02 0F 8C ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 0F 57 F6 4C 89 AC 24 00 01 00 00 48 85 C9 0F 84 BA "
    "02 00 00 48 8B 01 FF 90 C0 00 00 00 48 85 C0 4C 8B E8 0F 84 B7 02 00 00 F3 0F 10 36 F3 0F 10 15 "
    "?? ?? ?? ?? F3 44 0F 10 46 04 48 89 AC 24 30 01 00 00 F3 0F 59 F2 F3 44 0F 59 C2 F3 41 0F 10 9C "
    "24 64 01 00 00 F3 0F 10 0D ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ?? F3 0F 59 F3 F3 44 0F 59 C3 F3 0F "
    "59 F1 F3 44 0F 59 C1 F3 0F 59 F0 F3 44 0F 59 C0 F3 0F 2C FE F3 41 0F 2C D8 0F 28 C6 83 E7 3F 83 "
    "E3 3F 8D 77 01 8D 6B 01 83 E6 3F 83 E5 3F E8 ?? ?? ?? ?? F3 0F 5C F0 41 0F 28 C0 E8 ?? ?? ?? ?? "
    "F3 0F 10 0D ?? ?? ?? ?? 4C 8D 44 24 50 4C 63 CF F3 44 0F 5C C0 48 63 D3 48 C1 E2 06 0F 28 E1 F3 "
    "41 0F 5C E0 F3 0F 5C CE 0F 28 F9 F3 41 0F 59 C8 F3 44 0F 59 C6 49 8D 0C 11 48 03 C9 F3 0F 59 FC "
    "F3 0F 59 E6 41 8B 44 CD 00 41 89 00 41 8B 44 CD 04 41 89 40 04 41 8B 44 CD 08 F3 0F 11 BC 24 38 "
    "01 00 00 41 89 40 08 41 8B 44 CD 0C 41 89 40 0C 4C 63 C6 49 8D 0C 10 48 8D 54 24 40 48 03 C9 41 "
    "8B 44 CD 00 89 02 41 8B 44 CD 04 89 42 04 41 8B 44 CD 08 89 42 08 41 8B 44 CD 0C 89 42 0C 48 63 "
    "D5 48 C1 E2 06 4A 8D 0C 0A 4C 8D 4C 24 30 48 03 C9 41 8B 44 CD 00 41 89 01 41 8B 44 CD 04 41 89 "
    "41 04 41 8B 44 CD 08 41 89 41 08 41 8B 44 CD 0C 4A 8D 0C 02 48 03 C9 48 8D 54 24 20 41 89 41 0C "
    "41 8B 44 CD 00 F3 0F 10 6C 24 54 F3 0F 10 74 24 58 48 8B AC 24 30 01 00 00 89 02 41 8B 44 CD 04 "
    "F3 0F 10 44 24 40 F3 44 0F 10 4C 24 34 F3 44 0F 10 54 24 38 F3 0F 59 F7 F3 0F 59 EF 89 42 04 41 "
    "8B 44 CD 08 F3 0F 59 C4 F3 44 0F 59 C9 F3 44 0F 59 D1 F3 44 0F 10 5C 24 3C F3 0F 10 54 24 48 F3 "
    "0F 10 5C 24 4C 89 42 08 41 8B 44 CD 0C F3 0F 59 D4 F3 0F 59 DC F3 44 0F 59 D9 89 42 0C F3 0F 58 "
    "F2 F3 44 0F 10 64 24 20 F3 44 0F 10 6C 24 24 F3 44 0F 10 74 24 28 F3 45 0F 59 E0 F3 45 0F 59 E8 "
    "F3 45 0F 59 F0 F3 44 0F 10 7C 24 2C F3 41 0F 58 F2 F3 45 0F 59 F8 F3 44 0F 10 44 24 30 F3 41 0F "
    "58 F6 F3 44 0F 59 C1 F3 0F 10 4C 24 44 F3 0F 11 74 24 58 F3 0F 59 CC F3 0F 10 64 24 50 F3 0F 58 "
    "E9 F3 0F 59 E7 F3 0F 10 7C 24 5C F3 0F 58 E0 F3 41 0F 58 E9 F3 0F 59 BC 24 38 01 00 00 F3 0F 58 "
    "FB F3 41 0F 58 E0 F3 41 0F 58 ED F3 0F 11 6C 24 54 F3 41 0F 58 FB F3 41 0F 58 E4 F3 0F 11 64 24 "
    "50 F3 41 0F 58 FF F3 0F 11 7C 24 5C 0F 28 44 24 50 66 0F 7F 44 24 50 F3 0F 10 74 24 58 F3 0F 59 "
    "35 ?? ?? ?? ?? F3 41 0F 59 B4 24 68 01 00 00 4C 8B AC 24 00 01 00 00 0F 28 C6 44 0F 28 AC 24 80 "
    "00 00 00 44 0F 28 A4 24 90 00 00 00 44 0F 28 9C 24 A0 00 00 00 4C 8B A4 24 08 01 00 00 48 8B BC "
    "24 10 01 00 00 44 0F 28 94 24 B0 00 00 00 44 0F 28 8C 24 C0 00 00 00 44 0F 28 84 24 D0 00 00 00 "
    "0F 28 BC 24 E0 00 00 00 0F 28 B4 24 F0 00 00 00 44 0F 28 74 24 70 44 0F 28 7C 24 60 48 81 C4 18 "
    "01 00 00 5E 5B C3 ";
constexpr size_t kWaveBytes = 1094;  // (its analytic branch follows)
// its fields (rip-relative): the renderer, the timer (an imm8 follows the field), the 3D engine, the cvars, the render
// element; its exits (rel32): no waves, the analytic branch; its two calls to floorf
constexpr int kWaveRenderer = 0xD, kWaveTimer = 0x42, kWaveEngine = 0x65, kWaveCvars = 0xCE, kWaveElement = 0xEB;
constexpr int kWaveNone[] = {0x4B, 0x53}, kWaveAnalytic[] = {0xC7, 0xDB, 0xE4}, kWaveFloor[] = {0x18F, 0x19C};
// its constants, by rip-relative field: the grid's scale (0.0125, 1.25, 64), one, the height's scale (0.06)
struct Constant {
    int field;
    uint32_t bits;
};
constexpr Constant kWaveConstants[] = {
    {0x120, 0x3C4CCCCDu}, {0x149, 0x3FA00000u}, {0x151, 0x42800000u}, {0x1A4, 0x3F800000u}, {0x3C1, 0x3D75C28Fu}};
// where its no-waves exits go: 0
constexpr const char kNoneCode[] = "0F 57 C0 48 8B BC 24 10 01 00 00 48 81 C4 18 01 00 00 5E 5B C3";
constexpr const char kThunk[] = "FF 25 ?? ?? ?? ??";  // jmp qword ptr [rip + disp32]: an import

constexpr int kWaterShaders = 7;                        // eST_Water
constexpr uint32_t kVertexTextures = 0x80000000u;       // RFT_HW_VERTEXTEXTURES

// what a rip-relative or rel32 field points at
char* Target4(char* field) { return field + 4 + int32_t(ReadU32(field)); }

bool Code(char* base, char* at, const char* text) {
    const Pattern p = ParsePattern(text);
    return at && IsInCode(base, at, p.size()) && MatchesAt(at, p);
}

template <typename Fn>
Fn Vf(void* self, int offset) {
    return reinterpret_cast<Fn>((*static_cast<void* const* const*>(self))[offset / 8]);
}

inline float Float(const char* p) {
    float f;
    std::memcpy(&f, p, sizeof f);
    return f;
}

}  // namespace

bool Locate(char* base, char* fn, Target* out, const char** why) {
    if (!fn || !IsInCode(base, fn, kLevelSearch)) return *why = "GetOceanWaterLevel not found", false;
    const auto hits = FindInRange(fn, kLevelSearch, ParsePattern(kLevelCode), 2);
    if (hits.size() != 1) return *why = "GetOceanWaterLevel is not the expected code", false;
    char* level = Target4(hits[0] + kLevelStore);
    if (Target4(hits[0] + kLevelAdd) != level || !IsInImage(base, level, 4))
        return *why = "GetOceanWaterLevel's cached water level is not as expected", false;
    char* wave = Target4(hits[0] + kLevelWave);
    if (!Code(base, wave, kWaveCode)) return *why = "the ocean's waves (COcean::GetWave) are not the expected code", false;
    char* renderer = Target4(wave + kWaveRenderer);
    char* timer = Target4(wave + kWaveTimer) + 1;
    char* engine = Target4(wave + kWaveEngine);
    char* cvars = Target4(wave + kWaveCvars);
    char* element = Target4(wave + kWaveElement);
    if (!IsInImage(base, renderer, 8) || !IsInImage(base, timer, 8) || !IsInImage(base, engine, 8) ||
        !IsInImage(base, cvars, 8) || !IsInImage(base, element, 8))
        return *why = "the ocean's waves read data outside Cry3DEngine.dll", false;
    // its exits: both no-waves ones to one place, which returns 0; the analytic branch right after the pattern
    char* none = Target4(wave + kWaveNone[0]);
    bool exits = Target4(wave + kWaveNone[1]) == none && Code(base, none, kNoneCode);
    for (int at : kWaveAnalytic) exits = exits && Target4(wave + at) == wave + kWaveBytes;
    if (!exits) return *why = "the ocean's waves branch elsewhere than expected", false;
    for (const Constant& c : kWaveConstants) {
        const char* k = Target4(wave + c.field);
        if (!IsInImage(base, k, 4) || ReadU32(k) != c.bits)
            return *why = "the ocean's waves use other constants than expected", false;
    }
    char* floor = Target4(wave + kWaveFloor[0]);
    if (Target4(wave + kWaveFloor[1]) != floor || !Code(base, floor, kThunk))
        return *why = "the ocean's waves' floorf is not the expected import", false;
    Target t;
    t.renderer = reinterpret_cast<void* const*>(renderer);
    t.timer = reinterpret_cast<void* const*>(timer);
    t.engine = reinterpret_cast<char* const*>(engine);
    t.cvars = reinterpret_cast<char* const*>(cvars);
    t.element = reinterpret_cast<void* const*>(element);
    t.level = reinterpret_cast<const float*>(level);
    t.wave = wave;
    t.floor = reinterpret_cast<FloorFn>(floor);
    *out = t;
    return true;
}

// GetWave's tests in its order: the renderer; the water shaders' quality, and the timer; then vertex textures, the FFT
// cvar and a quality of at least 2 for the FFT branch
Inputs Capture(const Target& t) {
    Inputs in;
    void* r = *t.renderer;
    if (!r) return in;
    const int quality = Vf<int (*)(void*, int)>(r, kVfShaderQuality)(r, kWaterShaders);
    if (!*t.timer || quality == 0) return in;
    const uint32_t features = Vf<uint32_t (*)(void*)>(r, kVfFeatures)(r);
    in.branch = Branch::Analytic;
    const char* cvars = *t.cvars;
    const char* engine = *t.engine;
    if (!(features & kVertexTextures) || !cvars || !engine) return in;
    int32_t fft;
    std::memcpy(&fft, cvars + kCvarFft, sizeof fft);
    if (fft == 0 || quality < 2) return in;
    in.branch = Branch::Fft;
    in.element = *t.element;
    in.scale = Float(engine + kEngineScale);
    in.size = Float(engine + kEngineSize);
    in.level = *t.level;
    return in;
}

// Cry3DEngine +0xC4058..+0xC4317: the position on the grid, its cell and where in it, the four corners' heights
// weighted by the opposite areas, then the waves' size
float FftWave(const float* grid, const float* pos, float scale, float size, FloorFn floor) {
    const float gx = pos[0] * 0.0125f * scale * 1.25f * 64.0f;
    const float gy = pos[1] * 0.0125f * scale * 1.25f * 64.0f;
    const int x0 = _mm_cvtt_ss2si(_mm_set_ss(gx)) & (kGrid - 1), y0 = _mm_cvtt_ss2si(_mm_set_ss(gy)) & (kGrid - 1);
    const int x1 = (x0 + 1) & (kGrid - 1), y1 = (y0 + 1) & (kGrid - 1);
    const float fx = gx - floor(gx);
    const float fy = gy - floor(gy);
    const float w00 = (1.0f - fx) * (1.0f - fy), w10 = (1.0f - fy) * fx;
    const float w01 = (1.0f - fx) * fy, w11 = fy * fx;
    const float h00 = grid[(x0 + y0 * kGrid) * 4 + 2], h10 = grid[(x1 + y0 * kGrid) * 4 + 2];
    const float h01 = grid[(x0 + y1 * kGrid) * 4 + 2], h11 = grid[(x1 + y1 * kGrid) * 4 + 2];
    const float h = h00 * w00 + h10 * w10 + h01 * w01 + h11 * w11;
    return h * 0.06f * size;
}

// GetWave's FFT branch from its render element (without one: 0 times the size; without a grid: 0), then the level
float Height(const Inputs& in, const float* pos, FloorFn floor) {
    float wave = 0.0f;
    if (!in.element) {
        wave = wave * 0.06f * in.size;
    } else {
        const auto* grid = Vf<const float* (*)(void*)>(in.element, kVfDisplaceGrid)(in.element);
        if (grid) wave = FftWave(grid, pos, in.scale, in.size, floor);
    }
    return wave + in.level;
}

}  // namespace optiperf::ocean
