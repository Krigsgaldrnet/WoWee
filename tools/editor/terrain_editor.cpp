#include "terrain_editor.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace wowee {
namespace editor {

TerrainEditor::TerrainEditor() = default;

pipeline::ADTTerrain TerrainEditor::createBlankTerrain(int tileX, int tileY, float baseHeight,
                                                        Biome biome) {
    pipeline::ADTTerrain terrain;
    terrain.loaded = true;
    terrain.version = 18;
    terrain.coord = {tileX, tileY};

    const auto& biomeTextures = getBiomeTextures(biome);

    // Integer grid noise — guarantees shared edge vertices get identical heights
    auto gridNoise = [](int gx, int gy) -> float {
        uint32_t h = static_cast<uint32_t>(gx * 374761393 + gy * 668265263);
        h = (h ^ (h >> 13)) * 1274126177;
        h = h ^ (h >> 16);
        return (static_cast<float>(h & 0xFFFF) / 65535.0f - 0.5f) * 3.0f;
    };

    for (int cy = 0; cy < 16; cy++) {
        for (int cx = 0; cx < 16; cx++) {
            auto& chunk = terrain.chunks[cy * 16 + cx];
            chunk.flags = 0;
            chunk.indexX = cx;
            chunk.indexY = cy;
            chunk.holes = 0;

            chunk.position[0] = (32.0f - tileX) * TILE_SIZE - cx * CHUNK_SIZE;
            chunk.position[1] = (32.0f - tileY) * TILE_SIZE - cy * CHUNK_SIZE;
            chunk.position[2] = baseHeight;

            chunk.heightMap.loaded = true;

            for (int i = 0; i < 145; i++) {
                int row = i / 17;
                int col = i % 17;

                if (col <= 8) {
                    // Outer vertex — shared at chunk edges
                    int globalRow = cy * 8 + row;
                    int globalCol = cx * 8 + col;
                    chunk.heightMap.heights[i] = gridNoise(globalRow, globalCol);
                } else {
                    // Inner vertex (quad center) — not shared, offset grid
                    int innerCol = col - 9;
                    int globalRow = cy * 16 + row * 2 + 1;
                    int globalCol = cx * 16 + innerCol * 2 + 1;
                    chunk.heightMap.heights[i] = gridNoise(globalRow, globalCol);
                }
            }

            // Normals pointing up (will be recalculated by renderer)
            for (int i = 0; i < 145; i++) {
                chunk.normals[i * 3 + 0] = 0;
                chunk.normals[i * 3 + 1] = 0;
                chunk.normals[i * 3 + 2] = 127;
            }

            // Base texture layer
            pipeline::TextureLayer layer{};
            layer.textureId = 0;
            layer.flags = 0;
            layer.offsetMCAL = 0;
            layer.effectId = 0;
            chunk.layers.push_back(layer);
        }
    }

    // Biome textures
    terrain.textures.push_back(biomeTextures.base);
    terrain.textures.push_back(biomeTextures.secondary);
    terrain.textures.push_back(biomeTextures.accent);
    terrain.textures.push_back(biomeTextures.detail);

    return terrain;
}

glm::vec3 TerrainEditor::chunkVertexWorldPos(int chunkIdx, int vertIdx) const {
    const auto& chunk = terrain_->chunks[chunkIdx];
    int tileX = terrain_->coord.x;
    int tileY = terrain_->coord.y;
    int cx = chunkIdx % 16;
    int cy = chunkIdx / 16;

    float tileNW_renderX = (32.0f - static_cast<float>(tileY)) * TILE_SIZE;
    float tileNW_renderY = (32.0f - static_cast<float>(tileX)) * TILE_SIZE;
    float chunkBaseX = tileNW_renderX - static_cast<float>(cy) * CHUNK_SIZE;
    float chunkBaseY = tileNW_renderY - static_cast<float>(cx) * CHUNK_SIZE;
    float chunkBaseZ = chunk.position[2];

    int row = vertIdx / 17;
    int col = vertIdx % 17;
    float offsetX = static_cast<float>(col);
    float offsetY = static_cast<float>(row);
    if (col > 8) {
        offsetY += 0.5f;
        offsetX -= 8.5f;
    }

    float unitSize = CHUNK_SIZE / 8.0f;
    float x = chunkBaseX - offsetY * unitSize;
    float y = chunkBaseY - offsetX * unitSize;
    float z = chunkBaseZ + chunk.heightMap.heights[vertIdx];

    return glm::vec3(x, y, z);
}

float TerrainEditor::getVertexHeight(int chunkIdx, int vertIdx) const {
    return terrain_->chunks[chunkIdx].heightMap.heights[vertIdx];
}

void TerrainEditor::setVertexHeight(int chunkIdx, int vertIdx, float height) {
    terrain_->chunks[chunkIdx].heightMap.heights[vertIdx] = height;
}

bool TerrainEditor::raycastTerrain(const rendering::Ray& ray, glm::vec3& hitPos) const {
    if (!terrain_) return false;

    float bestT = 1e30f;
    bool hit = false;

    for (int chunkIdx = 0; chunkIdx < 256; chunkIdx++) {
        const auto& chunk = terrain_->chunks[chunkIdx];
        if (!chunk.hasHeightMap()) continue;

        // Quick AABB check: compute chunk bounds in render space
        glm::vec3 corner0 = chunkVertexWorldPos(chunkIdx, 0);
        glm::vec3 corner1 = chunkVertexWorldPos(chunkIdx, 144);
        glm::vec3 minB = glm::min(corner0, corner1) - glm::vec3(0, 0, 200);
        glm::vec3 maxB = glm::max(corner0, corner1) + glm::vec3(0, 0, 200);

        // Simple AABB-ray test
        float tmin = -1e30f, tmax = 1e30f;
        for (int i = 0; i < 3; i++) {
            if (std::abs(ray.direction[i]) < 1e-8f) {
                if (ray.origin[i] < minB[i] || ray.origin[i] > maxB[i]) { tmin = 1e30f; break; }
            } else {
                float t1 = (minB[i] - ray.origin[i]) / ray.direction[i];
                float t2 = (maxB[i] - ray.origin[i]) / ray.direction[i];
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);
            }
        }
        if (tmin > tmax || tmax < 0) continue;

        // Triangle intersection for each quad
        for (int qy = 0; qy < 8; qy++) {
            for (int qx = 0; qx < 8; qx++) {
                int center = 9 + qy * 17 + qx;
                int tl = center - 9;
                int tr = center - 8;
                int bl = center + 8;
                int br = center + 9;

                int tris[4][3] = {{center, tl, tr}, {center, tr, br}, {center, br, bl}, {center, bl, tl}};
                for (auto& tri : tris) {
                    glm::vec3 v0 = chunkVertexWorldPos(chunkIdx, tri[0]);
                    glm::vec3 v1 = chunkVertexWorldPos(chunkIdx, tri[1]);
                    glm::vec3 v2 = chunkVertexWorldPos(chunkIdx, tri[2]);

                    // Moller-Trumbore intersection
                    glm::vec3 e1 = v1 - v0;
                    glm::vec3 e2 = v2 - v0;
                    glm::vec3 h = glm::cross(ray.direction, e2);
                    float a = glm::dot(e1, h);
                    if (std::abs(a) < 1e-8f) continue;

                    float f = 1.0f / a;
                    glm::vec3 s = ray.origin - v0;
                    float u = f * glm::dot(s, h);
                    if (u < 0.0f || u > 1.0f) continue;

                    glm::vec3 q = glm::cross(s, e1);
                    float v = f * glm::dot(ray.direction, q);
                    if (v < 0.0f || u + v > 1.0f) continue;

                    float t = f * glm::dot(e2, q);
                    if (t > 0.001f && t < bestT) {
                        bestT = t;
                        hitPos = ray.origin + ray.direction * t;
                        hit = true;
                    }
                }
            }
        }
    }

