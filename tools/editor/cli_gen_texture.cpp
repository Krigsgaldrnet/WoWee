#include "cli_gen_texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

// stb_image_write impl lives in texture_exporter.cpp;
// we just need the declaration of stbi_write_png.
#include "stb_image_write.h"

namespace wowee {
namespace editor {
namespace cli {

namespace {

// Shared hex-color parser used by every texture generator.
// Accepts "RRGGBB", "rgb", or those forms with a leading '#'.
// Returns false on malformed input (caller should error out).
bool parseHex(std::string hex, uint8_t& r, uint8_t& g, uint8_t& b) {
    std::transform(hex.begin(), hex.end(), hex.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
    auto fromHexC = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        return -1;
    };
    int v[6];
    if (hex.size() == 6) {
        for (int k = 0; k < 6; ++k) {
            v[k] = fromHexC(hex[k]);
            if (v[k] < 0) return false;
        }
        r = static_cast<uint8_t>((v[0] << 4) | v[1]);
        g = static_cast<uint8_t>((v[2] << 4) | v[3]);
        b = static_cast<uint8_t>((v[4] << 4) | v[5]);
        return true;
    }
    if (hex.size() == 3) {
        for (int k = 0; k < 3; ++k) {
            v[k] = fromHexC(hex[k]);
            if (v[k] < 0) return false;
        }
        r = static_cast<uint8_t>((v[0] << 4) | v[0]);
        g = static_cast<uint8_t>((v[1] << 4) | v[1]);
        b = static_cast<uint8_t>((v[2] << 4) | v[2]);
        return true;
    }
    return false;
}

int handleCobble(int& i, int argc, char** argv) {
    // Cobblestone street pattern. Each pixel finds its
    // nearest "stone center" in a perturbed grid (Worley-
    // style cellular noise) and uses the distance to that
    // center to draw the stone face vs. mortar gaps. Stones
    // get small per-stone tint variation so the surface
    // doesn't read as flat.
    std::string outPath = argv[++i];
    std::string stoneHex = argv[++i];
    std::string mortarHex = argv[++i];
    int stonePx = 24;
    uint32_t seed = 1;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { stonePx = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        stonePx < 8 || stonePx > 512) {
        std::fprintf(stderr,
            "gen-texture-cobble: invalid dims (W/H 1..8192, stonePx 8..512)\n");
        return 1;
    }
    uint8_t sr, sg, sb, mr, mg, mb;
    if (!parseHex(stoneHex, sr, sg, sb)) {
        std::fprintf(stderr,
            "gen-texture-cobble: '%s' is not a valid hex color\n",
            stoneHex.c_str());
        return 1;
    }
    if (!parseHex(mortarHex, mr, mg, mb)) {
        std::fprintf(stderr,
            "gen-texture-cobble: '%s' is not a valid hex color\n",
            mortarHex.c_str());
        return 1;
    }
    // Seeded hash → stone center jitter + per-stone tint.
    // Hash takes (cellX, cellY, seed) and returns 4 floats
    // in [0,1): two for offset, two for tint variation.
    auto hash01 = [seed](int cx, int cy, int comp) -> float {
        uint32_t h = static_cast<uint32_t>(cx) * 374761393u +
                     static_cast<uint32_t>(cy) * 668265263u +
                     seed * 2147483647u +
                     static_cast<uint32_t>(comp) * 16777619u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h = h ^ (h >> 16);
        return (h >> 8) * (1.0f / 16777216.0f);
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    // For each pixel, find min distance among 9 neighboring
    // jittered cell centers (3x3 around current cell). The
    // closest center owns the pixel; second-closest sets
    // mortar boundary distance.
    for (int y = 0; y < H; ++y) {
        int cy0 = y / stonePx;
        for (int x = 0; x < W; ++x) {
            int cx0 = x / stonePx;
            float bestD = 1e9f, second = 1e9f;
            int bestCx = 0, bestCy = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int cx = cx0 + dx;
                    int cy = cy0 + dy;
                    float jx = (hash01(cx, cy, 0) - 0.5f) * 0.7f;
                    float jy = (hash01(cx, cy, 1) - 0.5f) * 0.7f;
                    float ccx = (cx + 0.5f + jx) * stonePx;
                    float ccy = (cy + 0.5f + jy) * stonePx;
                    float dxp = x - ccx, dyp = y - ccy;
                    float d = std::sqrt(dxp * dxp + dyp * dyp);
                    if (d < bestD) {
                        second = bestD;
                        bestD = d;
                        bestCx = cx;
                        bestCy = cy;
                    } else if (d < second) {
                        second = d;
                    }
                }
            }
            // Pixels close to the boundary (small gap between
            // closest and second-closest) become mortar.
            float boundary = second - bestD;
            float mortarThresh = stonePx * 0.10f;
            if (boundary < mortarThresh) {
                size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
                pixels[i2 + 0] = mr;
                pixels[i2 + 1] = mg;
                pixels[i2 + 2] = mb;
            } else {
                // Per-stone tint: ±15% on each channel.
                float tint = 0.85f + 0.30f * hash01(bestCx, bestCy, 2);
                // Subtle radial darkening toward edges so
                // the stone face reads as 3D rounded.
                float edgeFalloff = std::min(1.0f,
                    (boundary - mortarThresh) / (stonePx * 0.4f));
                float shade = (0.7f + 0.3f * edgeFalloff) * tint;
                size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
                pixels[i2 + 0] = static_cast<uint8_t>(
                    std::clamp(sr * shade, 0.0f, 255.0f));
                pixels[i2 + 1] = static_cast<uint8_t>(
                    std::clamp(sg * shade, 0.0f, 255.0f));
                pixels[i2 + 2] = static_cast<uint8_t>(
                    std::clamp(sb * shade, 0.0f, 255.0f));
            }
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-cobble: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size         : %dx%d\n", W, H);
    std::printf("  stone/mortar : %s / %s\n",
                stoneHex.c_str(), mortarHex.c_str());
    std::printf("  stone px     : %d\n", stonePx);
    std::printf("  seed         : %u\n", seed);
    return 0;
}

int handleMarble(int& i, int argc, char** argv) {
    // Marble pattern via warped sinusoidal veining. The
    // canonical "marble shader": take a sine wave, warp its
    // input by smooth multi-octave noise, raise the absolute
    // value to a high power so the bright vein bands stay
    // narrow. Result: irregular bright veins on a base color
    // that tile with octave-driven low-freq variation.
    std::string outPath = argv[++i];
    std::string baseHex = argv[++i];
    std::string veinHex = argv[++i];
    uint32_t seed = 1;
    float sharpness = 8.0f;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { sharpness = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        sharpness < 1.0f || sharpness > 64.0f) {
        std::fprintf(stderr,
            "gen-texture-marble: invalid dims (W/H 1..8192, sharpness 1..64)\n");
        return 1;
    }
    uint8_t br, bg, bb_, vr, vg, vb;
    if (!parseHex(baseHex, br, bg, bb_)) {
        std::fprintf(stderr,
            "gen-texture-marble: '%s' is not a valid hex color\n",
            baseHex.c_str());
        return 1;
    }
    if (!parseHex(veinHex, vr, vg, vb)) {
        std::fprintf(stderr,
            "gen-texture-marble: '%s' is not a valid hex color\n",
            veinHex.c_str());
        return 1;
    }
    // Cheap multi-octave noise: 4 sin/cos products at
    // doubling frequencies, seeded phase per octave. Smooth
    // and tiles imperfectly but for marble we want some
    // irregularity anyway.
    float seedF = static_cast<float>(seed);
    auto warpNoise = [&](float x, float y) -> float {
        float n = 0.0f;
        float freq = 0.02f;
        float amp = 1.0f;
        float total = 0.0f;
        for (int o = 0; o < 4; ++o) {
            n += amp * std::sin(x * freq + seedF * (1.0f + o)) *
                        std::cos(y * freq + seedF * (0.6f + o));
            total += amp;
            freq *= 2.0f;
            amp *= 0.5f;
        }
        return n / total;  // -1..1
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // Warped sine: vein density is sin(turbulent x).
            // High exponent on |sin| concentrates brightness
            // into thin bands.
            float warp = warpNoise(static_cast<float>(x),
                                   static_cast<float>(y));
            float v = std::sin((x + warp * 80.0f) * 0.07f);
            float vein = std::pow(std::abs(v), sharpness);
            uint8_t r = static_cast<uint8_t>(br * (1 - vein) + vr * vein);
            uint8_t g = static_cast<uint8_t>(bg * (1 - vein) + vg * vein);
            uint8_t b = static_cast<uint8_t>(bb_ * (1 - vein) + vb * vein);
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = r;
            pixels[i2 + 1] = g;
            pixels[i2 + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-marble: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size      : %dx%d\n", W, H);
    std::printf("  base/vein : %s / %s\n",
                baseHex.c_str(), veinHex.c_str());
    std::printf("  sharpness : %.1f\n", sharpness);
    std::printf("  seed      : %u\n", seed);
    return 0;
}

int handleMetal(int& i, int argc, char** argv) {
    // Brushed-metal pattern. We generate per-pixel white
    // noise then box-blur it heavily along one axis (the
    // brush direction) and lightly along the other. Result:
    // long thin streaks of varying brightness, the visual
    // signature of brushed steel/aluminum/iron. Apply that
    // streaky shade as a multiplicative tint on the base
    // metal color.
    std::string outPath = argv[++i];
    std::string baseHex = argv[++i];
    uint32_t seed = 1;
    std::string orientation = "horizontal";
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        orientation = argv[++i];
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192) {
        std::fprintf(stderr,
            "gen-texture-metal: invalid dims (W/H 1..8192)\n");
        return 1;
    }
    if (orientation != "horizontal" && orientation != "vertical") {
        std::fprintf(stderr,
            "gen-texture-metal: orientation must be horizontal|vertical\n");
        return 1;
    }
    uint8_t mr, mg, mb;
    if (!parseHex(baseHex, mr, mg, mb)) {
        std::fprintf(stderr,
            "gen-texture-metal: '%s' is not a valid hex color\n",
            baseHex.c_str());
        return 1;
    }
    uint32_t state = seed ? seed : 1u;
    auto next01 = [&state]() -> float {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) * (1.0f / 16777216.0f);
    };
    // Step 1: per-pixel white noise.
    std::vector<float> noise(static_cast<size_t>(W) * H);
    for (auto& v : noise) v = next01();
    // Step 2: directional blur. For horizontal orientation,
    // blur strongly in X (long brush strokes) and lightly
    // in Y (thin variation across strokes). Vertical
    // orientation flips X and Y.
    std::vector<float> blurred(noise.size(), 0.0f);
    int rxLong = (orientation == "horizontal") ? 24 : 2;
    int ryLong = (orientation == "horizontal") ? 2 : 24;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float sum = 0.0f;
            int n = 0;
            for (int dy = -ryLong; dy <= ryLong; ++dy) {
                int py = y + dy;
                if (py < 0 || py >= H) continue;
                for (int dx = -rxLong; dx <= rxLong; ++dx) {
                    int px = x + dx;
                    if (px < 0 || px >= W) continue;
                    sum += noise[static_cast<size_t>(py) * W + px];
                    n++;
                }
            }
            blurred[static_cast<size_t>(y) * W + x] = sum / n;
        }
    }
    // Step 3: stretch contrast back out so the streaks
    // are visible (blurring narrows the range).
    float minV = 1.0f, maxV = 0.0f;
    for (float v : blurred) { minV = std::min(minV, v); maxV = std::max(maxV, v); }
    float range = std::max(maxV - minV, 1e-6f);
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float t = (blurred[static_cast<size_t>(y) * W + x] - minV) / range;
            // Map noise to a multiplicative shade in [0.7, 1.1]
            // so the metal looks polished but not flat.
            float shade = 0.7f + t * 0.4f;
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = static_cast<uint8_t>(
                std::clamp(mr * shade, 0.0f, 255.0f));
            pixels[i2 + 1] = static_cast<uint8_t>(
                std::clamp(mg * shade, 0.0f, 255.0f));
            pixels[i2 + 2] = static_cast<uint8_t>(
                std::clamp(mb * shade, 0.0f, 255.0f));
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-metal: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size        : %dx%d\n", W, H);
    std::printf("  base color  : %s\n", baseHex.c_str());
    std::printf("  orientation : %s\n", orientation.c_str());
    std::printf("  seed        : %u\n", seed);
    return 0;
}

int handleLeather(int& i, int argc, char** argv) {
    // Leather grain pattern. Cellular Worley noise where
    // each "pebble" cell darkens at its boundaries with
    // its neighbors — the look of fine-grain leather.
    // Each cell also gets per-cell tint variation so the
    // surface doesn't read as uniform.
    std::string outPath = argv[++i];
    std::string baseHex = argv[++i];
    uint32_t seed = 1;
    int grainSize = 4;  // average pebble cell size in px
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { grainSize = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        grainSize < 2 || grainSize > 64) {
        std::fprintf(stderr,
            "gen-texture-leather: invalid dims (W/H 1..8192, grainSize 2..64)\n");
        return 1;
    }
    uint8_t lr, lg, lb;
    if (!parseHex(baseHex, lr, lg, lb)) {
        std::fprintf(stderr,
            "gen-texture-leather: '%s' is not a valid hex color\n",
            baseHex.c_str());
        return 1;
    }
    // Per-cell hash (same idea as cobble, but smaller cells).
    auto hash01 = [seed](int cx, int cy, int comp) -> float {
        uint32_t h = static_cast<uint32_t>(cx) * 374761393u +
                     static_cast<uint32_t>(cy) * 668265263u +
                     seed * 2147483647u +
                     static_cast<uint32_t>(comp) * 16777619u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h = h ^ (h >> 16);
        return (h >> 8) * (1.0f / 16777216.0f);
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        int cy0 = y / grainSize;
        for (int x = 0; x < W; ++x) {
            int cx0 = x / grainSize;
            float bestD = 1e9f, second = 1e9f;
            int bestCx = 0, bestCy = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int cx = cx0 + dx;
                    int cy = cy0 + dy;
                    float jx = (hash01(cx, cy, 0) - 0.5f) * 0.6f;
                    float jy = (hash01(cx, cy, 1) - 0.5f) * 0.6f;
                    float ccx = (cx + 0.5f + jx) * grainSize;
                    float ccy = (cy + 0.5f + jy) * grainSize;
                    float dxp = x - ccx, dyp = y - ccy;
                    float d = std::sqrt(dxp * dxp + dyp * dyp);
                    if (d < bestD) {
                        second = bestD;
                        bestD = d;
                        bestCx = cx;
                        bestCy = cy;
                    } else if (d < second) {
                        second = d;
                    }
                }
            }
            // Boundary darkness: closer to the cell border
            // = darker. Scaled by grainSize for resolution
            // independence.
            float boundary = (second - bestD) / grainSize;
            float boundaryShade = std::clamp(boundary * 1.5f, 0.4f, 1.0f);
            // Per-cell tint: ±15% lightness.
            float tint = 0.85f + 0.30f * hash01(bestCx, bestCy, 2);
            float shade = boundaryShade * tint;
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = static_cast<uint8_t>(
                std::clamp(lr * shade, 0.0f, 255.0f));
            pixels[i2 + 1] = static_cast<uint8_t>(
                std::clamp(lg * shade, 0.0f, 255.0f));
            pixels[i2 + 2] = static_cast<uint8_t>(
                std::clamp(lb * shade, 0.0f, 255.0f));
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-leather: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  base color : %s\n", baseHex.c_str());
    std::printf("  grain size : %d px\n", grainSize);
    std::printf("  seed       : %u\n", seed);
    return 0;
}

int handleSand(int& i, int argc, char** argv) {
    // Sand dunes pattern: per-pixel salt-and-pepper grain
    // jitter (the individual grains of sand) overlaid with
    // wide sinusoidal ripple bands (the wind-formed dune
    // ridges). Result reads as windswept beach or desert.
    std::string outPath = argv[++i];
    std::string baseHex = argv[++i];
    uint32_t seed = 1;
    int rippleSpacing = 24;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { rippleSpacing = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        rippleSpacing < 4 || rippleSpacing > 512) {
        std::fprintf(stderr,
            "gen-texture-sand: invalid dims (W/H 1..8192, rippleSpacing 4..512)\n");
        return 1;
    }
    uint8_t br, bg, bb_;
    if (!parseHex(baseHex, br, bg, bb_)) {
        std::fprintf(stderr,
            "gen-texture-sand: '%s' is not a valid hex color\n",
            baseHex.c_str());
        return 1;
    }
    uint32_t state = seed ? seed : 1u;
    auto next01 = [&state]() -> float {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) * (1.0f / 16777216.0f);
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    const float pi = 3.14159265358979f;
    float seedF = static_cast<float>(seed);
    // Pre-compute one ripple offset per row so dunes flow
    // smoothly along Y rather than being identical at each row.
    std::vector<float> rowPhase(H, 0.0f);
    for (int y = 0; y < H; ++y) {
        rowPhase[y] = std::sin(y * 0.05f + seedF) * rippleSpacing * 0.5f;
    }
    for (int y = 0; y < H; ++y) {
        float phaseY = rowPhase[y];
        for (int x = 0; x < W; ++x) {
            // Ripple shade: sine band aligned to (x + phaseY).
            float ripple = std::sin((x + phaseY) * 2.0f * pi /
                                    rippleSpacing);
            float rippleShade = 1.0f + 0.10f * ripple;
            // Per-pixel grain noise: ±5% jitter.
            float grain = (next01() - 0.5f) * 0.10f;
            float shade = rippleShade + grain;
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = static_cast<uint8_t>(
                std::clamp(br * shade, 0.0f, 255.0f));
            pixels[i2 + 1] = static_cast<uint8_t>(
                std::clamp(bg * shade, 0.0f, 255.0f));
            pixels[i2 + 2] = static_cast<uint8_t>(
                std::clamp(bb_ * shade, 0.0f, 255.0f));
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-sand: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size           : %dx%d\n", W, H);
    std::printf("  base color     : %s\n", baseHex.c_str());
    std::printf("  ripple spacing : %d px\n", rippleSpacing);
    std::printf("  seed           : %u\n", seed);
    return 0;
}

int handleSnow(int& i, int argc, char** argv) {
    // Snow texture: cool-white base with very subtle blueish
    // tint variation (the soft uneven luminance of fresh
    // powder), plus scattered single-pixel "sparkles" at
    // bright white where ice crystals catch light.
    std::string outPath = argv[++i];
    std::string baseHex = argv[++i];
    uint32_t seed = 1;
    float density = 0.005f;  // fraction of pixels that sparkle
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { density = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        density < 0.0f || density > 0.5f) {
        std::fprintf(stderr,
            "gen-texture-snow: invalid dims (W/H 1..8192, density 0..0.5)\n");
        return 1;
    }
    uint8_t br, bg, bb_;
    if (!parseHex(baseHex, br, bg, bb_)) {
        std::fprintf(stderr,
            "gen-texture-snow: '%s' is not a valid hex color\n",
            baseHex.c_str());
        return 1;
    }
    uint32_t state = seed ? seed : 1u;
    auto next01 = [&state]() -> float {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) * (1.0f / 16777216.0f);
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    // Soft luminance variation via low-frequency cosine
    // sums — gives the surface a gently uneven powdery
    // look rather than a flat field.
    float seedF = static_cast<float>(seed);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float wave = std::cos(x * 0.03f + seedF) *
                         std::cos(y * 0.04f + seedF * 0.7f);
            float jitter = (next01() - 0.5f) * 0.04f;
            float shade = 1.0f + 0.05f * wave + jitter;
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = static_cast<uint8_t>(
                std::clamp(br * shade, 0.0f, 255.0f));
            pixels[i2 + 1] = static_cast<uint8_t>(
                std::clamp(bg * shade, 0.0f, 255.0f));
            pixels[i2 + 2] = static_cast<uint8_t>(
                std::clamp(bb_ * shade, 0.0f, 255.0f));
        }
    }
    // Sparkle pass: scatter bright single-pixel highlights.
    int sparkles = static_cast<int>(W * H * density);
    for (int s = 0; s < sparkles; ++s) {
        int sx = static_cast<int>(next01() * W);
        int sy = static_cast<int>(next01() * H);
        size_t i2 = (static_cast<size_t>(sy) * W + sx) * 3;
        pixels[i2 + 0] = 255;
        pixels[i2 + 1] = 255;
        pixels[i2 + 2] = 255;
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-snow: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  base color : %s\n", baseHex.c_str());
    std::printf("  density    : %.4f (%d sparkles)\n",
                density, sparkles);
    std::printf("  seed       : %u\n", seed);
    return 0;
}

int handleLava(int& i, int argc, char** argv) {
    // Lava texture: dark cooled-crust base with bright
    // glowing cracks tracing Worley cell boundaries — the
    // canonical "broken obsidian shell over magma" look.
    // Same cellular noise structure as gen-texture-cobble
    // but the boundary regions glow hot instead of darken.
    std::string outPath = argv[++i];
    std::string darkHex = argv[++i];
    std::string hotHex = argv[++i];
    uint32_t seed = 1;
    int crackScale = 32;  // average cell size in px
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { crackScale = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        crackScale < 8 || crackScale > 512) {
        std::fprintf(stderr,
            "gen-texture-lava: invalid dims (W/H 1..8192, crackScale 8..512)\n");
        return 1;
    }
    uint8_t dr, dg, db, hr, hg, hb;
    if (!parseHex(darkHex, dr, dg, db)) {
        std::fprintf(stderr,
            "gen-texture-lava: '%s' is not a valid hex color\n",
            darkHex.c_str());
        return 1;
    }
    if (!parseHex(hotHex, hr, hg, hb)) {
        std::fprintf(stderr,
            "gen-texture-lava: '%s' is not a valid hex color\n",
            hotHex.c_str());
        return 1;
    }
    auto hash01 = [seed](int cx, int cy, int comp) -> float {
        uint32_t h = static_cast<uint32_t>(cx) * 374761393u +
                     static_cast<uint32_t>(cy) * 668265263u +
                     seed * 2147483647u +
                     static_cast<uint32_t>(comp) * 16777619u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h = h ^ (h >> 16);
        return (h >> 8) * (1.0f / 16777216.0f);
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        int cy0 = y / crackScale;
        for (int x = 0; x < W; ++x) {
            int cx0 = x / crackScale;
            float bestD = 1e9f, second = 1e9f;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int cx = cx0 + dx;
                    int cy = cy0 + dy;
                    float jx = (hash01(cx, cy, 0) - 0.5f) * 0.7f;
                    float jy = (hash01(cx, cy, 1) - 0.5f) * 0.7f;
                    float ccx = (cx + 0.5f + jx) * crackScale;
                    float ccy = (cy + 0.5f + jy) * crackScale;
                    float dxp = x - ccx, dyp = y - ccy;
                    float d = std::sqrt(dxp * dxp + dyp * dyp);
                    if (d < bestD) { second = bestD; bestD = d; }
                    else if (d < second) { second = d; }
                }
            }
            // Boundary intensity: thin glow band where the
            // distance to the second-closest center is
            // close to the distance to the closest. Glow
            // strength falls off as we move away from the
            // crack into the cell interior.
            float boundary = (second - bestD) / crackScale;
            float crackWidth = 0.08f;
            float glow = 0.0f;
            if (boundary < crackWidth) {
                // Inside the crack — bright hot color.
                glow = 1.0f - boundary / crackWidth;
            } else if (boundary < crackWidth * 4.0f) {
                // Penumbra: soft glow falling off into crust.
                glow = 0.3f * (1.0f - (boundary - crackWidth) /
                                      (crackWidth * 3.0f));
            }
            glow = std::clamp(glow, 0.0f, 1.0f);
            uint8_t r = static_cast<uint8_t>(dr * (1 - glow) + hr * glow);
            uint8_t g = static_cast<uint8_t>(dg * (1 - glow) + hg * glow);
            uint8_t b = static_cast<uint8_t>(db * (1 - glow) + hb * glow);
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = r;
            pixels[i2 + 1] = g;
            pixels[i2 + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-lava: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size        : %dx%d\n", W, H);
    std::printf("  dark/hot    : %s / %s\n",
                darkHex.c_str(), hotHex.c_str());
    std::printf("  crack scale : %d px\n", crackScale);
    std::printf("  seed        : %u\n", seed);
    return 0;
}


int handleGradient(int& i, int argc, char** argv) {
    // Linear two-color gradient. Useful for sky strips, UI
    // fills, glow rings, dirt-on-grass terrain blends — the
    // common "fade" cases that --gen-texture's solid/checker/
    // grid don't cover.
    //
    // Direction: "vertical" (top→bottom, default) or
    // "horizontal" (left→right). Colors are hex like
    // --gen-texture.
    std::string outPath = argv[++i];
    std::string fromHex = argv[++i];
    std::string toHex = argv[++i];
    bool horizontal = false;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        std::string dir = argv[i + 1];
        std::transform(dir.begin(), dir.end(), dir.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (dir == "horizontal" || dir == "vertical") {
            horizontal = (dir == "horizontal");
            i++;
        }
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192) {
        std::fprintf(stderr,
            "gen-texture-gradient: invalid size %dx%d (1..8192)\n",
            W, H);
        return 1;
    }
    // Hex parser: shared local helper for both endpoints. Same
    // RRGGBB / RGB rules as --gen-texture.
    uint8_t r0, g0, b0, r1, g1, b1;
    if (!parseHex(fromHex, r0, g0, b0)) {
        std::fprintf(stderr,
            "gen-texture-gradient: '%s' is not a valid hex color\n",
            fromHex.c_str());
        return 1;
    }
    if (!parseHex(toHex, r1, g1, b1)) {
        std::fprintf(stderr,
            "gen-texture-gradient: '%s' is not a valid hex color\n",
            toHex.c_str());
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float t;
            if (horizontal) {
                t = (W <= 1) ? 0.0f : float(x) / float(W - 1);
            } else {
                t = (H <= 1) ? 0.0f : float(y) / float(H - 1);
            }
            auto lerp = [](uint8_t a, uint8_t b, float t) {
                return static_cast<uint8_t>(a + (b - a) * t + 0.5f);
            };
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = lerp(r0, r1, t);
            pixels[i2 + 1] = lerp(g0, g1, t);
            pixels[i2 + 2] = lerp(b0, b1, t);
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-gradient: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  direction  : %s\n",
                horizontal ? "horizontal" : "vertical");
    std::printf("  from       : %s (rgb %u,%u,%u)\n",
                fromHex.c_str(), r0, g0, b0);
    std::printf("  to         : %s (rgb %u,%u,%u)\n",
                toHex.c_str(), r1, g1, b1);
    return 0;
}

int handleNoise(int& i, int argc, char** argv) {
    // Smooth value-noise PNG. Useful for terrain detail
    // overlays, dirt/grass blends, magic-fog backdrops —
    // anywhere a "natural-looking" pseudo-random texture
    // beats a flat color or grid.
    //
    // Algorithm: bilinearly-interpolated 16×16 random lattice
    // sampled per pixel. Cheaper than perlin and produces a
    // similar visual signal at this resolution.
    //
    // Deterministic from the integer seed so CI runs and
    // re-runs are reproducible. Output is grayscale
    // (R==G==B per pixel) so users can tint it externally.
    std::string outPath = argv[++i];
    uint32_t seed = 1;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); }
        catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192) {
        std::fprintf(stderr,
            "gen-texture-noise: invalid size %dx%d (1..8192)\n",
            W, H);
        return 1;
    }
    // Tiny LCG (numerical recipes constants) so noise is
    // dependency-free and bit-for-bit identical across
    // platforms.
    const int latticeSize = 17;  // 16 cells × bilinear corners
    std::vector<float> lattice(latticeSize * latticeSize);
    uint32_t state = seed ? seed : 1u;
    auto next = [&]() -> float {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) / float(1 << 24);
    };
    for (auto& v : lattice) v = next();
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        float fy = static_cast<float>(y) / H * (latticeSize - 1);
        int yi = static_cast<int>(fy);
        if (yi >= latticeSize - 1) yi = latticeSize - 2;
        float fty = fy - yi;
        // Smoothstep so cell boundaries don't show as bands.
        float ty = fty * fty * (3.0f - 2.0f * fty);
        for (int x = 0; x < W; ++x) {
            float fx = static_cast<float>(x) / W * (latticeSize - 1);
            int xi = static_cast<int>(fx);
            if (xi >= latticeSize - 1) xi = latticeSize - 2;
            float ftx = fx - xi;
            float tx = ftx * ftx * (3.0f - 2.0f * ftx);
            float a = lattice[yi * latticeSize + xi];
            float b = lattice[yi * latticeSize + xi + 1];
            float c = lattice[(yi + 1) * latticeSize + xi];
            float d = lattice[(yi + 1) * latticeSize + xi + 1];
            float ab = a + (b - a) * tx;
            float cd = c + (d - c) * tx;
            float v = ab + (cd - ab) * ty;
            uint8_t g = static_cast<uint8_t>(v * 255.0f + 0.5f);
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = g;
            pixels[i2 + 1] = g;
            pixels[i2 + 2] = g;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-noise: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size  : %dx%d\n", W, H);
    std::printf("  seed  : %u\n", seed);
    std::printf("  type  : smooth value noise (16x16 bilinear lattice)\n");
    return 0;
}

int handleNoiseColor(int& i, int argc, char** argv) {
    // Two-color noise blend: same value-noise function as
    // --gen-texture-noise but interpolated between two RGB
    // endpoints rather than emitted as grayscale. Useful
    // for terrain detail (grass+dirt mottle), magic fog,
    // marble veining, or any "natural variation" pass that
    // shouldn't be desaturated.
    std::string outPath = argv[++i];
    std::string aHex = argv[++i];
    std::string bHex = argv[++i];
    uint32_t seed = 1;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); }
        catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192) {
        std::fprintf(stderr,
            "gen-texture-noise-color: invalid size %dx%d\n", W, H);
        return 1;
    }
    uint8_t ra, ga, ba, rb, gb, bb;
    if (!parseHex(aHex, ra, ga, ba)) {
        std::fprintf(stderr,
            "gen-texture-noise-color: '%s' is not a valid hex color\n",
            aHex.c_str());
        return 1;
    }
    if (!parseHex(bHex, rb, gb, bb)) {
        std::fprintf(stderr,
            "gen-texture-noise-color: '%s' is not a valid hex color\n",
            bHex.c_str());
        return 1;
    }
    // Same noise pipeline as --gen-texture-noise.
    const int latticeSize = 17;
    std::vector<float> lattice(latticeSize * latticeSize);
    uint32_t state = seed ? seed : 1u;
    auto next = [&]() -> float {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) / float(1 << 24);
    };
    for (auto& v : lattice) v = next();
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        float fy = static_cast<float>(y) / H * (latticeSize - 1);
        int yi = static_cast<int>(fy);
        if (yi >= latticeSize - 1) yi = latticeSize - 2;
        float fty = fy - yi;
        float ty = fty * fty * (3.0f - 2.0f * fty);
        for (int x = 0; x < W; ++x) {
            float fx = static_cast<float>(x) / W * (latticeSize - 1);
            int xi = static_cast<int>(fx);
            if (xi >= latticeSize - 1) xi = latticeSize - 2;
            float ftx = fx - xi;
            float tx = ftx * ftx * (3.0f - 2.0f * ftx);
            float a = lattice[yi * latticeSize + xi];
            float b = lattice[yi * latticeSize + xi + 1];
            float c = lattice[(yi + 1) * latticeSize + xi];
            float d = lattice[(yi + 1) * latticeSize + xi + 1];
            float ab = a + (b - a) * tx;
            float cd = c + (d - c) * tx;
            float v = ab + (cd - ab) * ty;
            auto lerp = [](uint8_t lo, uint8_t hi, float t) {
                return static_cast<uint8_t>(lo + (hi - lo) * t + 0.5f);
            };
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = lerp(ra, rb, v);
            pixels[i2 + 1] = lerp(ga, gb, v);
            pixels[i2 + 2] = lerp(ba, bb, v);
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-noise-color: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size  : %dx%d\n", W, H);
    std::printf("  seed  : %u\n", seed);
    std::printf("  from  : %s\n", aHex.c_str());
    std::printf("  to    : %s\n", bHex.c_str());
    return 0;
}

