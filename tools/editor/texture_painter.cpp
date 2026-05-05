#include "texture_painter.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>

namespace wowee {
namespace editor {

void TexturePainter::setActiveTexture(const std::string& texturePath) {
    activeTexture_ = texturePath;
}

uint32_t TexturePainter::ensureTextureInList(const std::string& path) {
    for (uint32_t i = 0; i < terrain_->textures.size(); i++) {
        if (terrain_->textures[i] == path) return i;
    }
    terrain_->textures.push_back(path);
    return static_cast<uint32_t>(terrain_->textures.size() - 1);
}

int TexturePainter::ensureLayerOnChunk(int chunkIdx, uint32_t textureId) {
    auto& chunk = terrain_->chunks[chunkIdx];

    for (int i = 0; i < static_cast<int>(chunk.layers.size()); i++) {
        if (chunk.layers[i].textureId == textureId) return i;
    }

    if (chunk.layers.size() < 4) {
        pipeline::TextureLayer layer{};
        layer.textureId = textureId;
        layer.flags = 0x100;
        layer.offsetMCAL = static_cast<uint32_t>(chunk.alphaMap.size());
        layer.effectId = 0;
        chunk.layers.push_back(layer);
        chunk.alphaMap.resize(chunk.alphaMap.size() + 4096, 0);
        return static_cast<int>(chunk.layers.size() - 1);
    }

    // At 4 layers — find the non-base layer with lowest total alpha and replace it
    int weakest = -1;
    int weakestSum = INT32_MAX;
    for (int i = 1; i < static_cast<int>(chunk.layers.size()); i++) {
        if (chunk.layers[i].textureId == textureId) return i;
        size_t off = chunk.layers[i].offsetMCAL;
        if (off + 4096 > chunk.alphaMap.size()) continue;
        int sum = 0;
        for (int j = 0; j < 4096; j++) sum += chunk.alphaMap[off + j];
        if (sum < weakestSum) { weakestSum = sum; weakest = i; }
    }

    if (weakest < 0) return -1;

    // Replace the weakest layer
    chunk.layers[weakest].textureId = textureId;
    size_t off = chunk.layers[weakest].offsetMCAL;
    std::fill(chunk.alphaMap.begin() + off, chunk.alphaMap.begin() + off + 4096, 0);
    return weakest;
}

glm::vec2 TexturePainter::worldToChunkUV(int chunkIdx, const glm::vec3& worldPos) const {
    int cx = chunkIdx % 16;
    int cy = chunkIdx / 16;
    int tileX = terrain_->coord.x;
    int tileY = terrain_->coord.y;

    float tileNW_X = (32.0f - static_cast<float>(tileY)) * TILE_SIZE;
    float tileNW_Y = (32.0f - static_cast<float>(tileX)) * TILE_SIZE;
    float chunkBaseX = tileNW_X - static_cast<float>(cy) * CHUNK_SIZE;
    float chunkBaseY = tileNW_Y - static_cast<float>(cx) * CHUNK_SIZE;

    // UV: 0,0 at chunk NW corner, 1,1 at SE corner
    float u = (chunkBaseX - worldPos.x) / CHUNK_SIZE;
    float v = (chunkBaseY - worldPos.y) / CHUNK_SIZE;
    return glm::vec2(u, v);
}

void TexturePainter::modifyAlpha(int chunkIdx, int layerIdx, const glm::vec3& center,
                                  float radius, float strength, float falloff, bool erasing) {
    auto& chunk = terrain_->chunks[chunkIdx];
    auto& layer = chunk.layers[layerIdx];

    // Find alpha data offset for this layer
    size_t alphaOffset = layer.offsetMCAL;
    if (alphaOffset + 4096 > chunk.alphaMap.size()) return;

    int cx = chunkIdx % 16;
    int cy = chunkIdx / 16;
    int tileX = terrain_->coord.x;
    int tileY = terrain_->coord.y;

    float tileNW_X = (32.0f - static_cast<float>(tileY)) * TILE_SIZE;
    float tileNW_Y = (32.0f - static_cast<float>(tileX)) * TILE_SIZE;
    float chunkBaseX = tileNW_X - static_cast<float>(cy) * CHUNK_SIZE;
    float chunkBaseY = tileNW_Y - static_cast<float>(cx) * CHUNK_SIZE;

    float texelSize = CHUNK_SIZE / 64.0f;

    for (int ty = 0; ty < 64; ty++) {
        for (int tx = 0; tx < 64; tx++) {
            // World position of this alpha texel
            float wx = chunkBaseX - (static_cast<float>(ty) + 0.5f) * texelSize;
            float wy = chunkBaseY - (static_cast<float>(tx) + 0.5f) * texelSize;

            float dist = std::sqrt((wx - center.x) * (wx - center.x) +
                                   (wy - center.y) * (wy - center.y));
            if (dist >= radius) continue;

            // Falloff
            float t = dist / radius;
            float innerRadius = 1.0f - falloff;
            float influence = 1.0f;
            if (t > innerRadius && falloff > 0.001f) {
                float ft = (t - innerRadius) / falloff;
                influence = 1.0f - ft * ft;
            }

            size_t idx = alphaOffset + ty * 64 + tx;
            float current = static_cast<float>(chunk.alphaMap[idx]) / 255.0f;
            float delta = strength * influence;

            float newVal;
            if (erasing)
                newVal = std::max(0.0f, current - delta);
            else
                newVal = std::min(1.0f, current + delta);

            chunk.alphaMap[idx] = static_cast<uint8_t>(newVal * 255.0f);
        }
    }
}

std::vector<int> TexturePainter::paint(const glm::vec3& center, float radius,
                                        float strength, float falloff) {
    if (!terrain_ || activeTexture_.empty()) return {};

    uint32_t texId = ensureTextureInList(activeTexture_);
    std::vector<int> modified;

    for (int i = 0; i < 256; i++) {
        if (!terrain_->chunks[i].hasHeightMap()) continue;

        // Quick distance check from chunk center
        int cx = i % 16;
        int cy = i / 16;
        float tileNW_X = (32.0f - static_cast<float>(terrain_->coord.y)) * TILE_SIZE;
        float tileNW_Y = (32.0f - static_cast<float>(terrain_->coord.x)) * TILE_SIZE;
        float chunkCenterX = tileNW_X - (cy + 0.5f) * CHUNK_SIZE;
        float chunkCenterY = tileNW_Y - (cx + 0.5f) * CHUNK_SIZE;
        float dist = std::sqrt((chunkCenterX - center.x) * (chunkCenterX - center.x) +
                               (chunkCenterY - center.y) * (chunkCenterY - center.y));
        if (dist > radius + CHUNK_SIZE) continue;

        int layerIdx = ensureLayerOnChunk(i, texId);
        if (layerIdx < 0) continue; // chunk full

        modifyAlpha(i, layerIdx, center, radius, strength, falloff, false);
        modified.push_back(i);
    }

    return modified;
}

std::vector<int> TexturePainter::erase(const glm::vec3& center, float radius,
                                        float strength, float falloff) {
    if (!terrain_ || activeTexture_.empty()) return {};

    std::vector<int> modified;

    for (int i = 0; i < 256; i++) {
        if (!terrain_->chunks[i].hasHeightMap()) continue;
        auto& chunk = terrain_->chunks[i];

        int cx = i % 16;
        int cy = i / 16;
        float tileNW_X = (32.0f - static_cast<float>(terrain_->coord.y)) * TILE_SIZE;
        float tileNW_Y = (32.0f - static_cast<float>(terrain_->coord.x)) * TILE_SIZE;
        float chunkCenterX = tileNW_X - (cy + 0.5f) * CHUNK_SIZE;
        float chunkCenterY = tileNW_Y - (cx + 0.5f) * CHUNK_SIZE;
        float dist = std::sqrt((chunkCenterX - center.x) * (chunkCenterX - center.x) +
                               (chunkCenterY - center.y) * (chunkCenterY - center.y));
        if (dist > radius + CHUNK_SIZE) continue;

        // Erase all non-base layers in range
        for (int l = 1; l < static_cast<int>(chunk.layers.size()); l++) {
            modifyAlpha(i, l, center, radius, strength, falloff, true);
        }
        modified.push_back(i);
    }

    return modified;
}

} // namespace editor
} // namespace wowee