    return hit;
}

std::vector<int> TerrainEditor::getAffectedChunks(const glm::vec3& center, float radius) const {
    std::vector<int> result;
    for (int i = 0; i < 256; i++) {
        if (!terrain_->chunks[i].hasHeightMap()) continue;
        // Check if any vertex in chunk is within radius
        glm::vec3 c0 = chunkVertexWorldPos(i, 0);
        glm::vec3 c1 = chunkVertexWorldPos(i, 144);
        glm::vec3 chunkCenter = (c0 + c1) * 0.5f;
        float chunkRadius = glm::length(c1 - c0) * 0.5f;
        if (glm::length(glm::vec2(chunkCenter.x - center.x, chunkCenter.y - center.y)) < radius + chunkRadius)
            result.push_back(i);
    }
    return result;
}

void TerrainEditor::beginStroke() {
    if (!terrain_ || strokeActive_) return;
    strokeActive_ = true;

    auto affected = getAffectedChunks(brush_.getPosition(), brush_.settings().radius);
    // Capture all chunks that could be affected during the entire stroke
    std::vector<int> allChunks(256);
    std::iota(allChunks.begin(), allChunks.end(), 0);
    std::vector<int> valid;
    for (int i : allChunks) {
        if (terrain_->chunks[i].hasHeightMap()) valid.push_back(i);
    }
    history_.beginEdit(*terrain_, valid);
}

