#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <cstdint>

namespace wowee {
namespace pipeline {

// Wowee Open Building format (.wob) — novel WMO replacement
// Buildings with multiple groups, portals, and doodad sets
struct WoweeBuilding {
    struct Vertex {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec2 texCoord;
        glm::vec4 color; // vertex color/lighting
    };

    struct Group {
        std::string name;
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<std::string> texturePaths;
        glm::vec3 boundMin{0}, boundMax{0};
        bool isOutdoor = false;
    };

    struct Portal {
        int groupA = -1, groupB = -1;
        std::vector<glm::vec3> vertices; // portal polygon
    };

    struct DoodadPlacement {
        std::string modelPath; // .wom path
        glm::vec3 position;
        glm::vec3 rotation;
        float scale = 1.0f;
    };

    std::string name;
    std::vector<Group> groups;
    std::vector<Portal> portals;
    std::vector<DoodadPlacement> doodads;
    float boundRadius = 1.0f;

    bool isValid() const { return !groups.empty(); }
};

class WoweeBuildingLoader {
public:
    static WoweeBuilding load(const std::string& basePath);
    static bool save(const WoweeBuilding& building, const std::string& basePath);
    static bool exists(const std::string& basePath);

    // Convert WOB to WMOModel for the client's WMO renderer
    static bool toWMOModel(const WoweeBuilding& building, class WMOModel& outModel);

    // Convert WMOModel to WOB (for editor export)
    static WoweeBuilding fromWMO(const class WMOModel& wmo, const std::string& name = "");
};

} // namespace pipeline
} // namespace wowee
