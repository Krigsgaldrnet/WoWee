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


}  // namespace

bool handleGenTexture(int& i, int argc, char** argv, int& outRc) {
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
    return false;
}

}  // namespace cli
}  // namespace editor
}  // namespace wowee
