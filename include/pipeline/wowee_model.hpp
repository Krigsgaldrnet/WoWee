#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <cstdint>

namespace wowee {
namespace pipeline {

struct M2Model;

// Wowee Open Model format (.wom) — novel format, no Blizzard IP
// WOM1: static geometry | WOM2: + bones + animations
struct WoweeModel {
    struct Vertex {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec2 texCoord;
        uint8_t boneWeights[4] = {255, 0, 0, 0};
        uint8_t boneIndices[4] = {0, 0, 0, 0};
    };

    struct Bone {
        int32_t keyBoneId = -1;
        int16_t parentBone = -1;
        glm::vec3 pivot{0};
        uint32_t flags = 0;
    };

    struct AnimKeyframe {
        uint32_t timeMs;
        glm::vec3 translation;
        glm::quat rotation;
        glm::vec3 scale;
    };

    struct Animation {
        uint32_t id = 0;
        uint32_t durationMs = 0;
        float movingSpeed = 0;
        std::vector<std::vector<AnimKeyframe>> boneKeyframes; // [boneIdx][keyframe]
    };

    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<std::string> texturePaths;
    std::vector<Bone> bones;
    std::vector<Animation> animations;
    float boundRadius = 1.0f;
    glm::vec3 boundMin{0}, boundMax{0};
    uint32_t version = 1; // 1=WOM1(static), 2=WOM2(animated)

    bool isValid() const { return !vertices.empty() && !indices.empty(); }
    bool hasAnimation() const { return !bones.empty() && !animations.empty(); }
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

    // Convert WoweeModel to an in-memory M2Model so the M2Renderer can consume it.
    // Single batch, single material — sufficient for static and simple animated meshes.
    static M2Model toM2(const WoweeModel& wom);

    // Convenience: try loading <path-without-ext>.wom from the standard editor
    // search paths (custom_zones/models/, output/models/). Returns valid model on hit.
    static WoweeModel tryLoadByGamePath(const std::string& gamePath);
};

} // namespace pipeline
} // namespace wowee
