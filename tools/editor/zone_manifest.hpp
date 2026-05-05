#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace wowee {
namespace editor {

struct ZoneManifest {
    std::string mapName;
    std::string displayName;
    uint32_t mapId = 9000; // Custom map IDs start high to avoid conflicts
    std::vector<std::pair<int, int>> tiles; // (tileX, tileY) pairs
    std::string biome;
    float baseHeight = 100.0f;
    bool hasCreatures = false;
    std::string description;

    bool save(const std::string& path) const;
    bool load(const std::string& path);
};

} // namespace editor
} // namespace wowee
