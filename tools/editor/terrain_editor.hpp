#pragma once

#include "editor_brush.hpp"
#include "editor_history.hpp"
#include "terrain_biomes.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "rendering/camera.hpp"
#include <vector>
#include <functional>

namespace wowee {
namespace editor {

class TerrainEditor {
public:
    TerrainEditor();

    void setTerrain(pipeline::ADTTerrain* terrain) { terrain_ = terrain; }
    pipeline::ADTTerrain* getTerrain() { return terrain_; }
    const pipeline::ADTTerrain* getTerrain() const { return terrain_; }

    EditorBrush& brush() { return brush_; }
    const EditorBrush& brush() const { return brush_; }
    EditorHistory& history() { return history_; }

    static pipeline::ADTTerrain createBlankTerrain(int tileX, int tileY, float baseHeight = 100.0f,
                                                     Biome biome = Biome::Grassland);

    // Raycast against terrain, returns true if hit
    bool raycastTerrain(const rendering::Ray& ray, glm::vec3& hitPos) const;

    // Apply brush at current position (call per-frame while painting)
    void applyBrush(float deltaTime);

    // Begin/end a paint stroke (for undo grouping)
    void beginStroke();
    void endStroke();
    bool isStrokeActive() const { return strokeActive_; }

    // Get chunks modified since last call (for re-upload)
    std::vector<int> consumeDirtyChunks();

    // Regenerate mesh for specific chunks
    pipeline::TerrainMesh regenerateMesh() const;
    pipeline::ChunkMesh regenerateChunkMesh(int chunkIndex) const;

    void undo();
    void redo();

    // Recalculate normals for modified chunks (improves lighting after sculpt)
    void recalcNormals(const std::vector<int>& chunkIndices);

    // Water editing
    void setWaterLevel(const glm::vec3& center, float radius, float waterHeight, uint16_t liquidType = 0);
    void removeWater(const glm::vec3& center, float radius);

    bool hasUnsavedChanges() const { return dirty_; }
    void markSaved() { dirty_ = false; }

private:
    void applyRaise(float dt);
    void applySmooth(float dt);
    void applyFlatten(float dt);
    void stitchEdges(int chunkIdx);

    std::vector<int> getAffectedChunks(const glm::vec3& center, float radius) const;
    glm::vec3 chunkVertexWorldPos(int chunkIdx, int vertIdx) const;
    float getVertexHeight(int chunkIdx, int vertIdx) const;
    void setVertexHeight(int chunkIdx, int vertIdx, float height);

    pipeline::ADTTerrain* terrain_ = nullptr;
    EditorBrush brush_;
    EditorHistory history_;

    bool strokeActive_ = false;
    bool dirty_ = false;
    std::vector<int> dirtyChunks_;

    static constexpr float TILE_SIZE = 533.33333f;
    static constexpr float CHUNK_SIZE = TILE_SIZE / 16.0f;
    static constexpr float GRID_STEP = CHUNK_SIZE / 8.0f;
};

} // namespace editor
} // namespace wowee
