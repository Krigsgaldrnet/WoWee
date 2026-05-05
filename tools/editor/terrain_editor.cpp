#include "terrain_editor.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
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

        // Quick AABB check using actual vertex extent
        glm::vec3 corner0 = chunkVertexWorldPos(chunkIdx, 0);
        glm::vec3 corner1 = chunkVertexWorldPos(chunkIdx, 144);
        glm::vec3 minB = glm::min(corner0, corner1);
        glm::vec3 maxB = glm::max(corner0, corner1);
        // Expand Z by actual height range in chunk
        float minH = chunk.heightMap.heights[0], maxH = minH;
        for (int h = 1; h < 145; h++) {
            minH = std::min(minH, chunk.heightMap.heights[h]);
            maxH = std::max(maxH, chunk.heightMap.heights[h]);
        }
        minB.z = chunk.position[2] + minH - 10.0f;
        maxB.z = chunk.position[2] + maxH + 10.0f;

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
        case BrushMode::Erode: applyErode(deltaTime); break;
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

void TerrainEditor::recalcNormals(const std::vector<int>& chunkIndices) {
    if (!terrain_) return;
    float unitSize = CHUNK_SIZE / 8.0f;

    for (int ci : chunkIndices) {
        auto& chunk = terrain_->chunks[ci];
        if (!chunk.hasHeightMap()) continue;

        for (int i = 0; i < 145; i++) {
            int row = i / 17;
            int col = i % 17;

            // Get heights of neighbors
            float hC = chunk.heightMap.heights[i];
            float hL = hC, hR = hC, hU = hC, hD = hC;

            if (col <= 8) {
                // Outer vertex
                int li = row * 17 + std::max(0, col - 1);
                int ri = row * 17 + std::min(8, col + 1);
                int ui = std::max(0, row - 1) * 17 + col;
                int di = std::min(8, row + 1) * 17 + col;
                hL = chunk.heightMap.heights[li];
                hR = chunk.heightMap.heights[ri];
                hU = chunk.heightMap.heights[ui];
                hD = chunk.heightMap.heights[di];
            } else {
                // Inner vertex — use adjacent outer verts
                int innerCol = col - 9;
                int tl = row * 17 + innerCol;
                int tr = row * 17 + innerCol + 1;
                int bl = (row + 1) * 17 + innerCol;
                int br = (row + 1) * 17 + innerCol + 1;
                if (tl >= 0 && tl < 145) hU = chunk.heightMap.heights[tl];
                if (tr >= 0 && tr < 145) hR = chunk.heightMap.heights[tr];
                if (bl >= 0 && bl < 145) hD = chunk.heightMap.heights[bl];
                if (br >= 0 && br < 145) hL = chunk.heightMap.heights[br];
            }

            // Compute normal from height differences
            float dx = (hL - hR) / (2.0f * unitSize);
            float dy = (hU - hD) / (2.0f * unitSize);
            float len = std::sqrt(dx * dx + dy * dy + 1.0f);
            glm::vec3 n(dx / len, dy / len, 1.0f / len);

            chunk.normals[i * 3 + 0] = static_cast<int8_t>(n.x * 127.0f);
            chunk.normals[i * 3 + 1] = static_cast<int8_t>(n.y * 127.0f);
            chunk.normals[i * 3 + 2] = static_cast<int8_t>(n.z * 127.0f);
        }
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

void TerrainEditor::applyErode(float dt) {
    float factor = std::min(1.0f, brush_.settings().strength * dt * 0.3f);

    auto affected = getAffectedChunks(brush_.getPosition(), brush_.settings().radius);
    for (int chunkIdx : affected) {
        bool modified = false;
        auto& chunk = terrain_->chunks[chunkIdx];
        for (int v = 0; v < 145; v++) {
            glm::vec3 pos = chunkVertexWorldPos(chunkIdx, v);
            float dist = glm::length(glm::vec2(pos.x - brush_.getPosition().x,
                                                pos.y - brush_.getPosition().y));
            float influence = brush_.getInfluence(dist);
            if (influence <= 0.0f) continue;

            float h = chunk.heightMap.heights[v];
            int row = v / 17, col = v % 17;

            // Find lowest neighbor (same chunk)
            float lowestH = h;
            if (col <= 8) {
                int neighbors[] = {v - 17, v + 17, v - 1, v + 1};
                for (int n : neighbors) {
                    if (n >= 0 && n < 145)
                        lowestH = std::min(lowestH, chunk.heightMap.heights[n]);
                }
            }

            // Move height toward lowest neighbor (erosion)
            float slope = h - lowestH;
            if (slope > 0.1f) {
                float erosion = slope * factor * influence * 0.3f;
                chunk.heightMap.heights[v] -= erosion;
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

void TerrainEditor::applyNoise(float frequency, float amplitude, int octaves, uint32_t seed) {
    if (!terrain_) return;

    // Simple value noise with octaves
    auto hash2d = [](int x, int y, uint32_t s) -> float {
        uint32_t h = static_cast<uint32_t>(x * 374761393 + y * 668265263 + s * 1274126177);
        h = (h ^ (h >> 13)) * 1274126177;
        h = h ^ (h >> 16);
        return static_cast<float>(h & 0xFFFF) / 65535.0f * 2.0f - 1.0f;
    };

    auto smoothNoise = [&](float fx, float fy, uint32_t s) -> float {
        int ix = static_cast<int>(std::floor(fx));
        int iy = static_cast<int>(std::floor(fy));
        float fracX = fx - ix;
        float fracY = fy - iy;
        // Smoothstep
        fracX = fracX * fracX * (3.0f - 2.0f * fracX);
        fracY = fracY * fracY * (3.0f - 2.0f * fracY);
        float v00 = hash2d(ix, iy, s);
        float v10 = hash2d(ix + 1, iy, s);
        float v01 = hash2d(ix, iy + 1, s);
        float v11 = hash2d(ix + 1, iy + 1, s);
        float i0 = v00 + (v10 - v00) * fracX;
        float i1 = v01 + (v11 - v01) * fracX;
        return i0 + (i1 - i0) * fracY;
    };

    for (int ci = 0; ci < 256; ci++) {
        auto& chunk = terrain_->chunks[ci];
        if (!chunk.hasHeightMap()) continue;
        int cx = ci % 16, cy = ci / 16;

        for (int v = 0; v < 145; v++) {
            glm::vec3 wpos = chunkVertexWorldPos(ci, v);

            float total = 0.0f;
            float amp = amplitude;
            float freq = frequency;
            for (int o = 0; o < octaves; o++) {
                total += smoothNoise(wpos.x * freq, wpos.y * freq, seed + o * 97) * amp;
                freq *= 2.0f;
                amp *= 0.5f;
            }
            chunk.heightMap.heights[v] += total;
        }
        dirtyChunks_.push_back(ci);
    }
    dirty_ = true;
}

bool TerrainEditor::importHeightmap(const std::string& path, float heightScale) {
    if (!terrain_) return false;

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { return false; }
    auto fileSize = f.tellg();
    f.seekg(0);

    // Determine resolution from file size
    // 129x129 x 2 bytes = 33282 (one chunk row+1 per tile row+1)
    // 257x257 x 2 bytes = 132098 (2 samples per chunk quad)
    int res = 0;
    if (fileSize >= 132098) res = 257;
    else if (fileSize >= 33282) res = 129;
    else if (fileSize >= 16641) { res = 129; } // 8-bit 129x129
    else return false;

    bool is16bit = (fileSize >= res * res * 2);
    std::vector<float> heightData(res * res);

    if (is16bit) {
        std::vector<uint16_t> raw(res * res);
        f.read(reinterpret_cast<char*>(raw.data()), res * res * 2);
        for (int i = 0; i < res * res; i++)
            heightData[i] = static_cast<float>(raw[i]) / 65535.0f;
    } else {
        std::vector<uint8_t> raw(res * res);
        f.read(reinterpret_cast<char*>(raw.data()), res * res);
        for (int i = 0; i < res * res; i++)
            heightData[i] = static_cast<float>(raw[i]) / 255.0f;
    }

    // Map heightmap pixels to terrain vertices
    for (int cy = 0; cy < 16; cy++) {
        for (int cx = 0; cx < 16; cx++) {
            auto& chunk = terrain_->chunks[cy * 16 + cx];
            if (!chunk.hasHeightMap()) continue;

            for (int v = 0; v < 145; v++) {
                int row = v / 17, col = v % 17;
                float offX = static_cast<float>(col);
                float offY = static_cast<float>(row);
                if (col > 8) { offY += 0.5f; offX -= 8.5f; }

                // Map to pixel coords
                float px = (cx * 8.0f + offX) / 128.0f * (res - 1);
                float py = (cy * 8.0f + offY) / 128.0f * (res - 1);
                int ix = std::clamp(static_cast<int>(px), 0, res - 1);
                int iy = std::clamp(static_cast<int>(py), 0, res - 1);

                chunk.heightMap.heights[v] = heightData[iy * res + ix] * heightScale;
            }
            dirtyChunks_.push_back(cy * 16 + cx);
        }
    }
    dirty_ = true;
    return true;
}

void TerrainEditor::punchHole(const glm::vec3& center, float radius) {
    if (!terrain_) return;
    auto affected = getAffectedChunks(center, radius);
    for (int ci : affected) {
        auto& chunk = terrain_->chunks[ci];
        // Each chunk has 8x8 quads, holes use a 4x4 bitmask (each bit covers 2x2 quads)
        for (int hy = 0; hy < 4; hy++) {
            for (int hx = 0; hx < 4; hx++) {
                // Center of this 2x2 quad group
                int cx = ci % 16, cy = ci / 16;
                float tileNW_X = (32.0f - static_cast<float>(terrain_->coord.y)) * TILE_SIZE;
                float tileNW_Y = (32.0f - static_cast<float>(terrain_->coord.x)) * TILE_SIZE;
                float qx = tileNW_X - cy * CHUNK_SIZE - (hy * 2 + 1) * CHUNK_SIZE / 8.0f;
                float qy = tileNW_Y - cx * CHUNK_SIZE - (hx * 2 + 1) * CHUNK_SIZE / 8.0f;
                float dist = std::sqrt((qx - center.x) * (qx - center.x) +
                                       (qy - center.y) * (qy - center.y));
                if (dist < radius) {
                    int bit = 1 << (hy * 4 + hx);
                    chunk.holes |= static_cast<uint16_t>(bit);
                }
            }
        }
        if (std::find(dirtyChunks_.begin(), dirtyChunks_.end(), ci) == dirtyChunks_.end())
            dirtyChunks_.push_back(ci);
        dirty_ = true;
    }
}

void TerrainEditor::fillHole(const glm::vec3& center, float radius) {
    if (!terrain_) return;
    auto affected = getAffectedChunks(center, radius);
    for (int ci : affected) {
        auto& chunk = terrain_->chunks[ci];
        for (int hy = 0; hy < 4; hy++) {
            for (int hx = 0; hx < 4; hx++) {
                int cx = ci % 16, cy = ci / 16;
                float tileNW_X = (32.0f - static_cast<float>(terrain_->coord.y)) * TILE_SIZE;
                float tileNW_Y = (32.0f - static_cast<float>(terrain_->coord.x)) * TILE_SIZE;
                float qx = tileNW_X - cy * CHUNK_SIZE - (hy * 2 + 1) * CHUNK_SIZE / 8.0f;
                float qy = tileNW_Y - cx * CHUNK_SIZE - (hx * 2 + 1) * CHUNK_SIZE / 8.0f;
                float dist = std::sqrt((qx - center.x) * (qx - center.x) +
                                       (qy - center.y) * (qy - center.y));
                if (dist < radius) {
                    int bit = 1 << (hy * 4 + hx);
                    chunk.holes &= ~static_cast<uint16_t>(bit);
                }
            }
        }
        if (std::find(dirtyChunks_.begin(), dirtyChunks_.end(), ci) == dirtyChunks_.end())
            dirtyChunks_.push_back(ci);
        dirty_ = true;
    }
}

} // namespace editor
} // namespace wowee
