#include "cli_gen_texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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

}  // namespace

bool handleGenTexture(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-texture-gradient") == 0 && i + 3 < argc) {
        outRc = handleGradient(i, argc, argv); return true;
    }
    // noise-color first because the prefix-match would otherwise hit
    // 'noise' on a 'noise-color' invocation.
    if (std::strcmp(argv[i], "--gen-texture-noise-color") == 0 && i + 3 < argc) {
        outRc = handleNoiseColor(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-noise") == 0 && i + 1 < argc) {
        outRc = handleNoise(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-radial") == 0 && i + 3 < argc) {
        outRc = handleRadial(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-stripes") == 0 && i + 3 < argc) {
        outRc = handleStripes(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-dots") == 0 && i + 3 < argc) {
        outRc = handleDots(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-rings") == 0 && i + 3 < argc) {
        outRc = handleRings(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-checker") == 0 && i + 3 < argc) {
        outRc = handleChecker(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-brick") == 0 && i + 3 < argc) {
        outRc = handleBrick(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-wood") == 0 && i + 3 < argc) {
        outRc = handleWood(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-grass") == 0 && i + 3 < argc) {
        outRc = handleGrass(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-fabric") == 0 && i + 3 < argc) {
        outRc = handleFabric(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-cobble") == 0 && i + 3 < argc) {
        outRc = handleCobble(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-marble") == 0 && i + 2 < argc) {
        outRc = handleMarble(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-metal") == 0 && i + 2 < argc) {
        outRc = handleMetal(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-leather") == 0 && i + 2 < argc) {
        outRc = handleLeather(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-sand") == 0 && i + 2 < argc) {
        outRc = handleSand(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-snow") == 0 && i + 2 < argc) {
        outRc = handleSnow(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-lava") == 0 && i + 3 < argc) {
        outRc = handleLava(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-tile") == 0 && i + 3 < argc) {
        outRc = handleTile(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-bark") == 0 && i + 3 < argc) {
        outRc = handleBark(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-clouds") == 0 && i + 3 < argc) {
        outRc = handleClouds(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-stars") == 0 && i + 3 < argc) {
        outRc = handleStars(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-texture-vines") == 0 && i + 3 < argc) {
        outRc = handleVines(i, argc, argv); return true;
    }
    return false;
}

}  // namespace cli
}  // namespace editor
}  // namespace wowee
