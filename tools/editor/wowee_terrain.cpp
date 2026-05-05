#include "wowee_terrain.hpp"
#include "pipeline/wowee_terrain_loader.hpp"
#include "core/logger.hpp"
#include "stb_image_write.h"
#include <nlohmann/json.hpp>
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
        // Per-chunk: baseHeight(4) + heights[145](580) + alphaSize(4) + alphaData(N)
        for (int ci = 0; ci < 256; ci++) {
            const auto& chunk = terrain.chunks[ci];
            float base = chunk.position[2];
            f.write(reinterpret_cast<const char*>(&base), 4);
            f.write(reinterpret_cast<const char*>(chunk.heightMap.heights.data()), 145 * 4);
            uint32_t alphaSize = static_cast<uint32_t>(chunk.alphaMap.size());
            f.write(reinterpret_cast<const char*>(&alphaSize), 4);
            if (alphaSize > 0) {
                f.write(reinterpret_cast<const char*>(chunk.alphaMap.data()), alphaSize);
            }
        }
    }

    // Export JSON metadata (.wot = Wowee Open Terrain)
    std::string jsonPath = basePath + ".wot";
    {
        nlohmann::json j;
        j["format"] = "wot-1.0";
        j["editor"] = "wowee-editor-1.0.0";
        j["tileX"] = tileX;
        j["tileY"] = tileY;
        j["chunkGrid"] = {16, 16};
        j["vertsPerChunk"] = 145;
        j["heightmapFile"] = fs::path(hmPath).filename().string();
        j["tileSize"] = 533.33333f;
        j["chunkSize"] = 33.33333f;

        nlohmann::json texArr = nlohmann::json::array();
        for (const auto& tex : terrain.textures) texArr.push_back(tex);
        j["textures"] = texArr;

        nlohmann::json chunkArr = nlohmann::json::array();
        for (int ci = 0; ci < 256; ci++) {
            const auto& chunk = terrain.chunks[ci];
            nlohmann::json cl;
            nlohmann::json layerIds = nlohmann::json::array();
            for (const auto& layer : chunk.layers) layerIds.push_back(layer.textureId);
            cl["layers"] = layerIds;
            cl["holes"] = chunk.holes;
            bool hasAlpha = false;
            for (size_t li = 1; li < chunk.layers.size(); li++)
                if (chunk.layers[li].useAlpha()) { hasAlpha = true; break; }
            cl["hasAlpha"] = hasAlpha;
            chunkArr.push_back(cl);
        }
        j["chunkLayers"] = chunkArr;

        nlohmann::json waterArr = nlohmann::json::array();
        for (int ci = 0; ci < 256; ci++) {
            const auto& water = terrain.waterData[ci];
            if (water.hasWater()) {
                waterArr.push_back({{"chunk", ci},
                    {"type", water.layers[0].liquidType},
                    {"height", water.layers[0].maxHeight}});
            } else {
                waterArr.push_back(nullptr);
            }
        }
        j["water"] = waterArr;

        // Doodad placements (M2 models on terrain)
        nlohmann::json doodadNames = nlohmann::json::array();
        for (const auto& n : terrain.doodadNames) doodadNames.push_back(n);
        j["doodadNames"] = doodadNames;

        nlohmann::json doodads = nlohmann::json::array();
        for (const auto& dp : terrain.doodadPlacements) {
            doodads.push_back({
                {"nameId", dp.nameId}, {"uniqueId", dp.uniqueId},
                {"pos", {dp.position[0], dp.position[1], dp.position[2]}},
                {"rot", {dp.rotation[0], dp.rotation[1], dp.rotation[2]}},
                {"scale", dp.scale}, {"flags", dp.flags}
            });
        }
        j["doodads"] = doodads;

        // WMO placements (buildings on terrain)
        nlohmann::json wmoNames = nlohmann::json::array();
        for (const auto& n : terrain.wmoNames) wmoNames.push_back(n);
        j["wmoNames"] = wmoNames;

        nlohmann::json wmos = nlohmann::json::array();
        for (const auto& wp : terrain.wmoPlacements) {
            wmos.push_back({
                {"nameId", wp.nameId}, {"uniqueId", wp.uniqueId},
                {"pos", {wp.position[0], wp.position[1], wp.position[2]}},
                {"rot", {wp.rotation[0], wp.rotation[1], wp.rotation[2]}},
                {"flags", wp.flags}, {"doodadSet", wp.doodadSet}
            });
        }
        j["wmos"] = wmos;

        std::ofstream f(jsonPath);
        if (!f) return false;
        f << j.dump(2) << "\n";
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

bool WoweeTerrain::exportWaterMask(const pipeline::ADTTerrain& terrain,
                                    const std::string& path) {
    constexpr int res = 16; // One pixel per chunk
    std::vector<uint8_t> pixels(res * res);
    for (int ci = 0; ci < 256; ci++)
        pixels[ci] = terrain.waterData[ci].hasWater() ? 255 : 0;

    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    stbi_write_png(path.c_str(), res, res, 1, pixels.data(), res);
    return true;
}

bool WoweeTerrain::exportHoleMask(const pipeline::ADTTerrain& terrain,
                                   const std::string& path) {
    constexpr int res = 16;
    std::vector<uint8_t> pixels(res * res);
    for (int ci = 0; ci < 256; ci++)
        pixels[ci] = (terrain.chunks[ci].holes != 0) ? 255 : 0;

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
    return pipeline::WoweeTerrainLoader::load(basePath, terrain);
}

} // namespace editor
} // namespace wowee
