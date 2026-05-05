#pragma once

#include "editor_brush.hpp"
#include "pipeline/adt_loader.hpp"
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace wowee {
namespace editor {

class TexturePainter {
public:
    void setTerrain(pipeline::ADTTerrain* terrain) { terrain_ = terrain; }

    void setActiveTexture(const std::string& texturePath);
    const std::string& getActiveTexture() const { return activeTexture_; }

    // Paint the active texture at the given world position
    // Returns list of modified chunk indices
    std::vector<int> paint(const glm::vec3& center, float radius, float strength, float falloff);

    // Erase a texture layer at the given position
    std::vector<int> erase(const glm::vec3& center, float radius, float strength, float falloff);

private:
    uint32_t ensureTextureInList(const std::string& path);
    int ensureLayerOnChunk(int chunkIdx, uint32_t textureId);
    void modifyAlpha(int chunkIdx, int layerIdx, const glm::vec3& center,
                     float radius, float strength, float falloff, bool erasing);

    glm::vec2 worldToChunkUV(int chunkIdx, const glm::vec3& worldPos) const;

    pipeline::ADTTerrain* terrain_ = nullptr;
    std::string activeTexture_;

    static constexpr float TILE_SIZE = 533.33333f;
    static constexpr float CHUNK_SIZE = TILE_SIZE / 16.0f;
};

} // namespace editor
} // namespace wowee
