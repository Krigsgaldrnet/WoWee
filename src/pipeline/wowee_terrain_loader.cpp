#include "pipeline/wowee_terrain_loader.hpp"
#include "core/logger.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <cstring>

namespace wowee {
namespace pipeline {

static constexpr uint32_t WHM_MAGIC = 0x314D4857; // "WHM1"

bool WoweeTerrainLoader::exists(const std::string& basePath) {
    return std::filesystem::exists(basePath + ".whm") &&
           std::filesystem::exists(basePath + ".wot");
}

bool WoweeTerrainLoader::loadHeightmap(const std::string& whmPath, ADTTerrain& terrain) {
    std::ifstream f(whmPath, std::ios::binary);
    if (!f) return false;

    uint32_t magic, chunks, verts;
    f.read(reinterpret_cast<char*>(&magic), 4);
    if (magic != WHM_MAGIC) {
        LOG_ERROR("Not a WHM file: ", whmPath);
        return false;
    }
    f.read(reinterpret_cast<char*>(&chunks), 4);
    f.read(reinterpret_cast<char*>(&verts), 4);

    if (chunks != 256 || verts != 145) {
        LOG_ERROR("WHM unexpected dimensions: ", chunks, " chunks, ", verts, " verts");
        return false;
    }

    terrain.loaded = true;
    terrain.version = 18;

    for (int ci = 0; ci < 256; ci++) {
        auto& chunk = terrain.chunks[ci];
        chunk.heightMap.loaded = true;
        chunk.indexX = ci % 16;
        chunk.indexY = ci / 16;
        chunk.flags = 0;
        chunk.holes = 0;

        float base;
        f.read(reinterpret_cast<char*>(&base), 4);
        chunk.position[2] = base;

        f.read(reinterpret_cast<char*>(chunk.heightMap.heights.data()), 145 * 4);

        // Read alpha map data (may not be present in older WHM files)
        uint32_t alphaSize = 0;
        if (f.read(reinterpret_cast<char*>(&alphaSize), 4) && alphaSize > 0 && alphaSize <= 65536) {
            chunk.alphaMap.resize(alphaSize);
            f.read(reinterpret_cast<char*>(chunk.alphaMap.data()), alphaSize);
        }

        // Default normals (up)
        for (int i = 0; i < 145; i++) {
            chunk.normals[i * 3 + 0] = 0;
            chunk.normals[i * 3 + 1] = 0;
            chunk.normals[i * 3 + 2] = 127;
        }
    }

    LOG_INFO("WHM loaded: ", whmPath, " (256 chunks, 145 verts each)");
    return true;
}

bool WoweeTerrainLoader::loadMetadata(const std::string& wotPath, ADTTerrain& terrain) {
    std::ifstream f(wotPath);
    if (!f) return false;

    try {
        auto j = nlohmann::json::parse(f);

        terrain.coord.x = j.value("tileX", 0);
        terrain.coord.y = j.value("tileY", 0);

        // Compute chunk world positions from tile coordinates
        float tileSize = 533.33333f;
        float chunkSize = tileSize / 16.0f;
        for (int cy = 0; cy < 16; cy++) {
            for (int cx = 0; cx < 16; cx++) {
                auto& chunk = terrain.chunks[cy * 16 + cx];
                chunk.position[0] = (32.0f - terrain.coord.x) * tileSize - cx * chunkSize;
                chunk.position[1] = (32.0f - terrain.coord.y) * tileSize - cy * chunkSize;
            }
        }

        // Parse textures
        if (j.contains("textures") && j["textures"].is_array()) {
            for (const auto& tex : j["textures"]) {
                if (tex.is_string() && !tex.get<std::string>().empty())
                    terrain.textures.push_back(tex.get<std::string>());
            }
        }

        // Parse chunk layers
        if (j.contains("chunkLayers") && j["chunkLayers"].is_array()) {
            const auto& layers = j["chunkLayers"];
            for (int ci = 0; ci < std::min(256, static_cast<int>(layers.size())); ci++) {
                const auto& cl = layers[ci];
                if (cl.contains("layers") && cl["layers"].is_array()) {
                    for (const auto& texId : cl["layers"]) {
                        TextureLayer layer{};
                        layer.textureId = texId.get<uint32_t>();
                        layer.flags = terrain.chunks[ci].layers.empty() ? 0 : 0x100;
                        terrain.chunks[ci].layers.push_back(layer);
                    }
                }
                if (cl.contains("holes"))
                    terrain.chunks[ci].holes = cl["holes"].get<uint16_t>();
            }
        }

        // Parse water data
        if (j.contains("water") && j["water"].is_array()) {
            for (const auto& w : j["water"]) {
                if (w.is_null()) continue;
                int wci = w.value("chunk", -1);
                if (wci < 0 || wci >= 256) continue;
                ADTTerrain::WaterLayer wl;
                wl.liquidType = w.value("type", 0u);
                wl.maxHeight = w.value("height", 0.0f);
                wl.minHeight = wl.maxHeight;
                wl.x = 0; wl.y = 0; wl.width = 9; wl.height = 9;
                wl.heights.assign(81, wl.maxHeight);
                wl.mask.assign(8, 0xFF);
                terrain.waterData[wci].layers.push_back(wl);
            }
        }

        LOG_INFO("WOT loaded: ", wotPath, " (tile [", terrain.coord.x, ",", terrain.coord.y,
                 "], ", terrain.textures.size(), " textures)");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse WOT: ", e.what());
        return false;
    }
}

bool WoweeTerrainLoader::load(const std::string& basePath, ADTTerrain& terrain) {
    if (!loadHeightmap(basePath + ".whm", terrain)) return false;
    if (!loadMetadata(basePath + ".wot", terrain)) return false;
    return true;
}

} // namespace pipeline
} // namespace wowee
