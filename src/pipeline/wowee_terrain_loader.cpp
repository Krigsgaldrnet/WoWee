#include "pipeline/wowee_terrain_loader.hpp"
#include "core/logger.hpp"
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

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Parse tile coordinates
    auto findInt = [&](const std::string& key) -> int {
        auto pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) return 0;
        pos = content.find(':', pos);
        return std::stoi(content.substr(pos + 1));
    };

    terrain.coord.x = findInt("tileX");
    terrain.coord.y = findInt("tileY");

    // Compute chunk world positions from tile coordinates
    float tileSize = 533.33333f;
    float chunkSize = tileSize / 16.0f;
    for (int cy = 0; cy < 16; cy++) {
        for (int cx = 0; cx < 16; cx++) {
            auto& chunk = terrain.chunks[cy * 16 + cx];
            chunk.position[0] = (32.0f - terrain.coord.x) * tileSize - cx * chunkSize;
            chunk.position[1] = (32.0f - terrain.coord.y) * tileSize - cy * chunkSize;
            // position[2] already set by heightmap loader
        }
    }

    // Parse textures array
    auto texStart = content.find("\"textures\"");
    if (texStart != std::string::npos) {
        size_t pos = texStart;
        while ((pos = content.find('"', pos + 1)) != std::string::npos) {
            if (content[pos - 1] == '[' || content[pos - 1] == ',') {
                auto end = content.find('"', pos + 1);
                if (end == std::string::npos) break;
                std::string tex = content.substr(pos + 1, end - pos - 1);
                if (tex != "textures" && !tex.empty())
                    terrain.textures.push_back(tex);
                pos = end;
            }
            auto closeBracket = content.find(']', texStart);
            if (pos > closeBracket) break;
        }
    }

    // Parse chunk layers
    auto layersStart = content.find("\"chunkLayers\"");
    if (layersStart != std::string::npos) {
        size_t pos = layersStart;
        int ci = 0;
        while (ci < 256 && (pos = content.find('{', pos + 1)) != std::string::npos) {
            auto endObj = content.find('}', pos);
            if (endObj == std::string::npos) break;
            auto layersClose = content.find(']', content.find("\"chunkLayers\""));
            if (pos > layersClose) break;

            std::string block = content.substr(pos, endObj - pos + 1);

            // Parse layers array
            auto lStart = block.find("\"layers\":[");
            if (lStart != std::string::npos) {
                lStart += 10;
                auto lEnd = block.find(']', lStart);
                std::string layerStr = block.substr(lStart, lEnd - lStart);
                // Parse comma-separated integers
                size_t lp = 0;
                while (lp < layerStr.size()) {
                    while (lp < layerStr.size() && !std::isdigit(layerStr[lp])) lp++;
                    if (lp >= layerStr.size()) break;
                    uint32_t texId = std::stoi(layerStr.substr(lp));
                    TextureLayer layer{};
                    layer.textureId = texId;
                    layer.flags = (terrain.chunks[ci].layers.empty()) ? 0 : 0x100;
                    terrain.chunks[ci].layers.push_back(layer);
                    while (lp < layerStr.size() && std::isdigit(layerStr[lp])) lp++;
                }
            }

            // Parse holes
            auto holesPos = block.find("\"holes\":");
            if (holesPos != std::string::npos)
                terrain.chunks[ci].holes = static_cast<uint16_t>(std::stoi(block.substr(holesPos + 8)));

            ci++;
            pos = endObj;
        }
    }

    // Parse water data
    auto waterStart = content.find("\"water\"");
    if (waterStart != std::string::npos) {
        size_t pos = waterStart;
        int ci = 0;
        while (ci < 256) {
            auto nextObj = content.find('{', pos + 1);
            auto nextNull = content.find("null", pos + 1);
            auto waterClose = content.find(']', waterStart);

            if (nextObj != std::string::npos && nextObj < waterClose &&
                (nextNull == std::string::npos || nextObj < nextNull)) {
                auto endObj = content.find('}', nextObj);
                std::string block = content.substr(nextObj, endObj - nextObj + 1);

                auto chunkPos = block.find("\"chunk\":");
                auto typePos = block.find("\"type\":");
                auto heightPos = block.find("\"height\":");

                if (chunkPos != std::string::npos) {
                    int wci = std::stoi(block.substr(chunkPos + 8));
                    if (wci >= 0 && wci < 256) {
                        ADTTerrain::WaterLayer wl;
                        wl.liquidType = (typePos != std::string::npos) ?
                            static_cast<uint16_t>(std::stoi(block.substr(typePos + 7))) : 0;
                        wl.maxHeight = (heightPos != std::string::npos) ?
                            std::stof(block.substr(heightPos + 9)) : 0;
                        wl.minHeight = wl.maxHeight;
                        wl.x = 0; wl.y = 0; wl.width = 9; wl.height = 9;
                        wl.heights.assign(81, wl.maxHeight);
                        wl.mask.assign(8, 0xFF);
                        terrain.waterData[wci].layers.push_back(wl);
                    }
                }
                pos = endObj;
            } else {
                pos = (nextNull != std::string::npos && nextNull < waterClose) ? nextNull + 4 : waterClose;
            }
            ci++;
            if (pos >= waterClose) break;
        }
    }

    LOG_INFO("WOT loaded: ", wotPath, " (tile [", terrain.coord.x, ",", terrain.coord.y,
             "], ", terrain.textures.size(), " textures)");
    return true;
}

bool WoweeTerrainLoader::load(const std::string& basePath, ADTTerrain& terrain) {
    if (!loadHeightmap(basePath + ".whm", terrain)) return false;
    if (!loadMetadata(basePath + ".wot", terrain)) return false;
    return true;
}

} // namespace pipeline
} // namespace wowee
