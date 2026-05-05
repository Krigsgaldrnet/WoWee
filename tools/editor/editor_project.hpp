#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace wowee {
namespace editor {

struct ProjectZone {
    std::string mapName;
    int tileX = 32, tileY = 32;
    std::string biome;
    std::string description;
};

struct EditorProject {
    std::string name = "Untitled";
    std::string author;
    std::string description;
    std::string version = "1.0";
    std::string projectDir;
    uint32_t startMapId = 9000;
    std::vector<ProjectZone> zones;

    bool save(const std::string& path) const;
    bool load(const std::string& path);
    std::string getZoneOutputDir(int zoneIdx) const;
};

} // namespace editor
} // namespace wowee