void TerrainEditor::endStroke() {
    if (!strokeActive_) return;
    strokeActive_ = false;
    history_.endEdit(*terrain_);
}

void TerrainEditor::applyBrush(float deltaTime) {
    if (!terrain_ || !brush_.isActive()) return;

    switch (brush_.settings().mode) {
        case BrushMode::Raise: applyRaise(deltaTime); break;
        case BrushMode::Lower: applyRaise(deltaTime); break;
        case BrushMode::Smooth: applySmooth(deltaTime); break;
        case BrushMode::Flatten:
        case BrushMode::Level: applyFlatten(deltaTime); break;
    }
}

void TerrainEditor::applyRaise(float dt) {
    float amount = brush_.settings().strength * dt;
    if (brush_.settings().mode == BrushMode::Lower) amount = -amount;

    auto affected = getAffectedChunks(brush_.getPosition(), brush_.settings().radius);
    for (int chunkIdx : affected) {
        bool modified = false;
        for (int v = 0; v < 145; v++) {
            glm::vec3 pos = chunkVertexWorldPos(chunkIdx, v);
            float dist = glm::length(glm::vec2(pos.x - brush_.getPosition().x,
                                                pos.y - brush_.getPosition().y));
            float influence = brush_.getInfluence(dist);
            if (influence > 0.0f) {
                float h = getVertexHeight(chunkIdx, v);
                setVertexHeight(chunkIdx, v, h + amount * influence);
                modified = true;
            }
        }
        if (modified) {
            stitchEdges(chunkIdx);
            if (std::find(dirtyChunks_.begin(), dirtyChunks_.end(), chunkIdx) == dirtyChunks_.end())
                dirtyChunks_.push_back(chunkIdx);
            dirty_ = true;
        }
    }
}