int handleRadial(int& i, int argc, char** argv) {
    // Radial gradient: centerHex at the image center fading
    // smoothly to edgeHex at the corner. Useful for spell
    // glow rings, vignettes, soft-edged decals — the
    // common "circular blob" cases that linear gradients
    // can't produce.
    //
    // Distance is normalized so the corner is t=1 (image is
    // not necessarily square). A smoothstep curve gives a
    // soft falloff rather than a harsh disc edge.
    std::string outPath = argv[++i];
    std::string centerHex = argv[++i];
    std::string edgeHex = argv[++i];
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192) {
        std::fprintf(stderr,
            "gen-texture-radial: invalid size %dx%d (1..8192)\n",
            W, H);
        return 1;
    }
    uint8_t rc, gc, bc, re, ge, be;
    if (!parseHex(centerHex, rc, gc, bc)) {
        std::fprintf(stderr,
            "gen-texture-radial: '%s' is not a valid hex color\n",
            centerHex.c_str());
        return 1;
    }
    if (!parseHex(edgeHex, re, ge, be)) {
        std::fprintf(stderr,
            "gen-texture-radial: '%s' is not a valid hex color\n",
            edgeHex.c_str());
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    float cx = (W - 1) * 0.5f;
    float cy = (H - 1) * 0.5f;
    // Max distance is the corner (cx, cy itself = half-diag).
    float maxD = std::sqrt(cx * cx + cy * cy);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float dx = x - cx;
            float dy = y - cy;
            float d = std::sqrt(dx * dx + dy * dy);
            float t = (maxD > 0) ? (d / maxD) : 0.0f;
            if (t > 1.0f) t = 1.0f;
            // Smoothstep so the falloff is soft.
            float smt = t * t * (3.0f - 2.0f * t);
            auto lerp = [](uint8_t a, uint8_t b, float t) {
                return static_cast<uint8_t>(a + (b - a) * t + 0.5f);
            };
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = lerp(rc, re, smt);
            pixels[i2 + 1] = lerp(gc, ge, smt);
            pixels[i2 + 2] = lerp(bc, be, smt);
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-radial: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size   : %dx%d\n", W, H);
    std::printf("  center : %s (rgb %u,%u,%u)\n",
                centerHex.c_str(), rc, gc, bc);
    std::printf("  edge   : %s (rgb %u,%u,%u)\n",
                edgeHex.c_str(), re, ge, be);
    return 0;
}

