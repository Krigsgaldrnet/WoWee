#include "wowee_terrain.hpp"
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
        j["doodadCount"] = terrain.doodadPlacements.size();
        j["wmoCount"] = terrain.wmoPlacements.size();

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
    // Load binary heightmap (.whm)
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
        chunk.indexX = ci % 16;
        chunk.indexY = ci / 16;
        float base;
        f.read(reinterpret_cast<char*>(&base), 4);
        chunk.position[2] = base;
        f.read(reinterpret_cast<char*>(chunk.heightMap.heights.data()), 145 * 4);

        uint32_t alphaSize = 0;
        if (f.read(reinterpret_cast<char*>(&alphaSize), 4) && alphaSize > 0 && alphaSize <= 65536) {
            chunk.alphaMap.resize(alphaSize);
            f.read(reinterpret_cast<char*>(chunk.alphaMap.data()), alphaSize);
        }

        for (int i = 0; i < 145; i++) {
            chunk.normals[i * 3 + 0] = 0;
            chunk.normals[i * 3 + 1] = 0;
            chunk.normals[i * 3 + 2] = 127;
        }
    }

    // Load JSON metadata (.wot)
    std::string wotPath = basePath + ".wot";
    std::ifstream wf(wotPath);
    if (wf) {
        try {
            auto j = nlohmann::json::parse(wf);

            terrain.coord.x = j.value("tileX", 0);
            terrain.coord.y = j.value("tileY", 0);

            float tileSize = 533.33333f;
            float chunkSize = tileSize / 16.0f;
            for (int cy = 0; cy < 16; cy++) {
                for (int cx = 0; cx < 16; cx++) {
                    auto& chunk = terrain.chunks[cy * 16 + cx];
                    chunk.position[0] = (32.0f - terrain.coord.x) * tileSize - cx * chunkSize;
                    chunk.position[1] = (32.0f - terrain.coord.y) * tileSize - cy * chunkSize;
                }
            }

            if (j.contains("textures") && j["textures"].is_array()) {
                for (const auto& tex : j["textures"]) {
                    if (tex.is_string() && !tex.get<std::string>().empty())
                        terrain.textures.push_back(tex.get<std::string>());
                }
            }

            if (j.contains("chunkLayers") && j["chunkLayers"].is_array()) {
                const auto& layers = j["chunkLayers"];
                for (int ci = 0; ci < std::min(256, static_cast<int>(layers.size())); ci++) {
                    const auto& cl = layers[ci];
                    if (cl.contains("layers") && cl["layers"].is_array()) {
                        for (const auto& texId : cl["layers"]) {
                            pipeline::TextureLayer layer{};
                            layer.textureId = texId.get<uint32_t>();
                            layer.flags = terrain.chunks[ci].layers.empty() ? 0 : 0x100;
                            terrain.chunks[ci].layers.push_back(layer);
                        }
                    }
                    if (cl.contains("holes"))
                        terrain.chunks[ci].holes = cl["holes"].get<uint16_t>();
                }
            }

            if (j.contains("water") && j["water"].is_array()) {
                for (const auto& w : j["water"]) {
                    if (w.is_null()) continue;
                    int wci = w.value("chunk", -1);
                    if (wci < 0 || wci >= 256) continue;
                    pipeline::ADTTerrain::WaterLayer wl;
                    wl.liquidType = w.value("type", 0u);
                    wl.maxHeight = w.value("height", 0.0f);
                    wl.minHeight = wl.maxHeight;
                    wl.x = 0; wl.y = 0; wl.width = 9; wl.height = 9;
                    wl.heights.assign(81, wl.maxHeight);
                    wl.mask.assign(8, 0xFF);
                    terrain.waterData[wci].layers.push_back(wl);
                }
            }

            LOG_INFO("WOT metadata loaded: tile [", terrain.coord.x, ",", terrain.coord.y,
                     "], ", terrain.textures.size(), " textures");
        } catch (const std::exception& e) {
            LOG_WARNING("Could not parse WOT metadata: ", e.what());
        }
    }

    LOG_INFO("Open terrain imported: ", basePath);
    return true;
}

} // namespace editor
} // namespace wowee
