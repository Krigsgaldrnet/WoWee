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

    // Noise generator: applies procedural height noise to the terrain
    void applyNoise(float frequency, float amplitude, int octaves, uint32_t seed);

    // Global smooth pass across entire tile (N iterations)
    void smoothEntireTile(int iterations);

    // Clamp all heights to a min/max range
    void clampHeights(float minH, float maxH);

    // Scale all heights by a factor (useful for exaggerating or flattening)
    void scaleHeights(float factor);

    // Terrain stamp: copy heights from source area, paste at destination
    void copyStamp(const glm::vec3& center, float radius);
    void pasteStamp(const glm::vec3& center);
    bool hasStamp() const { return !stampData_.empty(); }

    // Mirror terrain along X or Y axis through tile center
    void mirrorX();
    void mirrorY();

    // Import/export heightmap (raw 16-bit grayscale, 129x129)
    bool importHeightmap(const std::string& path, float heightScale);
    bool exportHeightmap(const std::string& path, float heightScale);

    // Water editing
    void setWaterLevel(const glm::vec3& center, float radius, float waterHeight, uint16_t liquidType = 0);
    void removeWater(const glm::vec3& center, float radius);

    // Hole editing (4x4 bitmask per chunk — cave entrances, mine shafts)
    void punchHole(const glm::vec3& center, float radius);
    void fillHole(const glm::vec3& center, float radius);

    bool hasUnsavedChanges() const { return dirty_; }
    void markSaved() { dirty_ = false; }

private:
    void applyRaise(float dt);
    void applySmooth(float dt);
    void applyFlatten(float dt);
    void applyErode(float dt);
    void stitchEdges(int chunkIdx);

    std::vector<int> getAffectedChunks(const glm::vec3& center, float radius) const;
    glm::vec3 chunkVertexWorldPos(int chunkIdx, int vertIdx) const;
    float getVertexHeight(int chunkIdx, int vertIdx) const;
    void setVertexHeight(int chunkIdx, int vertIdx, float height);

    struct StampVertex { float dx, dy, height; };
    std::vector<StampVertex> stampData_;
    glm::vec3 stampCenter_{0};

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
