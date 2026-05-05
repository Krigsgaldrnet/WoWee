#include "wowee_terrain.hpp"
#include "core/logger.hpp"
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
        f << "  \"chunkLayers\": [\n";
        for (int ci = 0; ci < 256; ci++) {
            const auto& chunk = terrain.chunks[ci];
            f << "    {\"layers\": [";
            for (size_t li = 0; li < chunk.layers.size(); li++) {
                f << chunk.layers[li].textureId;
                if (li + 1 < chunk.layers.size()) f << ",";
            }
            f << "], \"holes\": " << chunk.holes << "}";
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
