#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <cstdint>

namespace wowee {
namespace pipeline {

struct ADTTerrain;

// Wowee Open Collision format (.woc) — walkability mesh for custom zones
struct WoweeCollision {
    struct Triangle {
        glm::vec3 v0, v1, v2;
        uint8_t flags;  // 0x01=walkable, 0x02=water, 0x04=steep, 0x08=indoor
    };

    struct BoundingBox {
        glm::vec3 min{1e30f}, max{-1e30f};
        void expand(const glm::vec3& p) {
            min = glm::min(min, p);
            max = glm::max(max, p);
        }
    };

    std::vector<Triangle> triangles;
    BoundingBox bounds;
    uint32_t tileX = 0, tileY = 0;

    bool isValid() const { return !triangles.empty(); }
    size_t walkableCount() const;
    size_t steepCount() const;
};

class WoweeCollisionBuilder {
public:
    // Generate collision mesh from terrain heightmap
    static WoweeCollision fromTerrain(const ADTTerrain& terrain,
                                       float steepAngle = 50.0f);

    // Save collision mesh to binary file
    static bool save(const WoweeCollision& collision, const std::string& path);

    // Load collision mesh from binary file
    static WoweeCollision load(const std::string& path);

    // Check if a collision file exists
    static bool exists(const std::string& basePath);
};

} // namespace pipeline
} // namespace wowee