void TerrainEditor::applySmooth(float dt) {
    float factor = std::min(1.0f, brush_.settings().strength * dt * 0.5f);

    auto affected = getAffectedChunks(brush_.getPosition(), brush_.settings().radius);

    // Build a snapshot of all heights so we read from consistent state
    std::array<std::array<float, 145>, 256> snapshot;
    for (int ci : affected)
        for (int v = 0; v < 145; v++)
            snapshot[ci][v] = getVertexHeight(ci, v);

    // Helper: get height of vertex at global outer grid position,
    // looking across chunk boundaries
    auto getGlobalOuterHeight = [&](int chunkIdx, int row, int col) -> float {
        int cx = chunkIdx % 16;
        int cy = chunkIdx / 16;

        // If within chunk bounds, return directly
        if (row >= 0 && row <= 8 && col >= 0 && col <= 8) {
            int vi = row * 17 + col;
            return snapshot[chunkIdx][vi];
        }

        // Cross into adjacent chunk
        int ncx = cx, ncy = cy;
        int nr = row, nc = col;
        if (row < 0) { ncy = cy - 1; nr = 8; }
        if (row > 8) { ncy = cy + 1; nr = 0; }
        if (col < 0) { ncx = cx - 1; nc = 8; }
        if (col > 8) { ncx = cx + 1; nc = 0; }

        if (ncx < 0 || ncx > 15 || ncy < 0 || ncy > 15)
            return snapshot[chunkIdx][std::clamp(row, 0, 8) * 17 + std::clamp(col, 0, 8)];

        int nci = ncy * 16 + ncx;
        if (!terrain_->chunks[nci].hasHeightMap())
            return snapshot[chunkIdx][std::clamp(row, 0, 8) * 17 + std::clamp(col, 0, 8)];

        int vi = nr * 17 + nc;
        return snapshot[nci][vi];
    };

    for (int chunkIdx : affected) {
        bool modified = false;
        for (int v = 0; v < 145; v++) {
            glm::vec3 pos = chunkVertexWorldPos(chunkIdx, v);
            float dist = glm::length(glm::vec2(pos.x - brush_.getPosition().x,
                                                pos.y - brush_.getPosition().y));
            float influence = brush_.getInfluence(dist);
            if (influence <= 0.0f) continue;

            int row = v / 17;
            int col = v % 17;

            float sum = 0.0f;
            int count = 0;

            if (col <= 8) {
                // Outer vertex — sample 4 neighbors, crossing chunk borders
                int dirs[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (auto& d : dirs) {
                    sum += getGlobalOuterHeight(chunkIdx, row + d[0], col + d[1]);
                    count++;
                }
            } else {
                // Inner vertex — use same-chunk neighbors only
                int neighbors[] = {v - 17, v + 17, v - 1, v + 1};
                for (int n : neighbors) {
                    if (n >= 0 && n < 145) {
                        sum += snapshot[chunkIdx][n];
                        count++;
                    }
                }
            }

            if (count > 0) {
                float avg = sum / static_cast<float>(count);
                float h = snapshot[chunkIdx][v];
                float newH = h + (avg - h) * factor * influence;
                if (newH != h) {
                    setVertexHeight(chunkIdx, v, newH);
                    modified = true;
                }
            }
        }

        if (modified) {
            stitchEdges(chunkIdx);
            if (std::find(dirtyChunks_.begin(), dirtyChunks_.end(), chunkIdx) == dirtyChunks_.end())
                dirtyChunks_.push_back(chunkIdx);
            dirty_ = true;
        }
    }
}

void TerrainEditor::stitchEdges(int chunkIdx) {
    int cx = chunkIdx % 16;
    int cy = chunkIdx / 16;

    auto pushDirty = [&](int idx) {
        if (std::find(dirtyChunks_.begin(), dirtyChunks_.end(), idx) == dirtyChunks_.end())
            dirtyChunks_.push_back(idx);
    };

    if (cx < 15) {
        int n = cy * 16 + cx + 1;
        if (terrain_->chunks[n].hasHeightMap()) {
            for (int r = 0; r <= 8; r++)
                setVertexHeight(n, r * 17, getVertexHeight(chunkIdx, r * 17 + 8));
            pushDirty(n);
        }
    }
    if (cx > 0) {
        int n = cy * 16 + cx - 1;
        if (terrain_->chunks[n].hasHeightMap()) {
            for (int r = 0; r <= 8; r++)
                setVertexHeight(n, r * 17 + 8, getVertexHeight(chunkIdx, r * 17));
            pushDirty(n);
        }
    }
    if (cy < 15) {
        int n = (cy + 1) * 16 + cx;
        if (terrain_->chunks[n].hasHeightMap()) {
            for (int c = 0; c <= 8; c++)
                setVertexHeight(n, c, getVertexHeight(chunkIdx, 8 * 17 + c));
            pushDirty(n);
        }
    }
    if (cy > 0) {
        int n = (cy - 1) * 16 + cx;
        if (terrain_->chunks[n].hasHeightMap()) {
            for (int c = 0; c <= 8; c++)
                setVertexHeight(n, 8 * 17 + c, getVertexHeight(chunkIdx, c));
            pushDirty(n);
        }
    }
}

void TerrainEditor::applyFlatten(float dt) {
    float factor = std::min(1.0f, brush_.settings().strength * dt * 0.3f);
    float targetH = brush_.settings().flattenHeight;

    auto affected = getAffectedChunks(brush_.getPosition(), brush_.settings().radius);
    for (int chunkIdx : affected) {
        bool modified = false;
        for (int v = 0; v < 145; v++) {
            glm::vec3 pos = chunkVertexWorldPos(chunkIdx, v);
            float dist = glm::length(glm::vec2(pos.x - brush_.getPosition().x,
                                                pos.y - brush_.getPosition().y));
            float influence = brush_.getInfluence(dist);
            if (influence <= 0.0f) continue;

            float h = getVertexHeight(chunkIdx, v);
            // targetH is absolute world Z; heights are relative to chunk base
            float relTarget = targetH - terrain_->chunks[chunkIdx].position[2];
            float newH = h + (relTarget - h) * factor * influence;
            if (newH != h) {
                setVertexHeight(chunkIdx, v, newH);
                modified = true;
            }
        }
        if (modified) {
            stitchEdges(chunkIdx);
            if (std::find(dirtyChunks_.begin(), dirtyChunks_.end(), chunkIdx) == dirtyChunks_.end())
                dirtyChunks_.push_back(chunkIdx);
            dirty_ = true;
        }
    }
}

std::vector<int> TerrainEditor::consumeDirtyChunks() {
    std::vector<int> result;
    result.swap(dirtyChunks_);
    return result;
}

pipeline::TerrainMesh TerrainEditor::regenerateMesh() const {
    if (!terrain_) return {};
    return pipeline::TerrainMeshGenerator::generate(*terrain_);
}

pipeline::ChunkMesh TerrainEditor::regenerateChunkMesh(int chunkIndex) const {
    if (!terrain_) return {};
    auto mesh = pipeline::TerrainMeshGenerator::generate(*terrain_);
    return mesh.chunks[chunkIndex];
}

void TerrainEditor::undo() {
    if (!terrain_) return;
    history_.undo(*terrain_);
    for (int idx : history_.lastAffectedChunks()) {
        if (std::find(dirtyChunks_.begin(), dirtyChunks_.end(), idx) == dirtyChunks_.end())
            dirtyChunks_.push_back(idx);
    }
}

void TerrainEditor::redo() {
    if (!terrain_) return;
    history_.redo(*terrain_);
    for (int idx : history_.lastAffectedChunks()) {
        if (std::find(dirtyChunks_.begin(), dirtyChunks_.end(), idx) == dirtyChunks_.end())
            dirtyChunks_.push_back(idx);
    }
}

void TerrainEditor::setWaterLevel(const glm::vec3& center, float radius,
                                   float waterHeight, uint16_t liquidType) {
    if (!terrain_) return;

    auto affected = getAffectedChunks(center, radius);
    for (int chunkIdx : affected) {
        auto& water = terrain_->waterData[chunkIdx];

        if (water.layers.empty()) {
            pipeline::ADTTerrain::WaterLayer wl;
            wl.liquidType = liquidType;
            wl.flags = 0;
            wl.minHeight = waterHeight;
            wl.maxHeight = waterHeight;
            wl.x = 0;
            wl.y = 0;
            wl.width = 9;
            wl.height = 9;
            wl.heights.assign(81, waterHeight);
            wl.mask.assign(8, 0xFF);
            water.layers.push_back(wl);
        } else {
            auto& wl = water.layers[0];
            wl.minHeight = waterHeight;
            wl.maxHeight = waterHeight;
            wl.liquidType = liquidType;
            std::fill(wl.heights.begin(), wl.heights.end(), waterHeight);
        }

        if (std::find(dirtyChunks_.begin(), dirtyChunks_.end(), chunkIdx) == dirtyChunks_.end())
            dirtyChunks_.push_back(chunkIdx);
        dirty_ = true;
    }
}

void TerrainEditor::removeWater(const glm::vec3& center, float radius) {
    if (!terrain_) return;

    auto affected = getAffectedChunks(center, radius);
    for (int chunkIdx : affected) {
        terrain_->waterData[chunkIdx].layers.clear();
        if (std::find(dirtyChunks_.begin(), dirtyChunks_.end(), chunkIdx) == dirtyChunks_.end())
            dirtyChunks_.push_back(chunkIdx);
        dirty_ = true;
    }
}

} // namespace editor
} // namespace wowee