int handleStripes(int& i, int argc, char** argv) {
    // Two-color stripe pattern. Stripe width in pixels, plus
    // direction (diagonal default, or horizontal/vertical).
    // Useful for caution tape, marble bands, hazard markers,
    // and racing-style start/finish flags — patterns that
    // checker/grid don't capture.
    std::string outPath = argv[++i];
    std::string aHex = argv[++i];
    std::string bHex = argv[++i];
    int stripePx = 16;
    std::string dir = "diagonal";
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { stripePx = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        std::string d = argv[i + 1];
        std::transform(d.begin(), d.end(), d.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (d == "diagonal" || d == "horizontal" || d == "vertical") {
            dir = d;
            i++;
        }
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        stripePx < 1 || stripePx > 4096) {
        std::fprintf(stderr,
            "gen-texture-stripes: invalid dims (W/H 1..8192, stripe 1..4096)\n");
        return 1;
    }
    uint8_t ra, ga, ba, rb, gb, bb;
    if (!parseHex(aHex, ra, ga, ba)) {
        std::fprintf(stderr,
            "gen-texture-stripes: '%s' is not a valid hex color\n",
            aHex.c_str());
        return 1;
    }
    if (!parseHex(bHex, rb, gb, bb)) {
        std::fprintf(stderr,
            "gen-texture-stripes: '%s' is not a valid hex color\n",
            bHex.c_str());
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int proj;
            if (dir == "horizontal") proj = y;
            else if (dir == "vertical") proj = x;
            else proj = x + y;
            bool isA = ((proj / stripePx) & 1) == 0;
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = isA ? ra : rb;
            pixels[i2 + 1] = isA ? ga : gb;
            pixels[i2 + 2] = isA ? ba : bb;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-stripes: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size      : %dx%d\n", W, H);
    std::printf("  direction : %s\n", dir.c_str());
    std::printf("  stripe    : %d px\n", stripePx);
    std::printf("  colors    : %s + %s\n", aHex.c_str(), bHex.c_str());
    return 0;
}

int handleDots(int& i, int argc, char** argv) {
    // Polka-dot pattern: solid background with circular dots
    // on a regular grid. Useful for fabric/clothing textures,
    // game-board patterns, or quick decorative tiling.
    std::string outPath = argv[++i];
    std::string bgHex = argv[++i];
    std::string dotHex = argv[++i];
    int radius = 8, spacing = 32;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { radius = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { spacing = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        radius < 1 || radius > 1024 ||
        spacing < 2 || spacing > 4096) {
        std::fprintf(stderr,
            "gen-texture-dots: invalid dims (W/H 1..8192, radius 1..1024, spacing 2..4096)\n");
        return 1;
    }
    uint8_t br, bg, bb, dr, dg, db;
    if (!parseHex(bgHex, br, bg, bb)) {
        std::fprintf(stderr,
            "gen-texture-dots: '%s' is not a valid hex color\n",
            bgHex.c_str());
        return 1;
    }
    if (!parseHex(dotHex, dr, dg, db)) {
        std::fprintf(stderr,
            "gen-texture-dots: '%s' is not a valid hex color\n",
            dotHex.c_str());
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    float r2 = static_cast<float>(radius) * radius;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // Distance to the nearest grid point.
            int gx = (x + spacing / 2) / spacing * spacing;
            int gy = (y + spacing / 2) / spacing * spacing;
            float dx = static_cast<float>(x - gx);
            float dy = static_cast<float>(y - gy);
            bool inDot = (dx * dx + dy * dy) < r2;
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = inDot ? dr : br;
            pixels[i2 + 1] = inDot ? dg : bg;
            pixels[i2 + 2] = inDot ? db : bb;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-dots: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size      : %dx%d\n", W, H);
    std::printf("  bg        : %s\n", bgHex.c_str());
    std::printf("  dot       : %s\n", dotHex.c_str());
    std::printf("  radius    : %d px\n", radius);
    std::printf("  spacing   : %d px\n", spacing);
    return 0;
}

int handleRings(int& i, int argc, char** argv) {
    // Concentric rings centered on the image. Useful for
    // archery targets, magic seal floors, dartboards, hypnosis
    // visuals — anywhere a "circular alternation" reads as
    // intentional design.
    std::string outPath = argv[++i];
    std::string aHex = argv[++i];
    std::string bHex = argv[++i];
    int ringPx = 16;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { ringPx = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        ringPx < 1 || ringPx > 4096) {
        std::fprintf(stderr,
            "gen-texture-rings: invalid dims (W/H 1..8192, ringPx 1..4096)\n");
        return 1;
    }
    uint8_t ra, ga, ba, rb, gb, bb;
    if (!parseHex(aHex, ra, ga, ba)) {
        std::fprintf(stderr,
            "gen-texture-rings: '%s' is not a valid hex color\n",
            aHex.c_str());
        return 1;
    }
    if (!parseHex(bHex, rb, gb, bb)) {
        std::fprintf(stderr,
            "gen-texture-rings: '%s' is not a valid hex color\n",
            bHex.c_str());
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    float cx = (W - 1) * 0.5f;
    float cy = (H - 1) * 0.5f;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float dx = x - cx;
            float dy = y - cy;
            float d = std::sqrt(dx * dx + dy * dy);
            bool isA = (static_cast<int>(d / ringPx) & 1) == 0;
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = isA ? ra : rb;
            pixels[i2 + 1] = isA ? ga : gb;
            pixels[i2 + 2] = isA ? ba : bb;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-rings: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size      : %dx%d\n", W, H);
    std::printf("  ring px   : %d\n", ringPx);
    std::printf("  colors    : %s + %s\n", aHex.c_str(), bHex.c_str());
    return 0;
}

int handleChecker(int& i, int argc, char** argv) {
    // Two-color checkerboard with custom colors. The
    // existing --gen-texture's "checker" pattern is fixed
    // black/white at 32px; this is the configurable variant
    // for game boards, kitchen floors, hazard markers in
    // colors other than monochrome.
    std::string outPath = argv[++i];
    std::string aHex = argv[++i];
    std::string bHex = argv[++i];
    int cellPx = 32;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { cellPx = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        cellPx < 1 || cellPx > 4096) {
        std::fprintf(stderr,
            "gen-texture-checker: invalid dims (W/H 1..8192, cellPx 1..4096)\n");
        return 1;
    }
    uint8_t ra, ga, ba, rb, gb, bb;
    if (!parseHex(aHex, ra, ga, ba)) {
        std::fprintf(stderr,
            "gen-texture-checker: '%s' is not a valid hex color\n",
            aHex.c_str());
        return 1;
    }
    if (!parseHex(bHex, rb, gb, bb)) {
        std::fprintf(stderr,
            "gen-texture-checker: '%s' is not a valid hex color\n",
            bHex.c_str());
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            bool isA = ((x / cellPx) + (y / cellPx)) % 2 == 0;
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = isA ? ra : rb;
            pixels[i2 + 1] = isA ? ga : gb;
            pixels[i2 + 2] = isA ? ba : bb;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-checker: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size     : %dx%d\n", W, H);
    std::printf("  cell px  : %d\n", cellPx);
    std::printf("  colors   : %s + %s\n", aHex.c_str(), bHex.c_str());
    return 0;
}

int handleBrick(int& i, int argc, char** argv) {
    // Brick wall pattern: rectangular bricks with offset rows
    // (each row shifted by half a brick width) and mortar
    // lines between. Useful for walls, chimneys, paths,
    // medieval-zone props.
    std::string outPath = argv[++i];
    std::string brickHex = argv[++i];
    std::string mortarHex = argv[++i];
    int brickW = 64, brickH = 24, mortarPx = 4;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { brickW = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { brickH = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { mortarPx = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        brickW < 4 || brickW > 4096 ||
        brickH < 4 || brickH > 4096 ||
        mortarPx < 0 || mortarPx > brickH / 2) {
        std::fprintf(stderr,
            "gen-texture-brick: invalid dims (W/H 1..8192, brick 4..4096, mortar < brickH/2)\n");
        return 1;
    }
    uint8_t br, bg, bb_, mr, mg, mb;
    if (!parseHex(brickHex, br, bg, bb_)) {
        std::fprintf(stderr,
            "gen-texture-brick: '%s' is not a valid hex color\n",
            brickHex.c_str());
        return 1;
    }
    if (!parseHex(mortarHex, mr, mg, mb)) {
        std::fprintf(stderr,
            "gen-texture-brick: '%s' is not a valid hex color\n",
            mortarHex.c_str());
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    int rowH = brickH;  // total row height (brick + mortar)
    int halfBrick = brickW / 2;
    for (int y = 0; y < H; ++y) {
        int row = y / rowH;
        int yInRow = y % rowH;
        bool inMortarH = (yInRow < mortarPx);
        int xOffset = (row & 1) ? halfBrick : 0;
        for (int x = 0; x < W; ++x) {
            int xS = (x + xOffset) % brickW;
            bool inMortarV = (xS < mortarPx);
            bool isMortar = inMortarH || inMortarV;
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = isMortar ? mr : br;
            pixels[i2 + 1] = isMortar ? mg : bg;
            pixels[i2 + 2] = isMortar ? mb : bb_;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-brick: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size      : %dx%d\n", W, H);
    std::printf("  brick     : %d × %d px (%s)\n",
                brickW, brickH, brickHex.c_str());
    std::printf("  mortar    : %d px (%s)\n",
                mortarPx, mortarHex.c_str());
    return 0;
}

int handleWood(int& i, int argc, char** argv) {
    // Wood grain pattern: vertical streaks of varying width
    // alternating between light and dark hues, plus a few
    // pseudo-random "knots" (small dark dots). Suitable for
    // doors, planks, fences, crates.
    std::string outPath = argv[++i];
    std::string lightHex = argv[++i];
    std::string darkHex = argv[++i];
    int spacing = 12;     // average grain spacing in px
    uint32_t seed = 1;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { spacing = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        spacing < 2 || spacing > 256) {
        std::fprintf(stderr,
            "gen-texture-wood: invalid dims (W/H 1..8192, spacing 2..256)\n");
        return 1;
    }
    uint8_t lr, lg, lb, dr, dg, db;
    if (!parseHex(lightHex, lr, lg, lb)) {
        std::fprintf(stderr,
            "gen-texture-wood: '%s' is not a valid hex color\n",
            lightHex.c_str());
        return 1;
    }
    if (!parseHex(darkHex, dr, dg, db)) {
        std::fprintf(stderr,
            "gen-texture-wood: '%s' is not a valid hex color\n",
            darkHex.c_str());
        return 1;
    }
    // Tiny LCG so output is reproducible from `seed` alone
    // without pulling in <random>.
    uint32_t state = seed ? seed : 1u;
    auto next01 = [&state]() -> float {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) * (1.0f / 16777216.0f);
    };
    // Pre-compute per-column "darkness" weight by accumulating
    // grain bands of varying width across the image. A band's
    // weight bleeds into a few neighbors so transitions feel
    // soft rather than blocky.
    std::vector<float> colWeight(W, 0.0f);
    int x = 0;
    while (x < W) {
        int width = spacing + static_cast<int>(next01() * spacing);
        float weight = next01();  // 0..1
        int feather = std::max(1, width / 6);
        for (int dx = -feather; dx < width + feather; ++dx) {
            int cx = x + dx;
            if (cx < 0 || cx >= W) continue;
            float t = 1.0f;
            if (dx < 0) t = 1.0f + dx / static_cast<float>(feather);
            else if (dx >= width) t = 1.0f - (dx - width) / static_cast<float>(feather);
            colWeight[cx] = std::max(colWeight[cx], weight * t);
        }
        x += width;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        // Slight Y-axis warp so streaks aren't perfectly straight
        float yWave = std::sin(y * 0.025f) * 1.5f;
        for (int xi = 0; xi < W; ++xi) {
            int sx = xi + static_cast<int>(yWave);
            if (sx < 0) sx = 0;
            if (sx >= W) sx = W - 1;
            float w = colWeight[sx];
            uint8_t r = static_cast<uint8_t>(lr * (1 - w) + dr * w);
            uint8_t g = static_cast<uint8_t>(lg * (1 - w) + dg * w);
            uint8_t b = static_cast<uint8_t>(lb * (1 - w) + db * w);
            size_t i2 = (static_cast<size_t>(y) * W + xi) * 3;
            pixels[i2 + 0] = r;
            pixels[i2 + 1] = g;
            pixels[i2 + 2] = b;
        }
    }
    // Sprinkle a handful of round "knots" using the same LCG.
    int knotCount = std::max(1, (W * H) / 32768);
    for (int k = 0; k < knotCount; ++k) {
        int kx = static_cast<int>(next01() * W);
        int ky = static_cast<int>(next01() * H);
        int radius = 3 + static_cast<int>(next01() * 4);
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                int px = kx + dx, py = ky + dy;
                if (px < 0 || py < 0 || px >= W || py >= H) continue;
                float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (d > radius) continue;
                float t = 1.0f - d / radius;
                size_t i2 = (static_cast<size_t>(py) * W + px) * 3;
                pixels[i2 + 0] = static_cast<uint8_t>(pixels[i2 + 0] * (1 - t) + dr * t);
                pixels[i2 + 1] = static_cast<uint8_t>(pixels[i2 + 1] * (1 - t) + dg * t);
                pixels[i2 + 2] = static_cast<uint8_t>(pixels[i2 + 2] * (1 - t) + db * t);
            }
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-wood: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size      : %dx%d\n", W, H);
    std::printf("  light/dark: %s / %s\n",
                lightHex.c_str(), darkHex.c_str());
    std::printf("  spacing   : %d px\n", spacing);
    std::printf("  knots     : %d\n", knotCount);
    std::printf("  seed      : %u\n", seed);
    return 0;
}

int handleGrass(int& i, int argc, char** argv) {
    // Tiling grass texture. Starts from a slightly perturbed
    // base color (per-pixel jitter so the field doesn't read
    // as flat), then sprinkles short blade highlights using
    // the brighter blade color. Density controls roughly
    // what fraction of pixels get touched by a blade.
    std::string outPath = argv[++i];
    std::string baseHex = argv[++i];
    std::string bladeHex = argv[++i];
    float density = 0.15f;
    uint32_t seed = 1;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { density = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        density < 0.0f || density > 1.0f) {
        std::fprintf(stderr,
            "gen-texture-grass: invalid dims (W/H 1..8192, density 0..1)\n");
        return 1;
    }
    uint8_t br, bg, bb_, gr, gg, gb;
    if (!parseHex(baseHex, br, bg, bb_)) {
        std::fprintf(stderr,
            "gen-texture-grass: '%s' is not a valid hex color\n",
            baseHex.c_str());
        return 1;
    }
    if (!parseHex(bladeHex, gr, gg, gb)) {
        std::fprintf(stderr,
            "gen-texture-grass: '%s' is not a valid hex color\n",
            bladeHex.c_str());
        return 1;
    }
    uint32_t state = seed ? seed : 1u;
    auto next01 = [&state]() -> float {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) * (1.0f / 16777216.0f);
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    // Base layer: per-pixel jitter ±10 around the base color.
    for (int y = 0; y < H; ++y) {
        for (int xi = 0; xi < W; ++xi) {
            float j = (next01() - 0.5f) * 20.0f;
            int r = std::clamp(static_cast<int>(br) + static_cast<int>(j), 0, 255);
            int g = std::clamp(static_cast<int>(bg) + static_cast<int>(j), 0, 255);
            int b = std::clamp(static_cast<int>(bb_) + static_cast<int>(j), 0, 255);
            size_t i2 = (static_cast<size_t>(y) * W + xi) * 3;
            pixels[i2 + 0] = static_cast<uint8_t>(r);
            pixels[i2 + 1] = static_cast<uint8_t>(g);
            pixels[i2 + 2] = static_cast<uint8_t>(b);
        }
    }
    // Blades: short vertical strokes at random positions.
    // Stroke length 2-5px, alpha-blended toward bladeHex.
    int strokeCount = static_cast<int>(W * H * density * 0.05f);
    for (int s = 0; s < strokeCount; ++s) {
        int sx = static_cast<int>(next01() * W);
        int sy = static_cast<int>(next01() * H);
        int slen = 2 + static_cast<int>(next01() * 4);
        float t = 0.4f + next01() * 0.4f;  // blade strength
        for (int dy = 0; dy < slen; ++dy) {
            int py = (sy + dy) % H;  // wrap so texture tiles
            int px = sx;
            size_t i2 = (static_cast<size_t>(py) * W + px) * 3;
            pixels[i2 + 0] = static_cast<uint8_t>(pixels[i2 + 0] * (1 - t) + gr * t);
            pixels[i2 + 1] = static_cast<uint8_t>(pixels[i2 + 1] * (1 - t) + gg * t);
            pixels[i2 + 2] = static_cast<uint8_t>(pixels[i2 + 2] * (1 - t) + gb * t);
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-grass: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size      : %dx%d\n", W, H);
    std::printf("  base/blade: %s / %s\n",
                baseHex.c_str(), bladeHex.c_str());
    std::printf("  density   : %.3f\n", density);
    std::printf("  blades    : %d\n", strokeCount);
    std::printf("  seed      : %u\n", seed);
    return 0;
}

int handleFabric(int& i, int argc, char** argv) {
    // Woven fabric pattern. We model an over/under weave: each
    // "cell" of size threadPx × threadPx is alternately a warp
    // (vertical) thread or a weft (horizontal) thread. Within
    // a thread, brightness shades from edge to center so the
    // weave reads as 3D yarn rather than flat checkerboard.
    std::string outPath = argv[++i];
    std::string warpHex = argv[++i];
    std::string weftHex = argv[++i];
    int threadPx = 4;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { threadPx = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        threadPx < 2 || threadPx > 256) {
        std::fprintf(stderr,
            "gen-texture-fabric: invalid dims (W/H 1..8192, threadPx 2..256)\n");
        return 1;
    }
    uint8_t wr, wg, wb, fr, fg, fb;
    if (!parseHex(warpHex, wr, wg, wb)) {
        std::fprintf(stderr,
            "gen-texture-fabric: '%s' is not a valid hex color\n",
            warpHex.c_str());
        return 1;
    }
    if (!parseHex(weftHex, fr, fg, fb)) {
        std::fprintf(stderr,
            "gen-texture-fabric: '%s' is not a valid hex color\n",
            weftHex.c_str());
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        int cy = y / threadPx;
        int yInCell = y % threadPx;
        for (int x = 0; x < W; ++x) {
            int cx = x / threadPx;
            int xInCell = x % threadPx;
            // Plain weave: alternate warp/weft per cell on
            // a checkerboard. Warp threads run vertically
            // (so we shade across xInCell), weft threads
            // run horizontally (shade across yInCell).
            bool isWarp = ((cx + cy) & 1) == 0;
            int across = isWarp ? xInCell : yInCell;
            float t = static_cast<float>(across) / (threadPx - 1);
            // Center is brighter, edges darker — gives the
            // illusion of a rounded yarn cross-section.
            float shade = 1.0f - 0.4f * std::abs(t - 0.5f) * 2.0f;
            uint8_t r = isWarp ? static_cast<uint8_t>(wr * shade)
                               : static_cast<uint8_t>(fr * shade);
            uint8_t g = isWarp ? static_cast<uint8_t>(wg * shade)
                               : static_cast<uint8_t>(fg * shade);
            uint8_t b = isWarp ? static_cast<uint8_t>(wb * shade)
                               : static_cast<uint8_t>(fb * shade);
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = r;
            pixels[i2 + 1] = g;
            pixels[i2 + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-fabric: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  warp/weft  : %s / %s\n",
                warpHex.c_str(), weftHex.c_str());
    std::printf("  thread px  : %d\n", threadPx);
    return 0;
}

int handleTile(int& i, int argc, char** argv) {
    // Square stone tile pattern: each cell is one tile face,
    // separated by grout lines on every grid edge. Tiles get
    // small per-tile shade jitter so the surface doesn't read
    // as a flat regular grid; grout is the constant separator
    // color. Floors, plaza paving, dungeon walls.
    std::string outPath = argv[++i];
    std::string tileHex = argv[++i];
    std::string groutHex = argv[++i];
    int tilePx = 32;
    int groutPx = 2;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { tilePx = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { groutPx = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        tilePx < 4 || tilePx > 1024 ||
        groutPx < 0 || groutPx > tilePx / 2) {
        std::fprintf(stderr,
            "gen-texture-tile: invalid dims (W/H 1..8192, tile 4..1024, grout < tile/2)\n");
        return 1;
    }
    uint8_t tr, tg, tb, gr, gg, gb;
    if (!parseHex(tileHex, tr, tg, tb)) {
        std::fprintf(stderr,
            "gen-texture-tile: '%s' is not a valid hex color\n",
            tileHex.c_str());
        return 1;
    }
    if (!parseHex(groutHex, gr, gg, gb)) {
        std::fprintf(stderr,
            "gen-texture-tile: '%s' is not a valid hex color\n",
            groutHex.c_str());
        return 1;
    }
    // Per-tile shade jitter. Hash the integer cell coords for
    // a stable shade per tile so adjacent tiles look distinct.
    auto cellShade = [](int cx, int cy) -> float {
        uint32_t h = static_cast<uint32_t>(cx) * 374761393u +
                     static_cast<uint32_t>(cy) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h = h ^ (h >> 16);
        float n = (h >> 8) * (1.0f / 16777216.0f);  // 0..1
        return 0.92f + 0.16f * n;                    // 0.92..1.08
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        int cy = y / tilePx;
        int yInCell = y % tilePx;
        bool yGrout = (yInCell < groutPx);
        for (int x = 0; x < W; ++x) {
            int cx = x / tilePx;
            int xInCell = x % tilePx;
            bool xGrout = (xInCell < groutPx);
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            if (xGrout || yGrout) {
                pixels[i2 + 0] = gr;
                pixels[i2 + 1] = gg;
                pixels[i2 + 2] = gb;
            } else {
                float shade = cellShade(cx, cy);
                pixels[i2 + 0] = static_cast<uint8_t>(
                    std::clamp(tr * shade, 0.0f, 255.0f));
                pixels[i2 + 1] = static_cast<uint8_t>(
                    std::clamp(tg * shade, 0.0f, 255.0f));
                pixels[i2 + 2] = static_cast<uint8_t>(
                    std::clamp(tb * shade, 0.0f, 255.0f));
            }
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-tile: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  tile/grout : %s / %s\n",
                tileHex.c_str(), groutHex.c_str());
    std::printf("  tile px    : %d\n", tilePx);
    std::printf("  grout px   : %d\n", groutPx);
    return 0;
}

int handleBark(int& i, int argc, char** argv) {
    // Tree bark: vertical wavy streaks (the trunk's growth lines)
    // plus dark vertical cracks at random columns (where bark
    // splits as the tree expands). Streaks waver per row via a
    // smooth cosine offset so the texture doesn't look gridded.
    std::string outPath = argv[++i];
    std::string baseHex = argv[++i];
    std::string crackHex = argv[++i];
    uint32_t seed = 1;
    float density = 0.04f;  // fraction of columns that become cracks
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { density = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        density < 0.0f || density > 0.5f) {
        std::fprintf(stderr,
            "gen-texture-bark: invalid dims (W/H 1..8192, density 0..0.5)\n");
        return 1;
    }
    uint8_t br, bg, bb_, cr, cg, cb;
    if (!parseHex(baseHex, br, bg, bb_)) {
        std::fprintf(stderr,
            "gen-texture-bark: '%s' is not a valid hex color\n",
            baseHex.c_str());
        return 1;
    }
    if (!parseHex(crackHex, cr, cg, cb)) {
        std::fprintf(stderr,
            "gen-texture-bark: '%s' is not a valid hex color\n",
            crackHex.c_str());
        return 1;
    }
    uint32_t state = seed ? seed : 1u;
    auto next01 = [&state]() -> float {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) * (1.0f / 16777216.0f);
    };
    // Pick crack columns up front (sparse).
    int crackCount = static_cast<int>(W * density);
    std::vector<int> crackCols;
    crackCols.reserve(crackCount);
    for (int k = 0; k < crackCount; ++k) {
        crackCols.push_back(static_cast<int>(next01() * W));
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    float seedF = static_cast<float>(seed);
    // Per-column shade variation (each vertical streak has its own
    // brightness derived from a column hash). Pre-compute so each
    // pixel just reads the column.
    std::vector<float> colShade(W);
    for (int x = 0; x < W; ++x) {
        // Stable column hash → 0.85..1.10 shade
        uint32_t h = static_cast<uint32_t>(x) * 2654435761u + seed;
        h = (h ^ (h >> 13)) * 1274126177u;
        h = h ^ (h >> 16);
        float n = (h >> 8) * (1.0f / 16777216.0f);
        colShade[x] = 0.85f + 0.25f * n;
    }
    for (int y = 0; y < H; ++y) {
        // Slow horizontal sway so vertical streaks waver per row.
        float sway = std::sin(y * 0.04f + seedF * 0.3f) * 1.5f;
        for (int x = 0; x < W; ++x) {
            int sx = x + static_cast<int>(sway);
            if (sx < 0) sx = 0;
            if (sx >= W) sx = W - 1;
            float shade = colShade[sx];
            uint8_t r = static_cast<uint8_t>(std::clamp(br * shade, 0.0f, 255.0f));
            uint8_t g = static_cast<uint8_t>(std::clamp(bg * shade, 0.0f, 255.0f));
            uint8_t b = static_cast<uint8_t>(std::clamp(bb_ * shade, 0.0f, 255.0f));
            // Crack overlay: any pixel within 1 px of a crack column
            // (with sway applied) becomes the crack color.
            for (int cc : crackCols) {
                if (std::abs(sx - cc) <= 1) {
                    r = cr; g = cg; b = cb;
                    break;
                }
            }
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = r;
            pixels[i2 + 1] = g;
            pixels[i2 + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-bark: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  base/crack : %s / %s\n",
                baseHex.c_str(), crackHex.c_str());
    std::printf("  density    : %.4f (%d cracks)\n",
                density, crackCount);
    std::printf("  seed       : %u\n", seed);
    return 0;
}

int handleClouds(int& i, int argc, char** argv) {
    // Sky with puffy clouds. Multi-octave smooth noise (4
    // octaves of cosine-product noise at doubling frequencies)
    // gives soft cloud blobs; the result is thresholded by
    // `coverage` so values above the threshold blend toward
    // cloud color, and values below fade smoothly to sky.
    std::string outPath = argv[++i];
    std::string skyHex = argv[++i];
    std::string cloudHex = argv[++i];
    uint32_t seed = 1;
    float coverage = 0.5f;  // 0=clear sky, 1=overcast
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { coverage = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        coverage < 0.0f || coverage > 1.0f) {
        std::fprintf(stderr,
            "gen-texture-clouds: invalid dims (W/H 1..8192, coverage 0..1)\n");
        return 1;
    }
    uint8_t sr, sg, sb, cr, cg, cb;
    if (!parseHex(skyHex, sr, sg, sb)) {
        std::fprintf(stderr,
            "gen-texture-clouds: '%s' is not a valid hex color\n",
            skyHex.c_str());
        return 1;
    }
    if (!parseHex(cloudHex, cr, cg, cb)) {
        std::fprintf(stderr,
            "gen-texture-clouds: '%s' is not a valid hex color\n",
            cloudHex.c_str());
        return 1;
    }
    float seedF = static_cast<float>(seed);
    auto cloudNoise = [&](float x, float y) -> float {
        // 4 octaves of sin/cos noise at doubling frequency,
        // halving amplitude. Output in 0..1 after normalize.
        float n = 0.0f;
        float total = 0.0f;
        float freq = 0.015f;
        float amp = 1.0f;
        for (int o = 0; o < 4; ++o) {
            n += amp * (0.5f + 0.5f *
                std::sin(x * freq + seedF * (1.0f + o * 0.7f)) *
                std::cos(y * freq + seedF * (0.5f + o * 0.4f)));
            total += amp;
            freq *= 2.0f;
            amp *= 0.5f;
        }
        return n / total;
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    // Coverage maps to a noise threshold: low coverage = high
    // threshold (only the brightest noise becomes clouds);
    // high coverage = low threshold (more area is cloudy).
    float thresh = 1.0f - coverage;
    int cloudPixels = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float n = cloudNoise(static_cast<float>(x),
                                 static_cast<float>(y));
            // Smooth blend across a 0.15-wide band around the
            // threshold so cloud edges feather rather than step.
            float t = std::clamp((n - thresh) / 0.15f, 0.0f, 1.0f);
            if (t > 0.5f) ++cloudPixels;
            uint8_t r = static_cast<uint8_t>(sr * (1 - t) + cr * t);
            uint8_t g = static_cast<uint8_t>(sg * (1 - t) + cg * t);
            uint8_t b = static_cast<uint8_t>(sb * (1 - t) + cb * t);
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = r;
            pixels[i2 + 1] = g;
            pixels[i2 + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-clouds: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  sky/cloud  : %s / %s\n",
                skyHex.c_str(), cloudHex.c_str());
    std::printf("  coverage   : %.2f (%d cloud pixels)\n",
                coverage, cloudPixels);
    std::printf("  seed       : %u\n", seed);
    return 0;
}

int handleStars(int& i, int argc, char** argv) {
    // Night sky: solid background color sprinkled with bright
    // stars at random positions and varied per-star brightness
    // (so the sky has a depth feel — bright nearby stars + dim
    // distant ones). Density controls roughly what fraction of
    // pixels become stars.
    std::string outPath = argv[++i];
    std::string bgHex = argv[++i];
    std::string starHex = argv[++i];
    uint32_t seed = 1;
    float density = 0.005f;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { density = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        density < 0.0f || density > 1.0f) {
        std::fprintf(stderr,
            "gen-texture-stars: invalid dims (W/H 1..8192, density 0..1)\n");
        return 1;
    }
    uint8_t br, bg, bb_, sr, sg, sb;
    if (!parseHex(bgHex, br, bg, bb_)) {
        std::fprintf(stderr,
            "gen-texture-stars: '%s' is not a valid hex color\n",
            bgHex.c_str());
        return 1;
    }
    if (!parseHex(starHex, sr, sg, sb)) {
        std::fprintf(stderr,
            "gen-texture-stars: '%s' is not a valid hex color\n",
            starHex.c_str());
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    // Background: flat fill.
    for (int p = 0; p < W * H; ++p) {
        size_t i2 = static_cast<size_t>(p) * 3;
        pixels[i2 + 0] = br;
        pixels[i2 + 1] = bg;
        pixels[i2 + 2] = bb_;
    }
    uint32_t state = seed ? seed : 1u;
    auto next01 = [&state]() -> float {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) * (1.0f / 16777216.0f);
    };
    int starCount = static_cast<int>(W * H * density);
    int bright = 0, faint = 0;
    for (int s = 0; s < starCount; ++s) {
        int sx = static_cast<int>(next01() * W);
        int sy = static_cast<int>(next01() * H);
        // Brightness: weighted toward dim stars (most stars at
        // 30..60% blend, occasional bright at 100%). Keeps the
        // texture from looking like equally-bright pixel noise.
        float r = next01();
        float t = (r < 0.85f) ? (0.3f + r * 0.35f) : (0.85f + r * 0.15f);
        if (t > 0.7f) ++bright; else ++faint;
        size_t i2 = (static_cast<size_t>(sy) * W + sx) * 3;
        pixels[i2 + 0] = static_cast<uint8_t>(br * (1 - t) + sr * t);
        pixels[i2 + 1] = static_cast<uint8_t>(bg * (1 - t) + sg * t);
        pixels[i2 + 2] = static_cast<uint8_t>(bb_ * (1 - t) + sb * t);
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-stars: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  bg/star    : %s / %s\n",
                bgHex.c_str(), starHex.c_str());
    std::printf("  density    : %.4f\n", density);
    std::printf("  stars      : %d (%d bright, %d faint)\n",
                starCount, bright, faint);
    std::printf("  seed       : %u\n", seed);
    return 0;
}

int handleVines(int& i, int argc, char** argv) {
    // Wall with climbing vines: solid wall background plus N
    // vine paths that walk upward from the bottom edge with
    // small horizontal jitter, leaving a 2-px-wide vine trail
    // on every column they pass through.
    std::string outPath = argv[++i];
    std::string wallHex = argv[++i];
    std::string vineHex = argv[++i];
    uint32_t seed = 1;
    int vineCount = 8;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { vineCount = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        vineCount < 0 || vineCount > 256) {
        std::fprintf(stderr,
            "gen-texture-vines: invalid dims (W/H 1..8192, vineCount 0..256)\n");
        return 1;
    }
    uint8_t wr, wg, wb_, vr, vg, vb;
    if (!parseHex(wallHex, wr, wg, wb_)) {
        std::fprintf(stderr,
            "gen-texture-vines: '%s' is not a valid hex color\n",
            wallHex.c_str());
        return 1;
    }
    if (!parseHex(vineHex, vr, vg, vb)) {
        std::fprintf(stderr,
            "gen-texture-vines: '%s' is not a valid hex color\n",
            vineHex.c_str());
        return 1;
    }
    uint32_t state = seed ? seed : 1u;
    auto next01 = [&state]() -> float {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) * (1.0f / 16777216.0f);
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    // Background: flat wall color.
    for (int p = 0; p < W * H; ++p) {
        size_t i2 = static_cast<size_t>(p) * 3;
        pixels[i2 + 0] = wr;
        pixels[i2 + 1] = wg;
        pixels[i2 + 2] = wb_;
    }
    // Each vine: pick a starting x at the bottom, walk upward
    // with a small per-step horizontal drift. Set 2 pixels wide
    // on each visited row so the vine reads as a thin band rather
    // than a single-pixel line.
    int leafPixels = 0;
    for (int v = 0; v < vineCount; ++v) {
        float x = next01() * W;
        for (int y = H - 1; y >= 0; --y) {
            // Drift: cosine wave + tiny random jitter.
            x += std::cos(y * 0.08f + v * 1.7f) * 0.6f;
            x += (next01() - 0.5f) * 0.4f;
            int xi = static_cast<int>(x);
            for (int dx = 0; dx < 2; ++dx) {
                int px = xi + dx;
                if (px < 0 || px >= W) continue;
                size_t i2 = (static_cast<size_t>(y) * W + px) * 3;
                pixels[i2 + 0] = vr;
                pixels[i2 + 1] = vg;
                pixels[i2 + 2] = vb;
                ++leafPixels;
            }
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-vines: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size        : %dx%d\n", W, H);
    std::printf("  wall/vine   : %s / %s\n",
                wallHex.c_str(), vineHex.c_str());
    std::printf("  vines       : %d (%d painted pixels)\n",
                vineCount, leafPixels);
    std::printf("  seed        : %u\n", seed);
    return 0;
}

int handleMosaic(int& i, int argc, char** argv) {
    // 3-color mosaic: small square tiles randomly assigned one
    // of 3 colors, with 1-px black grout lines between them.
    // Per-tile color picked from a stable hash so the same seed
    // always yields the same mosaic.
    std::string outPath = argv[++i];
    std::string aHex = argv[++i];
    std::string bHex = argv[++i];
    std::string cHex = argv[++i];
    int tilePx = 16;
    uint32_t seed = 1;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { tilePx = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        tilePx < 4 || tilePx > 256) {
        std::fprintf(stderr,
            "gen-texture-mosaic: invalid dims (W/H 1..8192, tilePx 4..256)\n");
        return 1;
    }
    uint8_t ar, ag, ab, br, bg, bb_, cr, cg, cb;
    if (!parseHex(aHex, ar, ag, ab) ||
        !parseHex(bHex, br, bg, bb_) ||
        !parseHex(cHex, cr, cg, cb)) {
        std::fprintf(stderr,
            "gen-texture-mosaic: one of the hex colors is invalid\n");
        return 1;
    }
    auto cellPick = [seed](int cx, int cy) -> int {
        uint32_t h = static_cast<uint32_t>(cx) * 374761393u +
                     static_cast<uint32_t>(cy) * 668265263u +
                     seed * 2147483647u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h = h ^ (h >> 16);
        return h % 3;
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    int counts[3] = {0, 0, 0};
    for (int y = 0; y < H; ++y) {
        int cy = y / tilePx;
        int yInCell = y % tilePx;
        for (int x = 0; x < W; ++x) {
            int cx = x / tilePx;
            int xInCell = x % tilePx;
            // 1-px grout on the top and left edge of every cell.
            bool grout = (xInCell == 0) || (yInCell == 0);
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            if (grout) {
                pixels[i2 + 0] = 0;
                pixels[i2 + 1] = 0;
                pixels[i2 + 2] = 0;
            } else {
                int pick = cellPick(cx, cy);
                if (yInCell == 1 && xInCell == 1) ++counts[pick];
                if (pick == 0) {
                    pixels[i2 + 0] = ar; pixels[i2 + 1] = ag; pixels[i2 + 2] = ab;
                } else if (pick == 1) {
                    pixels[i2 + 0] = br; pixels[i2 + 1] = bg; pixels[i2 + 2] = bb_;
                } else {
                    pixels[i2 + 0] = cr; pixels[i2 + 1] = cg; pixels[i2 + 2] = cb;
                }
            }
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-mosaic: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  colors     : %s / %s / %s\n",
                aHex.c_str(), bHex.c_str(), cHex.c_str());
    std::printf("  tile px    : %d\n", tilePx);
    std::printf("  tile counts: A=%d B=%d C=%d\n",
                counts[0], counts[1], counts[2]);
    std::printf("  seed       : %u\n", seed);
    return 0;
}

int handleRust(int& i, int argc, char** argv) {
    // Metal with rust patches: smooth multi-octave noise field
    // thresholded by `coverage` to make rust blobs, blended
    // with the metal base. Per-pixel grain jitter on top so
    // both metal and rust regions read with subtle variation.
    std::string outPath = argv[++i];
    std::string metalHex = argv[++i];
    std::string rustHex = argv[++i];
    uint32_t seed = 1;
    float coverage = 0.4f;  // 0=clean metal, 1=fully oxidized
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { coverage = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        coverage < 0.0f || coverage > 1.0f) {
        std::fprintf(stderr,
            "gen-texture-rust: invalid dims (W/H 1..8192, coverage 0..1)\n");
        return 1;
    }
    uint8_t mr, mg, mb, rr, rg, rb;
    if (!parseHex(metalHex, mr, mg, mb)) {
        std::fprintf(stderr,
            "gen-texture-rust: '%s' is not a valid hex color\n",
            metalHex.c_str());
        return 1;
    }
    if (!parseHex(rustHex, rr, rg, rb)) {
        std::fprintf(stderr,
            "gen-texture-rust: '%s' is not a valid hex color\n",
            rustHex.c_str());
        return 1;
    }
    uint32_t state = seed ? seed : 1u;
    auto next01 = [&state]() -> float {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) * (1.0f / 16777216.0f);
    };
    float seedF = static_cast<float>(seed);
    auto blob = [&](float x, float y) -> float {
        // 3-octave smooth noise; sin/cos product avoids needing
        // a permutation table.
        float n = 0.0f, total = 0.0f;
        float freq = 0.025f, amp = 1.0f;
        for (int o = 0; o < 3; ++o) {
            n += amp * (0.5f + 0.5f *
                std::sin(x * freq + seedF * (1.0f + o)) *
                std::cos(y * freq + seedF * (0.6f + o)));
            total += amp;
            freq *= 2.0f;
            amp *= 0.5f;
        }
        return n / total;  // 0..1
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    float thresh = 1.0f - coverage;
    int rustPixels = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float n = blob(static_cast<float>(x), static_cast<float>(y));
            // Smoothstep across a 0.12 band around threshold so
            // rust patches feather into clean metal.
            float t = std::clamp((n - thresh) / 0.12f, 0.0f, 1.0f);
            if (t > 0.5f) ++rustPixels;
            // Per-pixel grain jitter (separate small jitter on
            // each channel) so neither material reads as flat.
            float jitter = (next01() - 0.5f) * 0.08f;
            float r = (mr * (1 - t) + rr * t) * (1.0f + jitter);
            float g = (mg * (1 - t) + rg * t) * (1.0f + jitter);
            float b = (mb * (1 - t) + rb * t) * (1.0f + jitter);
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
            pixels[i2 + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
            pixels[i2 + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-rust: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  metal/rust : %s / %s\n",
                metalHex.c_str(), rustHex.c_str());
    std::printf("  coverage   : %.2f (%d rust pixels)\n",
                coverage, rustPixels);
    std::printf("  seed       : %u\n", seed);
    return 0;
}

int handleCircuit(int& i, int argc, char** argv) {
    // Sci-fi circuit board: solid PCB background plus N traces
    // that walk the surface in orthogonal Manhattan style — each
    // trace alternates random horizontal + vertical segments,
    // mimicking right-angle PCB routing. Each segment endpoint
    // gets a "via" dot (3×3 block) so the routing reads as
    // intentional rather than random scribbles.
    std::string outPath = argv[++i];
    std::string pcbHex = argv[++i];
    std::string traceHex = argv[++i];
    uint32_t seed = 1;
    int traceCount = 24;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { traceCount = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        traceCount < 0 || traceCount > 1024) {
        std::fprintf(stderr,
            "gen-texture-circuit: invalid dims (W/H 1..8192, traceCount 0..1024)\n");
        return 1;
    }
    uint8_t pr, pg, pb, tr, tg, tb;
    if (!parseHex(pcbHex, pr, pg, pb)) {
        std::fprintf(stderr,
            "gen-texture-circuit: '%s' is not a valid hex color\n",
            pcbHex.c_str());
        return 1;
    }
    if (!parseHex(traceHex, tr, tg, tb)) {
        std::fprintf(stderr,
            "gen-texture-circuit: '%s' is not a valid hex color\n",
            traceHex.c_str());
        return 1;
    }
    uint32_t state = seed ? seed : 1u;
    auto next01 = [&state]() -> float {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) * (1.0f / 16777216.0f);
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    // Background fill
    for (int p = 0; p < W * H; ++p) {
        size_t i2 = static_cast<size_t>(p) * 3;
        pixels[i2 + 0] = pr;
        pixels[i2 + 1] = pg;
        pixels[i2 + 2] = pb;
    }
    auto setPx = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
        pixels[i2 + 0] = tr;
        pixels[i2 + 1] = tg;
        pixels[i2 + 2] = tb;
    };
    auto setVia = [&](int x, int y) {
        // 3×3 dot for vias / segment joints.
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                setPx(x + dx, y + dy);
    };
    int viaCount = 0;
    for (int t = 0; t < traceCount; ++t) {
        int x = static_cast<int>(next01() * W);
        int y = static_cast<int>(next01() * H);
        // Each trace runs 3-6 segments
        int segs = 3 + static_cast<int>(next01() * 4);
        bool horiz = next01() < 0.5f;
        for (int s = 0; s < segs; ++s) {
            int len = 8 + static_cast<int>(next01() * 24);
            int dir = (next01() < 0.5f) ? 1 : -1;
            int nx = x, ny = y;
            for (int k = 0; k < len; ++k) {
                if (horiz) nx += dir;
                else ny += dir;
                setPx(nx, ny);
            }
            x = nx; y = ny;
            setVia(x, y);  // joint at the corner
            ++viaCount;
            horiz = !horiz;  // alternate axis
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-circuit: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  pcb/trace  : %s / %s\n",
                pcbHex.c_str(), traceHex.c_str());
    std::printf("  traces     : %d (~%d vias)\n", traceCount, viaCount);
    std::printf("  seed       : %u\n", seed);
    return 0;
}

int handleCoral(int& i, int argc, char** argv) {
    // Coral reef: water-color background plus N branching tree
    // shapes that grow from random anchor points. Each branch
    // walks a curved path (random angle drift), splitting into
    // 2-3 sub-branches at random intervals so the result reads
    // as organic coral rather than straight lines.
    std::string outPath = argv[++i];
    std::string waterHex = argv[++i];
    std::string coralHex = argv[++i];
    uint32_t seed = 1;
    int branchCount = 12;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { branchCount = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        branchCount < 0 || branchCount > 1024) {
        std::fprintf(stderr,
            "gen-texture-coral: invalid dims (W/H 1..8192, branchCount 0..1024)\n");
        return 1;
    }
    uint8_t wr, wg, wb_, cr, cg, cb;
    if (!parseHex(waterHex, wr, wg, wb_)) {
        std::fprintf(stderr,
            "gen-texture-coral: '%s' is not a valid hex color\n",
            waterHex.c_str());
        return 1;
    }
    if (!parseHex(coralHex, cr, cg, cb)) {
        std::fprintf(stderr,
            "gen-texture-coral: '%s' is not a valid hex color\n",
            coralHex.c_str());
        return 1;
    }
    uint32_t state = seed ? seed : 1u;
    auto next01 = [&state]() -> float {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) * (1.0f / 16777216.0f);
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int p = 0; p < W * H; ++p) {
        size_t i2 = static_cast<size_t>(p) * 3;
        pixels[i2 + 0] = wr; pixels[i2 + 1] = wg; pixels[i2 + 2] = wb_;
    }
    auto setPx = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
        pixels[i2 + 0] = cr; pixels[i2 + 1] = cg; pixels[i2 + 2] = cb;
    };
    // Recursive branch growth via explicit stack (no real
    // recursion to avoid blowing through stack for deep splits).
    struct Branch { float x, y, angle, length, thickness; };
    std::vector<Branch> stack;
    int totalBranches = 0;
    for (int b = 0; b < branchCount; ++b) {
        // Anchor at the bottom edge, growing upward
        Branch root;
        root.x = next01() * W;
        root.y = H - 1;
        root.angle = -3.14159f * 0.5f + (next01() - 0.5f) * 0.6f;
        root.length = 30 + next01() * 40;
        root.thickness = 2.0f;
        stack.push_back(root);
        while (!stack.empty()) {
            Branch br = stack.back();
            stack.pop_back();
            ++totalBranches;
            float x = br.x, y = br.y;
            int steps = static_cast<int>(br.length);
            for (int s = 0; s < steps; ++s) {
                // Random walk + slight upward bias
                br.angle += (next01() - 0.5f) * 0.15f;
                x += std::cos(br.angle);
                y += std::sin(br.angle);
                int rad = static_cast<int>(std::ceil(br.thickness));
                for (int dy = -rad; dy <= rad; ++dy) {
                    for (int dx = -rad; dx <= rad; ++dx) {
                        if (dx*dx + dy*dy > rad*rad) continue;
                        setPx(static_cast<int>(x) + dx,
                              static_cast<int>(y) + dy);
                    }
                }
                // Split occasionally
                if (next01() < 0.05f && br.thickness > 1.0f) {
                    Branch child;
                    child.x = x; child.y = y;
                    child.angle = br.angle + (next01() - 0.5f) * 1.2f;
                    child.length = br.length * (0.4f + next01() * 0.3f);
                    child.thickness = br.thickness * 0.7f;
                    if (stack.size() < 256) stack.push_back(child);
                }
            }
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-coral: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size        : %dx%d\n", W, H);
    std::printf("  water/coral : %s / %s\n",
                waterHex.c_str(), coralHex.c_str());
    std::printf("  branches    : %d roots → %d total (with splits)\n",
                branchCount, totalBranches);
    std::printf("  seed        : %u\n", seed);
    return 0;
}

int handleFlame(int& i, int argc, char** argv) {
    // Flame: vertical color gradient from dark hex at the
    // bottom to hot hex at the top, mixed with smooth noise
    // flicker so the boundary between hot and dark wavers
    // randomly. Reads as a flame seen from a distance.
    std::string outPath = argv[++i];
    std::string darkHex = argv[++i];
    std::string hotHex = argv[++i];
    uint32_t seed = 1;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192) {
        std::fprintf(stderr,
            "gen-texture-flame: invalid dims (W/H 1..8192)\n");
        return 1;
    }
    uint8_t dr, dg, db, hr, hg, hb;
    if (!parseHex(darkHex, dr, dg, db)) {
        std::fprintf(stderr,
            "gen-texture-flame: '%s' is not a valid hex color\n",
            darkHex.c_str());
        return 1;
    }
    if (!parseHex(hotHex, hr, hg, hb)) {
        std::fprintf(stderr,
            "gen-texture-flame: '%s' is not a valid hex color\n",
            hotHex.c_str());
        return 1;
    }
    float seedF = static_cast<float>(seed);
    auto noise = [&](float x, float y) -> float {
        // Multi-octave smooth noise; lower freq dominates.
        float n = 0.0f, total = 0.0f;
        float freq = 0.04f, amp = 1.0f;
        for (int o = 0; o < 3; ++o) {
            n += amp * (0.5f + 0.5f *
                std::sin(x * freq + seedF * (1.0f + o)) *
                std::cos(y * freq + seedF * (0.5f + o)));
            total += amp;
            freq *= 2.0f;
            amp *= 0.5f;
        }
        return n / total;
    };
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        // Vertical position: 0 at bottom (dark), 1 at top (hot).
        float vy = static_cast<float>(H - 1 - y) / (H - 1);
        for (int x = 0; x < W; ++x) {
            // Add wavy flicker via noise so the gradient boundary
            // isn't a clean horizontal line.
            float n = noise(static_cast<float>(x), static_cast<float>(y));
            float t = std::clamp(vy + (n - 0.5f) * 0.4f, 0.0f, 1.0f);
            // Curve so the bottom stays dark longer and the top
            // saturates faster (real flames are mostly dark with
            // a bright core/tip).
            t = t * t;
            uint8_t r = static_cast<uint8_t>(dr * (1 - t) + hr * t);
            uint8_t g = static_cast<uint8_t>(dg * (1 - t) + hg * t);
            uint8_t b = static_cast<uint8_t>(db * (1 - t) + hb * t);
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = r;
            pixels[i2 + 1] = g;
            pixels[i2 + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-flame: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  dark/hot   : %s / %s\n",
                darkHex.c_str(), hotHex.c_str());
    std::printf("  seed       : %u\n", seed);
    return 0;
}

int handleTartan(int& i, int argc, char** argv) {
    // Tartan plaid: 3-color crossing band pattern. Each cell
    // belongs to one of 6 logical zones (3 vertical + 3
    // horizontal bands per repeat unit) and the displayed
    // color is the additive mix of the band's vertical and
    // horizontal contributions — produces the characteristic
    // overlap diamond grid of Scottish tartans.
    std::string outPath = argv[++i];
    std::string aHex = argv[++i];
    std::string bHex = argv[++i];
    std::string cHex = argv[++i];
    int bandPx = 32;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { bandPx = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        bandPx < 4 || bandPx > 256) {
        std::fprintf(stderr,
            "gen-texture-tartan: invalid dims (W/H 1..8192, bandPx 4..256)\n");
        return 1;
    }
    uint8_t ar, ag, ab, br, bg, bb_, cr_, cg_, cb_;
    if (!parseHex(aHex, ar, ag, ab) ||
        !parseHex(bHex, br, bg, bb_) ||
        !parseHex(cHex, cr_, cg_, cb_)) {
        std::fprintf(stderr,
            "gen-texture-tartan: one of the hex colors is invalid\n");
        return 1;
    }
    // 3-band repeat: A wide, B narrow, C medium. Repeat is
    // 6 × bandPx wide. Each band weight is constant within
    // its slice; the displayed pixel color is averaged from
    // the vertical band (column) and horizontal band (row).
    auto bandColor = [&](int t) -> std::tuple<uint8_t, uint8_t, uint8_t> {
        // t is position modulo (6 * bandPx). Map to one of A/B/C
        // based on which segment t falls in.
        int slice = (t / bandPx) % 6;
        // 6-slice repeat pattern: A A B C C B (gives a typical
        // tartan look — wide A blocks separated by thin B/C lines).
        switch (slice) {
            case 0: case 1: return {ar, ag, ab};
            case 2:         return {br, bg, bb_};
            case 3: case 4: return {cr_, cg_, cb_};
            default:        return {br, bg, bb_};
        }
    };
    int repeat = 6 * bandPx;
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        int yMod = ((y % repeat) + repeat) % repeat;
        auto [hr, hg, hb] = bandColor(yMod);
        for (int x = 0; x < W; ++x) {
            int xMod = ((x % repeat) + repeat) % repeat;
            auto [vr, vg, vb] = bandColor(xMod);
            // Average the horizontal-band and vertical-band
            // colors. At intersections the average produces a
            // distinct mid-tone that creates the diamond grid
            // characteristic of plaid.
            uint8_t r = static_cast<uint8_t>((hr + vr) / 2);
            uint8_t g = static_cast<uint8_t>((hg + vg) / 2);
            uint8_t b = static_cast<uint8_t>((hb + vb) / 2);
            size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
            pixels[i2 + 0] = r;
            pixels[i2 + 1] = g;
            pixels[i2 + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-tartan: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  colors A/B/C: %s / %s / %s\n",
                aHex.c_str(), bHex.c_str(), cHex.c_str());
    std::printf("  band px    : %d (repeat %d px)\n",
                bandPx, repeat);
    return 0;
}

int handleArgyle(int& i, int argc, char** argv) {
    // Argyle: classic sweater-knit pattern of rotated squares
    // (lozenges) in checkerboard alternation, overlaid with
    // diagonal stitch lines in a third color. The rotation is
    // achieved by working in the rotated coord system (u, v) =
    // (x + y, x - y); each tile becomes a unit cell there.
    std::string outPath = argv[++i];
    std::string aHex = argv[++i];
    std::string bHex = argv[++i];
    std::string stitchHex = argv[++i];
    int cellPx = 64;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { cellPx = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        cellPx < 8 || cellPx > 512) {
        std::fprintf(stderr,
            "gen-texture-argyle: invalid dims (W/H 1..8192, cellPx 8..512)\n");
        return 1;
    }
    uint8_t ar, ag, ab, br, bg, bb_, sr_, sg_, sb_;
    if (!parseHex(aHex, ar, ag, ab) ||
        !parseHex(bHex, br, bg, bb_) ||
        !parseHex(stitchHex, sr_, sg_, sb_)) {
        std::fprintf(stderr,
            "gen-texture-argyle: one of the hex colors is invalid\n");
        return 1;
    }
    // Stitch lines are 2 pixels wide regardless of cell size — at
    // very small cells they'd dominate, but cellPx>=8 keeps them
    // visually subordinate to the diamond fill.
    const int stitchHalfWidth = 1;
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // Rotate the lattice 45° by mapping to (u, v) = (x+y, x-y).
            // Lozenge cells in the original frame become axis-aligned
            // squares in (u, v) space, easy to checkerboard.
            int u = x + y;
            int v = x - y;
            int uCell = u / cellPx;
            int vCell;
            // Floor division for negative v so the lattice stays
            // consistent across the whole image (avoids a seam at x<y).
            if (v >= 0) vCell = v / cellPx;
            else        vCell = -((-v + cellPx - 1) / cellPx);
            uint8_t r, g, b;
            if (((uCell + vCell) & 1) == 0) {
                r = ar; g = ag; b = ab;
            } else {
                r = br; g = bg; b = bb_;
            }
            // Stitch lines: 2-px-wide bands along the lattice grid
            // (i.e. at u % cellPx ≈ 0 and v % cellPx ≈ 0). These are
            // the diagonal lines characteristic of the argyle look.
            int uMod = ((u % cellPx) + cellPx) % cellPx;
            int vMod = ((v % cellPx) + cellPx) % cellPx;
            bool onStitch =
                (uMod <= stitchHalfWidth || uMod >= cellPx - stitchHalfWidth) ||
                (vMod <= stitchHalfWidth || vMod >= cellPx - stitchHalfWidth);
            if (onStitch) {
                r = sr_; g = sg_; b = sb_;
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-argyle: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  colors A/B  : %s / %s\n", aHex.c_str(), bHex.c_str());
    std::printf("  stitch     : %s\n", stitchHex.c_str());
    std::printf("  cell px    : %d\n", cellPx);
    return 0;
}

int handleHerringbone(int& i, int argc, char** argv) {
    // Herringbone (chevron-style): horizontal strips of slanted
    // parallel lines whose slant direction flips every strip,
    // producing the V-shaped "fish bone" pattern that's the
    // hallmark of herringbone fabric and parquet flooring.
    // Implemented as a per-pixel shear: shifting x by the row's
    // local-y collapses each diagonal line into a vertical band
    // in shifted-x space, where modular arithmetic picks line vs
    // background.
    std::string outPath = argv[++i];
    std::string bgHex = argv[++i];
    std::string lineHex = argv[++i];
    int stripHeight  = 32;   // height of each constant-direction strip
    int lineSpacing  = 12;   // distance between adjacent lines along x
    int lineWidth    = 4;    // line thickness in shifted-x coords
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { stripHeight = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { lineSpacing = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { lineWidth = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        stripHeight < 4 || stripHeight > 256 ||
        lineSpacing < 4 || lineSpacing > 256 ||
        lineWidth < 1 || lineWidth >= lineSpacing) {
        std::fprintf(stderr,
            "gen-texture-herringbone: invalid dims (W/H 1..8192, stripH 4..256, "
            "spacing 4..256, lineW 1..spacing-1)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, lr_, lg_, lb_;
    if (!parseHex(bgHex, br_, bg_, bb_) ||
        !parseHex(lineHex, lr_, lg_, lb_)) {
        std::fprintf(stderr,
            "gen-texture-herringbone: bg or line hex color is invalid\n");
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        int rowOfStrips = y / stripHeight;
        int withinStrip = y - rowOfStrips * stripHeight;
        // Even strips: lines slant down-right (+45°-ish, scaled by
        // stripHeight/lineSpacing). Odd strips: slant down-left.
        int sign = (rowOfStrips & 1) ? -1 : 1;
        for (int x = 0; x < W; ++x) {
            // Shift x by ±withinStrip — collapses the slanted line
            // into a vertical strip in shifted-x coords.
            int shifted = x + sign * withinStrip;
            int phase = ((shifted % lineSpacing) + lineSpacing) % lineSpacing;
            uint8_t r, g, b;
            if (phase < lineWidth) {
                r = lr_; g = lg_; b = lb_;
            } else {
                r = br_; g = bg_; b = bb_;
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-herringbone: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  bg / line  : %s / %s\n", bgHex.c_str(), lineHex.c_str());
    std::printf("  strip H    : %d (slant flips per strip)\n", stripHeight);
    std::printf("  line       : width %d / spacing %d\n",
                lineWidth, lineSpacing);
    return 0;
}

int handleScales(int& i, int argc, char** argv) {
    // Scales: fish / dragon / chain mail pattern. Each scale is a
    // circle whose center sits at the bottom-center of a cell;
    // adjacent rows are offset by half a cell width so the
    // circles interlock into the classic overlapping-scale look.
    // Three colors: background (gaps), scale body, and a rim
    // highlight near the top of each scale that gives the
    // armoured/raised appearance.
    std::string outPath = argv[++i];
    std::string bgHex = argv[++i];
    std::string scaleHex = argv[++i];
    std::string rimHex = argv[++i];
    int cellW = 24;
    int cellH = 16;   // shorter than wide for natural overlap
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { cellW = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { cellH = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        cellW < 4 || cellW > 256 ||
        cellH < 4 || cellH > 256) {
        std::fprintf(stderr,
            "gen-texture-scales: invalid dims (W/H 1..8192, cellW/H 4..256)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, sr_, sg_, sb_, rr_, rg_, rb_;
    if (!parseHex(bgHex, br_, bg_, bb_) ||
        !parseHex(scaleHex, sr_, sg_, sb_) ||
        !parseHex(rimHex, rr_, rg_, rb_)) {
        std::fprintf(stderr,
            "gen-texture-scales: bg, scale, or rim hex color is invalid\n");
        return 1;
    }
    // Scale radius is 55% of cell width so adjacent scales in the
    // same row touch + slightly overlap, and rows interlock cleanly
    // through the half-row stagger.
    float scaleR = cellW * 0.55f;
    float scaleR2 = scaleR * scaleR;
    // Rim threshold: top 25% of each scale gets the rim color.
    float rimNormY = 0.55f;
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        int rowIdx = y / cellH;
        int shift  = (rowIdx & 1) ? cellW / 2 : 0;
        for (int x = 0; x < W; ++x) {
            // Snap x into the current row's lattice (with stagger).
            // Use floor-div semantics that work for x near 0.
            int xRel = x - shift;
            int col;
            if (xRel >= 0) col = xRel / cellW;
            else           col = -((-xRel + cellW - 1) / cellW);
            // Scale center: bottom-middle of the cell.
            float cx = col * cellW + shift + cellW * 0.5f;
            float cy = rowIdx * cellH + cellH;
            float dx = x - cx;
            float dy = y - cy;
            float distSq = dx * dx + dy * dy;
            uint8_t r, g, b;
            if (distSq < scaleR2) {
                // Inside a scale. -dy/R is 0 at center, ~1 at top.
                float normY = -dy / scaleR;
                if (normY > rimNormY) {
                    r = rr_; g = rg_; b = rb_;
                } else {
                    r = sr_; g = sg_; b = sb_;
                }
            } else {
                r = br_; g = bg_; b = bb_;
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-scales: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  bg/scale/rim : %s / %s / %s\n",
                bgHex.c_str(), scaleHex.c_str(), rimHex.c_str());
    std::printf("  cell       : %dx%d (radius %.1f, half-row stagger)\n",
                cellW, cellH, scaleR);
    return 0;
}

int handleStainedGlass(int& i, int argc, char** argv) {
    // Stained glass: Voronoi-cell pattern with dark lead lines
    // separating colored regions. Each pixel is classified by
    // which seed point it's closest to; pixels near a cell
    // boundary (small relative gap to the second-nearest seed)
    // become the lead color, producing the leaded-glass look.
    // Three stained colors cycle across cells (cellIdx % 3) for
    // a balanced palette without per-cell color authoring.
    std::string outPath = argv[++i];
    std::string leadHex  = argv[++i];
    std::string aHex     = argv[++i];
    std::string bHex     = argv[++i];
    std::string cHex     = argv[++i];
    int cellCount = 32;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { cellCount = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        cellCount < 4 || cellCount > 1024) {
        std::fprintf(stderr,
            "gen-texture-stained-glass: invalid dims (W/H 1..8192, cells 4..1024)\n");
        return 1;
    }
    uint8_t lr_, lg_, lb_, ar, ag, ab, br, bg, bb_, cr_, cg_, cb_;
    if (!parseHex(leadHex, lr_, lg_, lb_) ||
        !parseHex(aHex, ar, ag, ab) ||
        !parseHex(bHex, br, bg, bb_) ||
        !parseHex(cHex, cr_, cg_, cb_)) {
        std::fprintf(stderr,
            "gen-texture-stained-glass: one of the hex colors is invalid\n");
        return 1;
    }
    // Deterministic seed placement — same image dimensions and
    // cellCount always yield the same cells, so re-running the
    // command reproduces previous output exactly.
    struct Seed { float x, y; int colorIdx; };
    std::vector<Seed> seeds;
    seeds.reserve(cellCount);
    uint32_t rng = static_cast<uint32_t>(cellCount) * 0x9E3779B9u +
                   static_cast<uint32_t>(W) * 0x85EBCA6Bu;
    auto rngStep = [&]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    };
    for (int s = 0; s < cellCount; ++s) {
        Seed sd;
        sd.x = (rngStep() & 0xFFFF) / 65535.0f * W;
        sd.y = (rngStep() & 0xFFFF) / 65535.0f * H;
        sd.colorIdx = s % 3;
        seeds.push_back(sd);
    }
    // Lead-line threshold: pixels where dist2/dist1 < threshold
    // are within the boundary band. 1.08 gives ~3-4 px lead
    // lines at 256x256 with 32 cells — readable but not heavy.
    const float boundaryRatio = 1.08f;
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float fx = static_cast<float>(x);
            float fy = static_cast<float>(y);
            // Track best two distances so we can detect cell
            // boundaries by the dist2/dist1 ratio.
            float bestSq = 1e30f, secondSq = 1e30f;
            int bestIdx = 0;
            for (int s = 0; s < cellCount; ++s) {
                float dx = seeds[s].x - fx;
                float dy = seeds[s].y - fy;
                float d2 = dx * dx + dy * dy;
                if (d2 < bestSq) {
                    secondSq = bestSq;
                    bestSq = d2;
                    bestIdx = s;
                } else if (d2 < secondSq) {
                    secondSq = d2;
                }
            }
            uint8_t r, g, b;
            // sqrt comparison via ratio of squared distances
            // works because boundaryRatio^2 is what we compare.
            float ratioSq = (bestSq > 0.0f) ? secondSq / bestSq : 1e30f;
            if (ratioSq < boundaryRatio * boundaryRatio) {
                r = lr_; g = lg_; b = lb_;
            } else {
                int ci = seeds[bestIdx].colorIdx;
                if      (ci == 0) { r = ar;  g = ag;  b = ab;  }
                else if (ci == 1) { r = br;  g = bg;  b = bb_; }
                else              { r = cr_; g = cg_; b = cb_; }
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-stained-glass: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  lead       : %s\n", leadHex.c_str());
    std::printf("  glass A/B/C: %s / %s / %s\n",
                aHex.c_str(), bHex.c_str(), cHex.c_str());
    std::printf("  cells      : %d (Voronoi)\n", cellCount);
    return 0;
}

int handleShingles(int& i, int argc, char** argv) {
    // Roof shingles: offset rows of rectangular tiles, with a
    // dark shadow band at the top of each row (where the row
    // above overlaps) and thin vertical seams between adjacent
    // shingles in a row. Three colors give the shingle body
    // its base tone, a shadow tone for the overlap band, and
    // a darker seam color.
    std::string outPath = argv[++i];
    std::string baseHex   = argv[++i];
    std::string shadowHex = argv[++i];
    std::string seamHex   = argv[++i];
    int shingleW = 32;
    int shingleH = 24;
    int shadowH  = 4;     // shadow band thickness at top of each row
    int seamW    = 1;     // vertical seam width between shingles
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { shingleW = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { shingleH = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { shadowH = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        shingleW < 4 || shingleW > 512 ||
        shingleH < 4 || shingleH > 512 ||
        shadowH < 0 || shadowH >= shingleH) {
        std::fprintf(stderr,
            "gen-texture-shingles: invalid dims (W/H 1..8192, shingleW/H 4..512, shadowH 0..shingleH-1)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, sr_, sg_, sb_, er_, eg_, eb_;
    if (!parseHex(baseHex, br_, bg_, bb_) ||
        !parseHex(shadowHex, sr_, sg_, sb_) ||
        !parseHex(seamHex, er_, eg_, eb_)) {
        std::fprintf(stderr,
            "gen-texture-shingles: base/shadow/seam hex color is invalid\n");
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        int rowIdx = y / shingleH;
        int withinRow = y - rowIdx * shingleH;
        int shift  = (rowIdx & 1) ? shingleW / 2 : 0;
        for (int x = 0; x < W; ++x) {
            // Position within the current shingle along x.
            int xRel = x - shift;
            int xMod;
            if (xRel >= 0) xMod = xRel % shingleW;
            else           xMod = ((xRel % shingleW) + shingleW) % shingleW;
            uint8_t r, g, b;
            if (withinRow < shadowH) {
                // Top of row: shadow band where the row above
                // overlaps this row's shingles.
                r = sr_; g = sg_; b = sb_;
            } else if (xMod < seamW || xMod >= shingleW - seamW) {
                // Vertical seam between adjacent shingles.
                r = er_; g = eg_; b = eb_;
            } else {
                r = br_; g = bg_; b = bb_;
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-shingles: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  base/shadow/seam: %s / %s / %s\n",
                baseHex.c_str(), shadowHex.c_str(), seamHex.c_str());
    std::printf("  shingle    : %dx%d (shadow %d px, seam %d px)\n",
                shingleW, shingleH, shadowH, seamW);
    return 0;
}

int handleFrost(int& i, int argc, char** argv) {
    // Frost: scattered crystal nuclei with radial spikes.
    // Each seed gets six thin lines radiating at 60° intervals
    // (with a per-seed random angular offset so they don't all
    // align). Line lengths are jittered per spike, and pixel
    // intensity falls off linearly toward the end of each line
    // so spikes fade naturally into the background.
    std::string outPath = argv[++i];
    std::string bgHex  = argv[++i];
    std::string iceHex = argv[++i];
    int seedCount = 80;
    int rayLen    = 18;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seedCount = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { rayLen = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        seedCount < 1 || seedCount > 8192 ||
        rayLen < 2 || rayLen > 256) {
        std::fprintf(stderr,
            "gen-texture-frost: invalid dims (W/H 1..8192, seeds 1..8192, ray 2..256)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, ir, ig, ib;
    if (!parseHex(bgHex, br_, bg_, bb_) ||
        !parseHex(iceHex, ir, ig, ib)) {
        std::fprintf(stderr,
            "gen-texture-frost: bg or ice hex color is invalid\n");
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    // Fill background.
    for (size_t p = 0; p < pixels.size(); p += 3) {
        pixels[p + 0] = br_;
        pixels[p + 1] = bg_;
        pixels[p + 2] = bb_;
    }
    // Deterministic RNG so re-runs reproduce the same frost.
    uint32_t rng = static_cast<uint32_t>(seedCount) * 0x9E3779B9u +
                   static_cast<uint32_t>(W) * 0x85EBCA6Bu +
                   static_cast<uint32_t>(rayLen);
    auto rngStep = [&]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    };
    auto blendPixel = [&](int x, int y, float alpha) {
        if (x < 0 || x >= W || y < 0 || y >= H) return;
        if (alpha <= 0) return;
        if (alpha > 1.0f) alpha = 1.0f;
        size_t idx = (static_cast<size_t>(y) * W + x) * 3;
        // Linear blend from bg toward ice color by alpha.
        pixels[idx + 0] = static_cast<uint8_t>(
            pixels[idx + 0] + (ir - pixels[idx + 0]) * alpha);
        pixels[idx + 1] = static_cast<uint8_t>(
            pixels[idx + 1] + (ig - pixels[idx + 1]) * alpha);
        pixels[idx + 2] = static_cast<uint8_t>(
            pixels[idx + 2] + (ib - pixels[idx + 2]) * alpha);
    };
    constexpr float kPi = 3.14159265358979323846f;
    for (int s = 0; s < seedCount; ++s) {
        // Seed position uniformly random across the image.
        float sx = (rngStep() & 0xFFFF) / 65535.0f * W;
        float sy = (rngStep() & 0xFFFF) / 65535.0f * H;
        // Angular jitter so spikes don't all align to the same
        // 6-fold rosette.
        float baseAngle = (rngStep() & 0xFFFF) / 65535.0f * kPi / 3.0f;
        // 6 rays per nucleus at 60° spacing.
        for (int r = 0; r < 6; ++r) {
            float angle = baseAngle + r * (kPi / 3.0f);
            float dx = std::cos(angle);
            float dy = std::sin(angle);
            // Per-spike length jitter (60-100% of nominal).
            float lenScale = 0.6f + (rngStep() & 0xFFFF) / 65535.0f * 0.4f;
            int spikeLen = static_cast<int>(rayLen * lenScale);
            // Walk pixels along the ray. Alpha falls linearly
            // from 1.0 at the seed to 0.0 at the end of the spike.
            for (int t = 0; t < spikeLen; ++t) {
                int px = static_cast<int>(sx + dx * t);
                int py = static_cast<int>(sy + dy * t);
                float alpha = 1.0f - static_cast<float>(t) / spikeLen;
                blendPixel(px, py, alpha);
            }
        }
        // Bright nucleus dot — a 2x2 block to make the seed
        // visible even when its spikes are short.
        for (int dyN = 0; dyN < 2; ++dyN) {
            for (int dxN = 0; dxN < 2; ++dxN) {
                int px = static_cast<int>(sx) + dxN;
                int py = static_cast<int>(sy) + dyN;
                blendPixel(px, py, 1.0f);
            }
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-frost: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  bg / ice   : %s / %s\n", bgHex.c_str(), iceHex.c_str());
    std::printf("  seeds      : %d (6-spike rosettes, ray %d px)\n",
                seedCount, rayLen);
    return 0;
}

int handleParquet(int& i, int argc, char** argv) {
    // Parquet: basket-weave wood floor pattern. The image is
    // tiled with 2N x 2N cells; cells alternate orientation in
    // a checkerboard so half are split into 2 horizontal planks
    // and half into 2 vertical planks. Two wood colors (one
    // per orientation) make the basket-weave structure pop;
    // a third "gap" color paints thin lines along plank edges
    // for the inset / wood-joint look.
    std::string outPath = argv[++i];
    std::string woodAHex = argv[++i];
    std::string woodBHex = argv[++i];
    std::string gapHex   = argv[++i];
    int cellSize = 32;   // cell side = 2N; each plank is N wide
    int gapW     = 1;    // gap line thickness between planks
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { cellSize = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { gapW = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        cellSize < 8 || cellSize > 512 ||
        gapW < 0 || gapW * 4 >= cellSize) {
        std::fprintf(stderr,
            "gen-texture-parquet: invalid dims (W/H 1..8192, cellSize 8..512, gap 0..cellSize/4)\n");
        return 1;
    }
    uint8_t ar, ag, ab, br, bg, bb_, gr, gg, gb_;
    if (!parseHex(woodAHex, ar, ag, ab) ||
        !parseHex(woodBHex, br, bg, bb_) ||
        !parseHex(gapHex,   gr, gg, gb_)) {
        std::fprintf(stderr,
            "gen-texture-parquet: woodA/woodB/gap hex color is invalid\n");
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        // Use floor division for negative coords (just-in-case
        // future callers pass tile-offset images).
        int cellY = y / cellSize;
        int yMod  = y - cellY * cellSize;
        for (int x = 0; x < W; ++x) {
            int cellX = x / cellSize;
            int xMod  = x - cellX * cellSize;
            // Checkerboard: (cellX + cellY) even -> horizontal
            // planks (long axis along X, two stacked vertically);
            // odd -> vertical planks (long axis along Y, two
            // side by side).
            bool horizontalPair = (((cellX + cellY) & 1) == 0);
            uint8_t r, g, b;
            if (horizontalPair) {
                // 2 horizontal planks: top half = wood A, bottom
                // half = wood A (same color since the orientation
                // is what matters for the weave). Gap line at the
                // midline between the two planks, plus around the
                // cell perimeter.
                bool inMidline = (yMod >= cellSize / 2 - gapW &&
                                   yMod <  cellSize / 2 + gapW);
                bool onCellEdge = (yMod < gapW || yMod >= cellSize - gapW ||
                                    xMod < gapW || xMod >= cellSize - gapW);
                if (inMidline || onCellEdge) {
                    r = gr; g = gg; b = gb_;
                } else {
                    r = ar; g = ag; b = ab;
                }
            } else {
                // 2 vertical planks: left half + right half (wood B).
                // Gap line at the vertical midline.
                bool inMidline = (xMod >= cellSize / 2 - gapW &&
                                   xMod <  cellSize / 2 + gapW);
                bool onCellEdge = (yMod < gapW || yMod >= cellSize - gapW ||
                                    xMod < gapW || xMod >= cellSize - gapW);
                if (inMidline || onCellEdge) {
                    r = gr; g = gg; b = gb_;
                } else {
                    r = br; g = bg; b = bb_;
                }
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-parquet: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  wood A/B   : %s / %s (%s gap)\n",
                woodAHex.c_str(), woodBHex.c_str(), gapHex.c_str());
    std::printf("  cell       : %d px (gap %d px, basket-weave)\n",
                cellSize, gapW);
    return 0;
}

int handleBubbles(int& i, int argc, char** argv) {
    // Bubbles: scattered circles of varied radii, drawn as
    // translucent fills with a brighter rim. Bubbles overlap;
    // rim color wins at any pixel that lies in any bubble's
    // ring band (so overlapping outlines stay readable).
    std::string outPath = argv[++i];
    std::string bgHex   = argv[++i];
    std::string fillHex = argv[++i];
    std::string rimHex  = argv[++i];
    int bubbleCount = 50;
    int minR = 6;
    int maxR = 24;
    int rimW = 2;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { bubbleCount = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { minR = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { maxR = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { rimW = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        bubbleCount < 1 || bubbleCount > 4096 ||
        minR < 1 || maxR < minR || maxR > 1024 ||
        rimW < 1 || rimW > minR) {
        std::fprintf(stderr,
            "gen-texture-bubbles: invalid dims (W/H 1..8192, bubbles 1..4096, "
            "minR..maxR 1..1024, rimW 1..minR)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, fr, fg, fb_, rr, rg, rb_;
    if (!parseHex(bgHex, br_, bg_, bb_) ||
        !parseHex(fillHex, fr, fg, fb_) ||
        !parseHex(rimHex, rr, rg, rb_)) {
        std::fprintf(stderr,
            "gen-texture-bubbles: bg/fill/rim hex color is invalid\n");
        return 1;
    }
    // Deterministic seed placement so re-runs reproduce.
    struct Bubble { int x, y, r; int rimRsq; int rSq; };
    std::vector<Bubble> bubbles;
    bubbles.reserve(bubbleCount);
    uint32_t rng = static_cast<uint32_t>(bubbleCount) * 0x9E3779B9u +
                   static_cast<uint32_t>(W) * 0x85EBCA6Bu +
                   static_cast<uint32_t>(maxR);
    auto rngStep = [&]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    };
    int radSpan = maxR - minR + 1;
    for (int s = 0; s < bubbleCount; ++s) {
        Bubble b;
        b.x = static_cast<int>((rngStep() & 0xFFFF) / 65535.0f * W);
        b.y = static_cast<int>((rngStep() & 0xFFFF) / 65535.0f * H);
        b.r = minR + static_cast<int>(rngStep() % radSpan);
        b.rSq = b.r * b.r;
        int innerR = std::max(1, b.r - rimW);
        b.rimRsq = innerR * innerR;
        bubbles.push_back(b);
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            bool onRim = false;
            bool hasFill = false;
            for (const auto& b : bubbles) {
                int dx = b.x - x;
                int dy = b.y - y;
                int distSq = dx * dx + dy * dy;
                if (distSq > b.rSq) continue;
                hasFill = true;
                if (distSq >= b.rimRsq) {
                    onRim = true;
                    break;  // rim wins; no need to check further
                }
            }
            uint8_t r, g, b;
            if (onRim) {
                r = rr; g = rg; b = rb_;
            } else if (hasFill) {
                r = fr; g = fg; b = fb_;
            } else {
                r = br_; g = bg_; b = bb_;
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-bubbles: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  bg/fill/rim: %s / %s / %s\n",
                bgHex.c_str(), fillHex.c_str(), rimHex.c_str());
    std::printf("  bubbles    : %d (radius %d-%d, rim %d px)\n",
                bubbleCount, minR, maxR, rimW);
    return 0;
}

int handleSpiderWeb(int& i, int argc, char** argv) {
    // Spider web: classic geometric web with N radial spokes
    // and M concentric polygonal rings centered on the image.
    // Spokes are detected by angular distance to the nearest
    // multiple of 2pi/N (scaled by radius so spokes are pixel-
    // wide near the center and stay readable far out). Rings
    // are detected by radial distance to the nearest of M
    // evenly-spaced radii.
    std::string outPath = argv[++i];
    std::string bgHex  = argv[++i];
    std::string webHex = argv[++i];
    int spokes = 8;
    int rings  = 5;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { spokes = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { rings = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        spokes < 3 || spokes > 64 ||
        rings < 1 || rings > 32) {
        std::fprintf(stderr,
            "gen-texture-spider-web: invalid dims (W/H 1..8192, spokes 3..64, rings 1..32)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, wr, wg, wb_;
    if (!parseHex(bgHex, br_, bg_, bb_) ||
        !parseHex(webHex, wr, wg, wb_)) {
        std::fprintf(stderr,
            "gen-texture-spider-web: bg or web hex color is invalid\n");
        return 1;
    }
    constexpr float kPi = 3.14159265358979323846f;
    const float cx = W * 0.5f;
    const float cy = H * 0.5f;
    // Web extends to the smaller half-extent so it always fits.
    const float maxR = std::min(cx, cy);
    const float spokeStep = 2.0f * kPi / spokes;
    const float ringStep = maxR / rings;
    // Line widths in pixels — kept fixed so the web reads at any
    // image size; users wanting a denser/thicker web can re-run
    // with bigger spoke/ring counts.
    const float lineHalfW = 1.0f;
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float dx = x + 0.5f - cx;
            float dy = y + 0.5f - cy;
            float r = std::sqrt(dx * dx + dy * dy);
            uint8_t cr_, cg_, cb_;
            cr_ = br_; cg_ = bg_; cb_ = bb_;
            if (r > 0.5f && r < maxR + lineHalfW) {
                // Spoke check: angular distance to nearest spoke
                // line, measured as arc length (= r * dTheta) so
                // spokes have constant pixel width regardless of r.
                float theta = std::atan2(dy, dx);
                float wrapped = std::fmod(theta + kPi * 100.0f, spokeStep);
                float spokeDelta = std::min(wrapped, spokeStep - wrapped);
                float arcDist = spokeDelta * r;
                if (arcDist <= lineHalfW) {
                    cr_ = wr; cg_ = wg; cb_ = wb_;
                }
                // Ring check: nearest ring radius. Skip the
                // would-be ring at r=0 (which is the center).
                float ringIdx = r / ringStep;
                float nearestRing = std::round(ringIdx) * ringStep;
                if (nearestRing > 0.5f &&
                    std::fabs(r - nearestRing) <= lineHalfW) {
                    cr_ = wr; cg_ = wg; cb_ = wb_;
                }
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = cr_;
            pixels[idx + 1] = cg_;
            pixels[idx + 2] = cb_;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-spider-web: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d (web center at %.1f, %.1f)\n",
                W, H, cx, cy);
    std::printf("  bg / web   : %s / %s\n", bgHex.c_str(), webHex.c_str());
    std::printf("  spokes     : %d (every %.1f°)\n",
                spokes, 360.0f / spokes);
    std::printf("  rings      : %d (spacing %.1f px)\n",
                rings, ringStep);
    return 0;
}

int handleGingham(int& i, int argc, char** argv) {
    // Gingham: classic picnic-blanket / shirt fabric pattern.
    // Two perpendicular sets of stripes (horizontal + vertical)
    // with a darker color where they cross. The crossing creates
    // the characteristic 3-tone checker that gingham is known
    // for, distinct from --gen-texture-checker (solid blocks).
    std::string outPath = argv[++i];
    std::string bgHex     = argv[++i];
    std::string stripeHex = argv[++i];
    std::string crossHex  = argv[++i];
    int stripeSpacing = 16;
    int stripeWidth   = 8;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { stripeSpacing = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { stripeWidth = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        stripeSpacing < 4 || stripeSpacing > 256 ||
        stripeWidth < 1 || stripeWidth >= stripeSpacing) {
        std::fprintf(stderr,
            "gen-texture-gingham: invalid dims (W/H 1..8192, spacing 4..256, width 1..spacing-1)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, sr, sg, sb_, cr_, cg_, cb_;
    if (!parseHex(bgHex, br_, bg_, bb_) ||
        !parseHex(stripeHex, sr, sg, sb_) ||
        !parseHex(crossHex, cr_, cg_, cb_)) {
        std::fprintf(stderr,
            "gen-texture-gingham: bg/stripe/cross hex color is invalid\n");
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        bool inHStripe = ((y % stripeSpacing) < stripeWidth);
        for (int x = 0; x < W; ++x) {
            bool inVStripe = ((x % stripeSpacing) < stripeWidth);
            uint8_t r, g, b;
            if (inHStripe && inVStripe) {
                // Crossing region: darkest of the three colors.
                r = cr_; g = cg_; b = cb_;
            } else if (inHStripe || inVStripe) {
                // Single-direction stripe band.
                r = sr; g = sg; b = sb_;
            } else {
                r = br_; g = bg_; b = bb_;
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-gingham: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  bg/stripe/cross : %s / %s / %s\n",
                bgHex.c_str(), stripeHex.c_str(), crossHex.c_str());
    std::printf("  spacing    : %d px (stripe width %d)\n",
                stripeSpacing, stripeWidth);
    return 0;
}

int handleLattice(int& i, int argc, char** argv) {
    // Lattice: garden trellis — two sets of diagonal lines, one
    // at +45° and one at -45°, drawn simultaneously across the
    // whole image so they form diamond-shaped openings between
    // the lines. Distinct from --gen-texture-herringbone (which
    // alternates strip orientation) — this draws both diagonals
    // at every pixel.
    std::string outPath  = argv[++i];
    std::string bgHex    = argv[++i];
    std::string lineHex  = argv[++i];
    int lineSpacing = 24;
    int lineWidth   = 3;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { lineSpacing = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { lineWidth = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        lineSpacing < 4 || lineSpacing > 256 ||
        lineWidth < 1 || lineWidth >= lineSpacing) {
        std::fprintf(stderr,
            "gen-texture-lattice: invalid dims (W/H 1..8192, spacing 4..256, width 1..spacing-1)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, lr, lg, lb_;
    if (!parseHex(bgHex, br_, bg_, bb_) ||
        !parseHex(lineHex, lr, lg, lb_)) {
        std::fprintf(stderr,
            "gen-texture-lattice: bg or line hex color is invalid\n");
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // +45° line set: where (x + y) mod spacing is small.
            int posMod = ((x + y) % lineSpacing + lineSpacing) % lineSpacing;
            // -45° line set: where (x - y) mod spacing is small.
            int negMod = ((x - y) % lineSpacing + lineSpacing) % lineSpacing;
            bool onLine = (posMod < lineWidth) || (negMod < lineWidth);
            uint8_t r, g, b;
            if (onLine) {
                r = lr; g = lg; b = lb_;
            } else {
                r = br_; g = bg_; b = bb_;
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-lattice: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  bg / line  : %s / %s\n", bgHex.c_str(), lineHex.c_str());
    std::printf("  diagonals  : ±45° at %d-px spacing, %d-px width\n",
                lineSpacing, lineWidth);
    return 0;
}

int handleHoneycomb(int& i, int argc, char** argv) {
    // Honeycomb: hexagonal cell tiling. Hex centers sit on a
    // triangular lattice (alternating rows shifted by half a
    // horizontal step); each pixel is classified by which hex
    // center it's nearest to (Voronoi cells of a triangular
    // lattice are perfect hexagons). Pixels near a cell
    // boundary become the border color.
    std::string outPath  = argv[++i];
    std::string fillHex  = argv[++i];
    std::string borderHex = argv[++i];
    int hexSide = 16;     // hex side length in pixels
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { hexSide = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        hexSide < 4 || hexSide > 256) {
        std::fprintf(stderr,
            "gen-texture-honeycomb: invalid dims (W/H 1..8192, hexSide 4..256)\n");
        return 1;
    }
    uint8_t fr, fg, fb_, br_, bg_, bb_;
    if (!parseHex(fillHex, fr, fg, fb_) ||
        !parseHex(borderHex, br_, bg_, bb_)) {
        std::fprintf(stderr,
            "gen-texture-honeycomb: fill or border hex color is invalid\n");
        return 1;
    }
    constexpr float kSqrt3 = 1.7320508075688772f;
    // Pointy-top hex grid: horizontal step = hexSide * sqrt(3),
    // vertical step = hexSide * 1.5; alternate rows shifted by
    // half the horizontal step.
    float hStep = hexSide * kSqrt3;
    float vStep = hexSide * 1.5f;
    // Generate seeds covering the image (with a 2-cell margin
    // on each side so border pixels at the image edge always
    // have a 'second nearest' to compare against).
    struct Seed { float x, y; };
    std::vector<Seed> seeds;
    int rowMin = -2;
    int rowMax = static_cast<int>(H / vStep) + 3;
    int colMin = -2;
    int colMax = static_cast<int>(W / hStep) + 3;
    seeds.reserve((rowMax - rowMin + 1) * (colMax - colMin + 1));
    for (int row = rowMin; row <= rowMax; ++row) {
        float shift = (row & 1) ? hStep * 0.5f : 0.0f;
        for (int col = colMin; col <= colMax; ++col) {
            seeds.push_back({col * hStep + shift, row * vStep});
        }
    }
    // Border ratio: pixels where second-nearest seed is within
    // 1.04x of the nearest become border. Tuned so border is
    // 1-2 px at hexSide=16 and scales naturally with hexSide.
    const float boundaryRatio = 1.04f;
    const float boundaryRatioSq = boundaryRatio * boundaryRatio;
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) {
        float fy = static_cast<float>(y);
        for (int x = 0; x < W; ++x) {
            float fx = static_cast<float>(x);
            float bestSq = 1e30f, secondSq = 1e30f;
            for (const auto& s : seeds) {
                float dx = s.x - fx;
                float dy = s.y - fy;
                float d2 = dx * dx + dy * dy;
                if (d2 < bestSq) {
                    secondSq = bestSq;
                    bestSq = d2;
                } else if (d2 < secondSq) {
                    secondSq = d2;
                }
            }
            float ratioSq = (bestSq > 0.0f) ? secondSq / bestSq : 1e30f;
            uint8_t r, g, b;
            if (ratioSq < boundaryRatioSq) {
                r = br_; g = bg_; b = bb_;
            } else {
                r = fr; g = fg; b = fb_;
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-honeycomb: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  fill / border : %s / %s\n",
                fillHex.c_str(), borderHex.c_str());
    std::printf("  hex side   : %d px (%zu seeds total)\n",
                hexSide, seeds.size());
    return 0;
}

int handleCracked(int& i, int argc, char** argv) {
    // Cracked: organic crack network done via recursive random
    // walks from N seed nuclei. Each seed spawns a crack that
    // walks in a random direction for some length, then with
    // 60% chance branches into one or two more cracks of
    // shorter length. Result: irregular fissures that read as
    // cracked mud, dry earth, broken glass, weathered stone.
    std::string outPath  = argv[++i];
    std::string bgHex    = argv[++i];
    std::string crackHex = argv[++i];
    int seedCount = 12;
    int maxLength = 40;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seedCount = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { maxLength = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        seedCount < 1 || seedCount > 4096 ||
        maxLength < 4 || maxLength > 1024) {
        std::fprintf(stderr,
            "gen-texture-cracked: invalid dims (W/H 1..8192, seeds 1..4096, maxLen 4..1024)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, cr_, cg_, cb_;
    if (!parseHex(bgHex, br_, bg_, bb_) ||
        !parseHex(crackHex, cr_, cg_, cb_)) {
        std::fprintf(stderr,
            "gen-texture-cracked: bg or crack hex color is invalid\n");
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (size_t p = 0; p < pixels.size(); p += 3) {
        pixels[p + 0] = br_;
        pixels[p + 1] = bg_;
        pixels[p + 2] = bb_;
    }
    // Deterministic LCG so re-runs reproduce the same pattern.
    uint32_t rng = static_cast<uint32_t>(seedCount) * 0x9E3779B9u +
                   static_cast<uint32_t>(W) * 0x85EBCA6Bu +
                   static_cast<uint32_t>(maxLength);
    auto rngStep = [&]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    };
    auto next01 = [&]() { return (rngStep() & 0xFFFFFF) / float(0x1000000); };
    auto paintPixel = [&](int x, int y) {
        if (x < 0 || x >= W || y < 0 || y >= H) return;
        size_t idx = (static_cast<size_t>(y) * W + x) * 3;
        pixels[idx + 0] = cr_;
        pixels[idx + 1] = cg_;
        pixels[idx + 2] = cb_;
    };
    constexpr float kPi = 3.14159265358979323846f;
    // Iterative DFS instead of true recursion so we don't blow
    // the stack on long branching chains.
    struct Crack { float x, y; int remaining; };
    std::vector<Crack> stack;
    for (int s = 0; s < seedCount; ++s) {
        Crack seed;
        seed.x = next01() * W;
        seed.y = next01() * H;
        seed.remaining = maxLength;
        stack.push_back(seed);
    }
    while (!stack.empty()) {
        Crack c = stack.back();
        stack.pop_back();
        if (c.remaining <= 0) continue;
        // Pick a random direction (any angle) and a per-segment
        // length up to remaining.
        float angle = next01() * 2.0f * kPi;
        float dx = std::cos(angle);
        float dy = std::sin(angle);
        int segLen = 4 + static_cast<int>(next01() * (c.remaining - 4));
        float fx = c.x, fy = c.y;
        for (int t = 0; t < segLen; ++t) {
            paintPixel(static_cast<int>(fx), static_cast<int>(fy));
            fx += dx;
            fy += dy;
        }
        // Branching: 60% chance the segment endpoint spawns 1
        // more crack of half-remaining length, 25% chance it
        // spawns 2 (so most cracks die out, a few network).
        float branchRoll = next01();
        int branches = (branchRoll < 0.25f) ? 2 :
                       (branchRoll < 0.85f) ? 1 : 0;
        for (int b = 0; b < branches; ++b) {
            stack.push_back({fx, fy, c.remaining / 2});
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-cracked: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  bg / crack : %s / %s\n", bgHex.c_str(), crackHex.c_str());
    std::printf("  seeds      : %d (max length %d, branching DFS)\n",
                seedCount, maxLength);
    return 0;
}

int handleRunes(int& i, int argc, char** argv) {
    // Runes: magical glyphs scattered on a textured background.
    // Each rune is 3-5 random line segments emanating from a
    // center point; segments use 8 cardinal/diagonal angles
    // (0°, 45°, 90°, ...) so the strokes read as deliberate
    // angular runes rather than random scribbles. Layout is a
    // sparse grid with per-rune jitter so they look hand-carved.
    std::string outPath  = argv[++i];
    std::string bgHex    = argv[++i];
    std::string runeHex  = argv[++i];
    int gridSpacing = 64;     // rune slot size
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { gridSpacing = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        gridSpacing < 16 || gridSpacing > 512) {
        std::fprintf(stderr,
            "gen-texture-runes: invalid dims (W/H 1..8192, gridSpacing 16..512)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, rr_, rg_, rb_;
    if (!parseHex(bgHex, br_, bg_, bb_) ||
        !parseHex(runeHex, rr_, rg_, rb_)) {
        std::fprintf(stderr,
            "gen-texture-runes: bg or rune hex color is invalid\n");
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (size_t p = 0; p < pixels.size(); p += 3) {
        pixels[p + 0] = br_;
        pixels[p + 1] = bg_;
        pixels[p + 2] = bb_;
    }
    auto paint = [&](int x, int y) {
        if (x < 0 || x >= W || y < 0 || y >= H) return;
        size_t idx = (static_cast<size_t>(y) * W + x) * 3;
        pixels[idx + 0] = rr_;
        pixels[idx + 1] = rg_;
        pixels[idx + 2] = rb_;
    };
    auto drawLine = [&](int x0, int y0, int x1, int y1) {
        // Bresenham. Pixels paint with the rune color.
        int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        while (true) {
            paint(x0, y0);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    };
    // 8 cardinal/diagonal direction unit vectors.
    static const int kDir[8][2] = {
        { 1, 0}, { 1, 1}, {0,  1}, {-1,  1},
        {-1, 0}, {-1,-1}, {0, -1}, { 1, -1},
    };
    // Rune slot grid. Each slot gets a single rune centered
    // (with jitter) inside it.
    int slotR = gridSpacing / 2;
    uint32_t rng = static_cast<uint32_t>(gridSpacing) * 0x9E3779B9u +
                   static_cast<uint32_t>(W) * 0x85EBCA6Bu;
    auto rngStep = [&]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    };
    int runeRadius = slotR / 3;       // half-length of each stroke
    int jitterMax  = slotR / 4;
    int runeCount = 0;
    for (int sy = slotR; sy < H + slotR; sy += gridSpacing) {
        for (int sx = slotR; sx < W + slotR; sx += gridSpacing) {
            // Per-rune jitter so the layout doesn't look like a
            // perfect grid; 5% of slots are skipped (empty
            // ground between runes).
            if ((rngStep() & 0xFF) < 13) continue;  // ~5% skip
            int cx = sx + (static_cast<int>(rngStep() & 0xFF) - 128) *
                          jitterMax / 128;
            int cy = sy + (static_cast<int>(rngStep() & 0xFF) - 128) *
                          jitterMax / 128;
            // 3-5 strokes per rune.
            int strokeCount = 3 + (rngStep() % 3);
            for (int s = 0; s < strokeCount; ++s) {
                int dirA = rngStep() & 7;
                int dirB = rngStep() & 7;
                int lenA = runeRadius * (40 + static_cast<int>(rngStep() % 60)) / 100;
                int lenB = runeRadius * (40 + static_cast<int>(rngStep() % 60)) / 100;
                int x0 = cx + kDir[dirA][0] * lenA;
                int y0 = cy + kDir[dirA][1] * lenA;
                int x1 = cx + kDir[dirB][0] * lenB;
                int y1 = cy + kDir[dirB][1] * lenB;
                drawLine(x0, y0, x1, y1);
            }
            runeCount++;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-runes: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  bg / rune  : %s / %s\n", bgHex.c_str(), runeHex.c_str());
    std::printf("  runes      : %d (slot %d px, 3-5 strokes each)\n",
                runeCount, gridSpacing);
    return 0;
}

int handleLeopard(int& i, int argc, char** argv) {
    // Leopard print: irregular spots scattered across a tan
    // background. Each spot is the union of 3-4 small
    // overlapping sub-circles at slightly offset positions —
    // gives spots an organic non-circular silhouette without
    // needing per-spot polygon authoring. Two colors total
    // (background + spot) for the classic leopard look.
    std::string outPath  = argv[++i];
    std::string bgHex    = argv[++i];
    std::string spotHex  = argv[++i];
    int spotCount = 60;
    int spotRadius = 8;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { spotCount = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { spotRadius = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        spotCount < 1 || spotCount > 4096 ||
        spotRadius < 2 || spotRadius > 256) {
        std::fprintf(stderr,
            "gen-texture-leopard: invalid dims (W/H 1..8192, spots 1..4096, radius 2..256)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, sr_, sg_, sb_;
    if (!parseHex(bgHex, br_, bg_, bb_) ||
        !parseHex(spotHex, sr_, sg_, sb_)) {
        std::fprintf(stderr,
            "gen-texture-leopard: bg or spot hex color is invalid\n");
        return 1;
    }
    // Each spot is composed of 4 sub-circles. Pre-generate the
    // spot list so we can do a single pass per pixel.
    struct Spot {
        int cx[4], cy[4];     // sub-circle centers
        int rSq;              // squared radius (shared)
    };
    std::vector<Spot> spots;
    spots.reserve(spotCount);
    uint32_t rng = static_cast<uint32_t>(spotCount) * 0x9E3779B9u +
                   static_cast<uint32_t>(W) * 0x85EBCA6Bu +
                   static_cast<uint32_t>(spotRadius);
    auto rngStep = [&]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    };
    for (int s = 0; s < spotCount; ++s) {
        Spot sp;
        // Sub-circle radius is 60% of nominal so the union
        // approximates a single spot of approximately the
        // requested radius.
        int subR = std::max(2, spotRadius * 6 / 10);
        sp.rSq = subR * subR;
        // Spot center placed uniformly across the image with a
        // half-radius padding so most spots stay inside.
        int sx = static_cast<int>((rngStep() & 0xFFFF) / 65535.0f * W);
        int sy = static_cast<int>((rngStep() & 0xFFFF) / 65535.0f * H);
        // Sub-circles offset by up to 0.5 * spotRadius from
        // the spot center, in 4 quadrants. Per-spot jitter
        // makes them irregular.
        int jitter = spotRadius / 2;
        for (int k = 0; k < 4; ++k) {
            int dx = static_cast<int>((rngStep() & 0xFF) - 128) * jitter / 128;
            int dy = static_cast<int>((rngStep() & 0xFF) - 128) * jitter / 128;
            sp.cx[k] = sx + dx;
            sp.cy[k] = sy + dy;
        }
        spots.push_back(sp);
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    for (size_t p = 0; p < pixels.size(); p += 3) {
        pixels[p + 0] = br_;
        pixels[p + 1] = bg_;
        pixels[p + 2] = bb_;
    }
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // Inside any spot if any sub-circle covers (x, y).
            bool inSpot = false;
            for (const auto& sp : spots) {
                for (int k = 0; k < 4 && !inSpot; ++k) {
                    int dx = sp.cx[k] - x;
                    int dy = sp.cy[k] - y;
                    if (dx * dx + dy * dy <= sp.rSq) inSpot = true;
                }
                if (inSpot) break;
            }
            if (inSpot) {
                size_t idx = (static_cast<size_t>(y) * W + x) * 3;
                pixels[idx + 0] = sr_;
                pixels[idx + 1] = sg_;
                pixels[idx + 2] = sb_;
            }
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-leopard: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  bg / spot  : %s / %s\n", bgHex.c_str(), spotHex.c_str());
    std::printf("  spots      : %d (radius ~%d, 4 sub-circles each)\n",
                spotCount, spotRadius);
    return 0;
}

int handleZebra(int& i, int argc, char** argv) {
    // Zebra: wavy parallel stripes. The base stripes run
    // horizontally; a sinusoidal y-shift in x makes them
    // undulate, producing the characteristic non-straight
    // zebra look without lining up perfectly with the row
    // grid. Two-color (bg + stripe) for the iconic black-on-
    // white animal-print effect.
    std::string outPath  = argv[++i];
    std::string bgHex    = argv[++i];
    std::string stripeHex = argv[++i];
    int stripePeriod = 24;     // stripe + gap together
    int amplitude    = 8;      // sine-wave amplitude (px)
    int wavelength   = 80;     // x-period of the sine wave (px)
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { stripePeriod = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { amplitude = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { wavelength = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        stripePeriod < 4 || stripePeriod > 256 ||
        amplitude < 0 || amplitude > 128 ||
        wavelength < 8 || wavelength > 1024) {
        std::fprintf(stderr,
            "gen-texture-zebra: invalid dims (W/H 1..8192, period 4..256, "
            "amplitude 0..128, wavelength 8..1024)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, sr, sg, sb_;
    if (!parseHex(bgHex, br_, bg_, bb_) ||
        !parseHex(stripeHex, sr, sg, sb_)) {
        std::fprintf(stderr,
            "gen-texture-zebra: bg or stripe hex color is invalid\n");
        return 1;
    }
    constexpr float kPi = 3.14159265358979323846f;
    const float twoPi = 2.0f * kPi;
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    int halfPeriod = stripePeriod / 2;     // each stripe = half the period
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // Apply sine perturbation to the row index. Each
            // column gets a y-shift based on cos(x * 2π/wavelength).
            float shift = amplitude * std::sin(x * twoPi / wavelength);
            int adjY = y + static_cast<int>(shift);
            int phase = ((adjY % stripePeriod) + stripePeriod) % stripePeriod;
            uint8_t r, g, b;
            if (phase < halfPeriod) {
                r = sr; g = sg; b = sb_;     // stripe
            } else {
                r = br_; g = bg_; b = bb_;   // bg
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-zebra: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  bg/stripe  : %s / %s\n", bgHex.c_str(), stripeHex.c_str());
    std::printf("  stripes    : period %d (amplitude %d, wavelength %d px)\n",
                stripePeriod, amplitude, wavelength);
    return 0;
}

int handleKnit(int& i, int argc, char** argv) {
    // Knit: V-stitch pattern. Each stitch occupies a cellW x cellH
    // cell; the stitch is the V-shape made by two diagonal strokes
    // meeting at the apex (cellW/2, 0) and dropping to the cell's
    // bottom corners. Cells tile contiguously in both axes, giving
    // the iconic chevron-zigzag look of knitted fabric.
    std::string outPath  = argv[++i];
    std::string bgHex    = argv[++i];
    std::string stitchHex = argv[++i];
    int cellW = 16;
    int cellH = 12;
    int strokeWidth = 2;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { cellW = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { cellH = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { strokeWidth = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        cellW < 4 || cellW > 256 ||
        cellH < 4 || cellH > 256 ||
        strokeWidth < 1 || strokeWidth >= cellH) {
        std::fprintf(stderr,
            "gen-texture-knit: invalid dims (W/H 1..8192, cellW/H 4..256, "
            "strokeWidth 1..cellH-1)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, sr, sg, sb_;
    if (!parseHex(bgHex, br_, bg_, bb_) ||
        !parseHex(stitchHex, sr, sg, sb_)) {
        std::fprintf(stderr,
            "gen-texture-knit: bg or stitch hex color is invalid\n");
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    // Slope of each V stroke: rise (cellH) over run (cellW/2)
    // gives slope = 2*cellH/cellW. Vertical strokeWidth in pixels
    // measures how far the pixel is from the ideal stroke line.
    const float slope = 2.0f * cellH / static_cast<float>(cellW);
    for (int y = 0; y < H; ++y) {
        int localY = y % cellH;
        for (int x = 0; x < W; ++x) {
            int localX = x % cellW;
            // Distance from stitch apex along x.
            int d = std::abs(localX - cellW / 2);
            float expectedY = d * slope;
            // Pixel is "stitch" if its localY is within strokeWidth
            // of the V stroke's expected y at this x.
            uint8_t r, g, b;
            if (std::fabs(localY - expectedY) < strokeWidth) {
                r = sr; g = sg; b = sb_;
            } else {
                r = br_; g = bg_; b = bb_;
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-knit: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  bg/stitch  : %s / %s\n", bgHex.c_str(), stitchHex.c_str());
    std::printf("  stitch     : %dx%d (stroke %d px)\n",
                cellW, cellH, strokeWidth);
    return 0;
}

int handleChainmail(int& i, int argc, char** argv) {
    // Chainmail: rings tile in a brick/hexagonal pattern with even
    // and odd rows offset by half a cell width — each pixel is
    // tested against the nearest ring center; if its distance lies
    // in [ringR-stroke/2, ringR+stroke/2] it's painted as the ring
    // color, else background. The cellH < cellW default produces
    // overlapping rings that read as interlinked metal mail.
    std::string outPath = argv[++i];
    std::string bgHex   = argv[++i];
    std::string ringHex = argv[++i];
    int cellW = 14;
    int cellH = 10;
    int ringR = 5;
    float strokeW = 1.5f;
    int W = 256, H = 256;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { cellW = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { cellH = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { ringR = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { strokeW = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { W = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { H = std::stoi(argv[++i]); } catch (...) {}
    }
    if (W < 1 || H < 1 || W > 8192 || H > 8192 ||
        cellW < 4 || cellW > 256 ||
        cellH < 4 || cellH > 256 ||
        ringR < 2 || ringR > cellW ||
        strokeW < 0.5f || strokeW > ringR) {
        std::fprintf(stderr,
            "gen-texture-chainmail: invalid dims (W/H 1..8192, "
            "cellW/H 4..256, ringR 2..cellW, strokeW 0.5..ringR)\n");
        return 1;
    }
    uint8_t br_, bg_, bb_, rr_, rg_, rb_;
    if (!parseHex(bgHex, br_, bg_, bb_) ||
        !parseHex(ringHex, rr_, rg_, rb_)) {
        std::fprintf(stderr,
            "gen-texture-chainmail: bg or ring hex color is invalid\n");
        return 1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
    const float halfStroke = strokeW * 0.5f;
    const float fRingR = static_cast<float>(ringR);
    for (int y = 0; y < H; ++y) {
        // Offset alternate rows by half a cell so rings interlock
        // with the row above/below — classic brick/hex layout.
        int row = y / cellH;
        float rowOffset = (row & 1) ? cellW * 0.5f : 0.0f;
        float cy = (row + 0.5f) * cellH;
        for (int x = 0; x < W; ++x) {
            // Wrap into the row's offset cell to find ring center.
            float xOff = x - rowOffset;
            int col = static_cast<int>(std::floor(xOff / cellW));
            float cx = (col + 0.5f) * cellW + rowOffset;
            float dx = x - cx;
            float dy = y - cy;
            float d = std::sqrt(dx * dx + dy * dy);
            uint8_t r, g, b;
            if (std::fabs(d - fRingR) < halfStroke) {
                r = rr_; g = rg_; b = rb_;
            } else {
                r = br_; g = bg_; b = bb_;
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 3;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
        }
    }
    if (!stbi_write_png(outPath.c_str(), W, H, 3,
                        pixels.data(), W * 3)) {
        std::fprintf(stderr,
            "gen-texture-chainmail: stbi_write_png failed for %s\n",
            outPath.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  size       : %dx%d\n", W, H);
    std::printf("  bg/ring    : %s / %s\n", bgHex.c_str(), ringHex.c_str());
    std::printf("  ring       : R=%d on %dx%d brick (stroke %.2f px)\n",
                ringR, cellW, cellH, strokeW);
    return 0;
}

}  // namespace

namespace {
// Same dispatch pattern as cli_gen_mesh.cpp's kMeshTable. Each row
// names the flag, the minimum arg count after it (used as a guard
// for the dispatcher — kArgRequired catches the bare-flag case at
// argv parse time, but this guard fires when there are zero args
// AND no later argv slots), and the handler function pointer.
struct TextureEntry {
    const char* flag;
    int minNextArgs;
    int (*fn)(int&, int, char**);
};

constexpr TextureEntry kTextureTable[] = {
    {"--gen-texture-gradient",       3, handleGradient},
    {"--gen-texture-noise-color",    3, handleNoiseColor},
    {"--gen-texture-noise",          1, handleNoise},
    {"--gen-texture-radial",         3, handleRadial},
    {"--gen-texture-stripes",        3, handleStripes},
    {"--gen-texture-dots",           3, handleDots},
    {"--gen-texture-rings",          3, handleRings},
    {"--gen-texture-checker",        3, handleChecker},
    {"--gen-texture-brick",          3, handleBrick},
    {"--gen-texture-wood",           3, handleWood},
    {"--gen-texture-grass",          3, handleGrass},
    {"--gen-texture-fabric",         3, handleFabric},
    {"--gen-texture-cobble",         3, handleCobble},
    {"--gen-texture-marble",         2, handleMarble},
    {"--gen-texture-metal",          2, handleMetal},
    {"--gen-texture-leather",        2, handleLeather},
    {"--gen-texture-sand",           2, handleSand},
    {"--gen-texture-snow",           2, handleSnow},
    {"--gen-texture-lava",           3, handleLava},
    {"--gen-texture-tile",           3, handleTile},
    {"--gen-texture-bark",           3, handleBark},
    {"--gen-texture-clouds",         3, handleClouds},
    {"--gen-texture-stars",          3, handleStars},
    {"--gen-texture-vines",          3, handleVines},
    {"--gen-texture-mosaic",         4, handleMosaic},
    {"--gen-texture-rust",           3, handleRust},
    {"--gen-texture-circuit",        3, handleCircuit},
    {"--gen-texture-coral",          3, handleCoral},
    {"--gen-texture-flame",          3, handleFlame},
    {"--gen-texture-tartan",         4, handleTartan},
    {"--gen-texture-argyle",         4, handleArgyle},
    {"--gen-texture-herringbone",    3, handleHerringbone},
    {"--gen-texture-scales",         4, handleScales},
    {"--gen-texture-stained-glass",  5, handleStainedGlass},
    {"--gen-texture-shingles",       4, handleShingles},
    {"--gen-texture-frost",          3, handleFrost},
    {"--gen-texture-parquet",        4, handleParquet},
    {"--gen-texture-bubbles",        4, handleBubbles},
    {"--gen-texture-spider-web",     3, handleSpiderWeb},
    {"--gen-texture-gingham",        4, handleGingham},
    {"--gen-texture-lattice",        3, handleLattice},
    {"--gen-texture-honeycomb",      3, handleHoneycomb},
    {"--gen-texture-cracked",        3, handleCracked},
    {"--gen-texture-runes",          3, handleRunes},
    {"--gen-texture-leopard",        3, handleLeopard},
    {"--gen-texture-zebra",          3, handleZebra},
    {"--gen-texture-knit",           3, handleKnit},
    {"--gen-texture-chainmail",      3, handleChainmail},
};
}  // namespace

bool handleGenTexture(int& i, int argc, char** argv, int& outRc) {
    // Note: order matters only for prefix-collision flags. strcmp
    // is exact-match so e.g. --gen-texture-noise vs --gen-texture-noise-color
    // are unambiguous regardless of order.
    for (const auto& e : kTextureTable) {
        if (std::strcmp(argv[i], e.flag) == 0 && i + e.minNextArgs < argc) {
            outRc = e.fn(i, argc, argv);
            return true;
        }
    }
    return false;
}

}  // namespace cli
}  // namespace editor
}  // namespace wowee
