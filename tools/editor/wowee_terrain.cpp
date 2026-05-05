#include "wowee_terrain.hpp"
#include "core/logger.hpp"
#include "stb_image_write.h"
#include <fstream>
#include <filesystem>
#include <cstring>

namespace wowee {
namespace editor {

bool WoweeTerrain::exportOpen(const pipeline::ADTTerrain& terrain,
                               const std::string& basePath, int tileX, int tileY) {
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(basePath).parent_path());

    // Export binary heightmap (.whm = Wowee HeightMap)
    // Format: 256 chunks × 145 floats = 37120 floats (148480 bytes)
    std::string hmPath = basePath + ".whm";
    {
        std::ofstream f(hmPath, std::ios::binary);
        if (!f) return false;
        // Header: "WHM1" + chunkCount(4) + vertsPerChunk(4)
        uint32_t magic = 0x314D4857; // "WHM1"
        uint32_t chunks = 256, verts = 145;
        f.write(reinterpret_cast<const char*>(&magic), 4);
        f.write(reinterpret_cast<const char*>(&chunks), 4);
        f.write(reinterpret_cast<const char*>(&verts), 4);
        // Per-chunk: baseHeight(4) + heights[145](580)
        for (int ci = 0; ci < 256; ci++) {
            const auto& chunk = terrain.chunks[ci];
            float base = chunk.position[2];
            f.write(reinterpret_cast<const char*>(&base), 4);
            f.write(reinterpret_cast<const char*>(chunk.heightMap.heights.data()), 145 * 4);
        }
    }

    // Export JSON metadata (.wot = Wowee Open Terrain)
    std::string jsonPath = basePath + ".wot";
    {
        std::ofstream f(jsonPath);
        if (!f) return false;
        f << "{\n";
        f << "  \"format\": \"wot-1.0\",\n";
        f << "  \"tileX\": " << tileX << ",\n";
        f << "  \"tileY\": " << tileY << ",\n";
        f << "  \"chunkGrid\": [16, 16],\n";
        f << "  \"vertsPerChunk\": 145,\n";
        f << "  \"heightmapFile\": \"" << fs::path(hmPath).filename().string() << "\",\n";
        f << "  \"textures\": [\n";
        for (size_t i = 0; i < terrain.textures.size(); i++) {
            f << "    \"" << terrain.textures[i] << "\"";
            if (i + 1 < terrain.textures.size()) f << ",";
            f << "\n";
        }
        f << "  ],\n";
        f << "  \"tileSize\": 533.33333,\n";
        f << "  \"chunkSize\": 33.33333,\n";
        f << "  \"chunkLayers\": [\n";
        for (int ci = 0; ci < 256; ci++) {
            const auto& chunk = terrain.chunks[ci];
            f << "    {\"layers\": [";
            for (size_t li = 0; li < chunk.layers.size(); li++) {
                f << chunk.layers[li].textureId;
                if (li + 1 < chunk.layers.size()) f << ",";
            }
            f << "], \"holes\": " << chunk.holes;
            // Include alpha map presence flag
            bool hasAlpha = false;
            for (size_t li = 1; li < chunk.layers.size(); li++)
                if (chunk.layers[li].useAlpha()) { hasAlpha = true; break; }
            f << ", \"hasAlpha\": " << (hasAlpha ? "true" : "false");
            f << "}";
            if (ci < 255) f << ",";
            f << "\n";
        }
        f << "  ],\n";
        // Water data
        f << "  \"water\": [\n";
        for (int ci = 0; ci < 256; ci++) {
            const auto& water = terrain.waterData[ci];
            if (water.hasWater()) {
                f << "    {\"chunk\": " << ci
                  << ", \"type\": " << water.layers[0].liquidType
                  << ", \"height\": " << water.layers[0].maxHeight << "}";
            } else {
                f << "    null";
            }
            if (ci < 255) f << ",";
            f << "\n";
        }
        f << "  ]\n";
        f << "}\n";
    }

