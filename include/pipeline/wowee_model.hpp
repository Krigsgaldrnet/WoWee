#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <cstdint>

namespace wowee {
namespace pipeline {

// Wowee Open Model format (.wom) — novel format, no Blizzard IP
// Designed for static doodads, props, and simple animated objects
struct WoweeModel {
    struct Vertex {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec2 texCoord;
    };

    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<std::string> texturePaths; // PNG paths
    float boundRadius = 1.0f;
    glm::vec3 boundMin{0}, boundMax{0};

    bool isValid() const { return !vertices.empty() && !indices.empty(); }
};

class WoweeModelLoader {
public:
    // Load from .wom file (binary) + .wom.json (metadata)
    static WoweeModel load(const std::string& basePath);

    // Save to .wom + .wom.json
    static bool save(const WoweeModel& model, const std::string& basePath);

    // Convert an M2 model to WoweeModel (static geometry only, no animation)
    static WoweeModel fromM2(const std::string& m2Path, class AssetManager* am);

    // Check if a .wom exists
    static bool exists(const std::string& basePath);
};

} // namespace pipeline
} // namespace wowee
