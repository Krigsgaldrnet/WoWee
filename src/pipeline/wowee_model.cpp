#include "pipeline/wowee_model.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <filesystem>
#include <cstring>

namespace wowee {
namespace pipeline {

static constexpr uint32_t WOM_MAGIC = 0x314D4F57; // "WOM1"

bool WoweeModelLoader::exists(const std::string& basePath) {
    return std::filesystem::exists(basePath + ".wom");
}

WoweeModel WoweeModelLoader::load(const std::string& basePath) {
    WoweeModel model;
    std::string womPath = basePath + ".wom";

    std::ifstream f(womPath, std::ios::binary);
    if (!f) return model;

    uint32_t magic;
    f.read(reinterpret_cast<char*>(&magic), 4);
    if (magic != WOM_MAGIC) return model;

    uint32_t vertCount, indexCount, texCount;
    f.read(reinterpret_cast<char*>(&vertCount), 4);
    f.read(reinterpret_cast<char*>(&indexCount), 4);
    f.read(reinterpret_cast<char*>(&texCount), 4);
    f.read(reinterpret_cast<char*>(&model.boundRadius), 4);
    f.read(reinterpret_cast<char*>(&model.boundMin), 12);
    f.read(reinterpret_cast<char*>(&model.boundMax), 12);

    // Read name
    uint16_t nameLen;
    f.read(reinterpret_cast<char*>(&nameLen), 2);
    model.name.resize(nameLen);
    f.read(model.name.data(), nameLen);

    // Read vertices
    model.vertices.resize(vertCount);
    f.read(reinterpret_cast<char*>(model.vertices.data()),
           vertCount * sizeof(WoweeModel::Vertex));

    // Read indices
    model.indices.resize(indexCount);
    f.read(reinterpret_cast<char*>(model.indices.data()),
           indexCount * sizeof(uint32_t));

    // Read texture paths
    for (uint32_t i = 0; i < texCount; i++) {
        uint16_t pathLen;
        f.read(reinterpret_cast<char*>(&pathLen), 2);
        std::string path(pathLen, '\0');
        f.read(path.data(), pathLen);
        model.texturePaths.push_back(path);
    }

    LOG_INFO("WOM loaded: ", basePath, " (", vertCount, " verts, ",
             indexCount / 3, " tris)");
    return model;
}

bool WoweeModelLoader::save(const WoweeModel& model, const std::string& basePath) {
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(basePath).parent_path());

    std::string womPath = basePath + ".wom";
    std::ofstream f(womPath, std::ios::binary);
    if (!f) return false;

    f.write(reinterpret_cast<const char*>(&WOM_MAGIC), 4);
    uint32_t vertCount = static_cast<uint32_t>(model.vertices.size());
    uint32_t indexCount = static_cast<uint32_t>(model.indices.size());
    uint32_t texCount = static_cast<uint32_t>(model.texturePaths.size());
    f.write(reinterpret_cast<const char*>(&vertCount), 4);
    f.write(reinterpret_cast<const char*>(&indexCount), 4);
    f.write(reinterpret_cast<const char*>(&texCount), 4);
    f.write(reinterpret_cast<const char*>(&model.boundRadius), 4);
    f.write(reinterpret_cast<const char*>(&model.boundMin), 12);
    f.write(reinterpret_cast<const char*>(&model.boundMax), 12);

    uint16_t nameLen = static_cast<uint16_t>(model.name.size());
    f.write(reinterpret_cast<const char*>(&nameLen), 2);
    f.write(model.name.data(), nameLen);

    f.write(reinterpret_cast<const char*>(model.vertices.data()),
            vertCount * sizeof(WoweeModel::Vertex));
    f.write(reinterpret_cast<const char*>(model.indices.data()),
            indexCount * sizeof(uint32_t));

    for (const auto& path : model.texturePaths) {
        uint16_t pathLen = static_cast<uint16_t>(path.size());
        f.write(reinterpret_cast<const char*>(&pathLen), 2);
        f.write(path.data(), pathLen);
    }

    LOG_INFO("WOM saved: ", womPath, " (", vertCount, " verts, ",
             indexCount / 3, " tris)");
    return true;
}

WoweeModel WoweeModelLoader::fromM2(const std::string& m2Path, AssetManager* am) {
    WoweeModel model;
    if (!am) return model;

    auto data = am->readFile(m2Path);
    if (data.empty()) return model;

    auto m2 = M2Loader::load(data);

    // Load skin file for WotLK M2s
    if (!m2.isValid()) {
        std::string skinPath = m2Path;
        auto dotPos = skinPath.rfind('.');
        if (dotPos != std::string::npos)
            skinPath = skinPath.substr(0, dotPos) + "00.skin";
        auto skinData = am->readFile(skinPath);
        if (!skinData.empty())
            M2Loader::loadSkin(skinData, m2);
    }

    if (!m2.isValid()) return model;

    model.name = m2.name;
    model.boundRadius = m2.boundRadius;

    // Convert M2 vertices to WOM format (strip bone data)
    model.vertices.reserve(m2.vertices.size());
    for (const auto& v : m2.vertices) {
        WoweeModel::Vertex wv;
        wv.position = v.position;
        wv.normal = v.normal;
        wv.texCoord = v.texCoords[0];
        model.vertices.push_back(wv);

        model.boundMin = glm::min(model.boundMin, v.position);
        model.boundMax = glm::max(model.boundMax, v.position);
    }

    // Convert indices (M2 uses uint16, WOM uses uint32)
    model.indices.reserve(m2.indices.size());
    for (uint16_t idx : m2.indices)
        model.indices.push_back(static_cast<uint32_t>(idx));

    // Convert texture paths (BLP → PNG)
    for (const auto& tex : m2.textures) {
        std::string path = tex.filename;
        auto dot = path.rfind('.');
        if (dot != std::string::npos)
            path = path.substr(0, dot) + ".png";
        model.texturePaths.push_back(path);
    }

    return model;
}

} // namespace pipeline
} // namespace wowee