    LOG_INFO("Open terrain exported: ", basePath, " (.wot + .whm)");
    return true;
}

bool WoweeTerrain::exportNormalMap(const pipeline::ADTTerrain& terrain,
                                    const std::string& path) {
    // Export 129x129 normal map as RGB PNG
    constexpr int res = 129;
    std::vector<uint8_t> pixels(res * res * 3);

    for (int cy = 0; cy < 16; cy++) {
        for (int cx = 0; cx < 16; cx++) {
            const auto& chunk = terrain.chunks[cy * 16 + cx];
            if (!chunk.hasHeightMap()) continue;
            for (int v = 0; v < 145; v++) {
                int row = v / 17, col = v % 17;
                if (col > 8) continue;
                int px = cx * 8 + col, py = cy * 8 + row;
                if (px >= res || py >= res) continue;

                int ni = v * 3;
                float nx = static_cast<float>(chunk.normals[ni]) / 127.0f;
                float ny = static_cast<float>(chunk.normals[ni + 1]) / 127.0f;
                float nz = static_cast<float>(chunk.normals[ni + 2]) / 127.0f;

                int idx = (py * res + px) * 3;
                pixels[idx] = static_cast<uint8_t>((nx * 0.5f + 0.5f) * 255);
                pixels[idx + 1] = static_cast<uint8_t>((ny * 0.5f + 0.5f) * 255);
                pixels[idx + 2] = static_cast<uint8_t>((nz * 0.5f + 0.5f) * 255);
            }
        }
    }

    stbi_write_png(path.c_str(), res, res, 3, pixels.data(), res * 3);
    return true;
}

bool WoweeTerrain::exportHeightmapPreview(const pipeline::ADTTerrain& terrain,
                                           const std::string& path) {
    constexpr int res = 129;
    std::vector<uint8_t> pixels(res * res);

    float minH = 1e30f, maxH = -1e30f;
    for (int ci = 0; ci < 256; ci++) {
        const auto& chunk = terrain.chunks[ci];
        if (!chunk.hasHeightMap()) continue;
        for (int v = 0; v < 145; v++) {
            float h = chunk.position[2] + chunk.heightMap.heights[v];
            minH = std::min(minH, h);
            maxH = std::max(maxH, h);
        }
    }
    float range = std::max(1.0f, maxH - minH);

    for (int cy = 0; cy < 16; cy++) {
        for (int cx = 0; cx < 16; cx++) {
            const auto& chunk = terrain.chunks[cy * 16 + cx];
            if (!chunk.hasHeightMap()) continue;
            for (int v = 0; v < 145; v++) {
                int row = v / 17, col = v % 17;
                if (col > 8) continue;
                int px = cx * 8 + col, py = cy * 8 + row;
                if (px >= res || py >= res) continue;
                float h = chunk.position[2] + chunk.heightMap.heights[v];
                float t = (h - minH) / range;
                pixels[py * res + px] = static_cast<uint8_t>(t * 255.0f);
            }
        }
    }

    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    stbi_write_png(path.c_str(), res, res, 1, pixels.data(), res);
    return true;
}

int WoweeTerrain::exportAlphaMaps(const pipeline::ADTTerrain& terrain,
                                    const std::string& outputDir) {
    namespace fs = std::filesystem;
    fs::create_directories(outputDir);
    int exported = 0;

    for (int ci = 0; ci < 256; ci++) {
        const auto& chunk = terrain.chunks[ci];
        for (size_t li = 1; li < chunk.layers.size(); li++) {
            if (!chunk.layers[li].useAlpha()) continue;
            size_t off = chunk.layers[li].offsetMCAL;
            if (off + 4096 > chunk.alphaMap.size()) continue;

            std::string path = outputDir + "/chunk_" + std::to_string(ci) +
                               "_layer_" + std::to_string(li) + ".png";
            stbi_write_png(path.c_str(), 64, 64, 1,
                           chunk.alphaMap.data() + off, 64);
            exported++;
        }
    }
    return exported;
}

bool WoweeTerrain::importOpen(const std::string& basePath, pipeline::ADTTerrain& terrain) {
    std::string hmPath = basePath + ".whm";
    std::ifstream f(hmPath, std::ios::binary);
    if (!f) return false;

    uint32_t magic, chunks, verts;
    f.read(reinterpret_cast<char*>(&magic), 4);
    if (magic != 0x314D4857) return false;
    f.read(reinterpret_cast<char*>(&chunks), 4);
    f.read(reinterpret_cast<char*>(&verts), 4);
    if (chunks != 256 || verts != 145) return false;

    terrain.loaded = true;
    terrain.version = 18;
    for (int ci = 0; ci < 256; ci++) {
        auto& chunk = terrain.chunks[ci];
        chunk.heightMap.loaded = true;
        float base;
        f.read(reinterpret_cast<char*>(&base), 4);
        chunk.position[2] = base;
        f.read(reinterpret_cast<char*>(chunk.heightMap.heights.data()), 145 * 4);
    }

    LOG_INFO("Open terrain imported: ", basePath);
    return true;
}

} // namespace editor
} // namespace wowee
