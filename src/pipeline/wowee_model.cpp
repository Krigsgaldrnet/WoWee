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
static constexpr uint32_t WOM2_MAGIC = 0x324D4F57; // "WOM2"

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
    bool isV2 = (magic == WOM2_MAGIC);
    if (magic != WOM_MAGIC && magic != WOM2_MAGIC) return model;
    model.version = isV2 ? 2 : 1;

    uint32_t vertCount, indexCount, texCount;
    f.read(reinterpret_cast<char*>(&vertCount), 4);
    f.read(reinterpret_cast<char*>(&indexCount), 4);
    f.read(reinterpret_cast<char*>(&texCount), 4);
    f.read(reinterpret_cast<char*>(&model.boundRadius), 4);
    f.read(reinterpret_cast<char*>(&model.boundMin), 12);
    f.read(reinterpret_cast<char*>(&model.boundMax), 12);

    uint16_t nameLen;
    f.read(reinterpret_cast<char*>(&nameLen), 2);
    model.name.resize(nameLen);
    f.read(model.name.data(), nameLen);

    // Read vertices (WOM1: 32 bytes/vert, WOM2: 40 bytes with bone data)
    model.vertices.resize(vertCount);
    if (isV2) {
        f.read(reinterpret_cast<char*>(model.vertices.data()),
               vertCount * sizeof(WoweeModel::Vertex));
    } else {
        // WOM1 backward compat: read 32-byte vertices (no bone data)
        struct V1Vertex { glm::vec3 pos; glm::vec3 norm; glm::vec2 uv; };
        for (uint32_t i = 0; i < vertCount; i++) {
            V1Vertex v1;
            f.read(reinterpret_cast<char*>(&v1), sizeof(V1Vertex));
            model.vertices[i].position = v1.pos;
            model.vertices[i].normal = v1.norm;
            model.vertices[i].texCoord = v1.uv;
        }
    }

    model.indices.resize(indexCount);
    f.read(reinterpret_cast<char*>(model.indices.data()), indexCount * 4);

    for (uint32_t i = 0; i < texCount; i++) {
        uint16_t pathLen;
        f.read(reinterpret_cast<char*>(&pathLen), 2);
        std::string path(pathLen, '\0');
        f.read(path.data(), pathLen);
        model.texturePaths.push_back(path);
    }

    // WOM2: read bones and animations
    if (isV2) {
        uint32_t boneCount = 0;
        if (f.read(reinterpret_cast<char*>(&boneCount), 4) && boneCount > 0 && boneCount <= 512) {
            model.bones.resize(boneCount);
            for (uint32_t bi = 0; bi < boneCount; bi++) {
                auto& bone = model.bones[bi];
                f.read(reinterpret_cast<char*>(&bone.keyBoneId), 4);
                f.read(reinterpret_cast<char*>(&bone.parentBone), 2);
                f.read(reinterpret_cast<char*>(&bone.pivot), 12);
                f.read(reinterpret_cast<char*>(&bone.flags), 4);
            }
        }

        uint32_t animCount = 0;
        if (f.read(reinterpret_cast<char*>(&animCount), 4) && animCount > 0 && animCount <= 1024) {
            model.animations.resize(animCount);
            for (uint32_t ai = 0; ai < animCount; ai++) {
                auto& anim = model.animations[ai];
                f.read(reinterpret_cast<char*>(&anim.id), 4);
                f.read(reinterpret_cast<char*>(&anim.durationMs), 4);
                f.read(reinterpret_cast<char*>(&anim.movingSpeed), 4);

                anim.boneKeyframes.resize(model.bones.size());
                for (size_t bi = 0; bi < model.bones.size(); bi++) {
                    uint32_t kfCount = 0;
                    f.read(reinterpret_cast<char*>(&kfCount), 4);
                    for (uint32_t ki = 0; ki < kfCount && ki < 10000; ki++) {
                        WoweeModel::AnimKeyframe kf;
                        f.read(reinterpret_cast<char*>(&kf.timeMs), 4);
                        f.read(reinterpret_cast<char*>(&kf.translation), 12);
                        f.read(reinterpret_cast<char*>(&kf.rotation), 16);
                        f.read(reinterpret_cast<char*>(&kf.scale), 12);
                        anim.boneKeyframes[bi].push_back(kf);
                    }
                }
            }
        }
    }

    LOG_INFO("WOM", (isV2 ? "2" : "1"), " loaded: ", basePath, " (", vertCount, " verts, ",
             model.bones.size(), " bones, ", model.animations.size(), " anims)");
    return model;
}

bool WoweeModelLoader::save(const WoweeModel& model, const std::string& basePath) {
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(basePath).parent_path());

    std::string womPath = basePath + ".wom";
    std::ofstream f(womPath, std::ios::binary);
    if (!f) return false;

    bool hasAnim = model.hasAnimation();
    uint32_t magic = hasAnim ? WOM2_MAGIC : WOM_MAGIC;
    f.write(reinterpret_cast<const char*>(&magic), 4);

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

    // WOM2 writes full vertex with bone data; WOM1 writes 32-byte vertex
    if (hasAnim) {
        f.write(reinterpret_cast<const char*>(model.vertices.data()),
                vertCount * sizeof(WoweeModel::Vertex));
    } else {
        for (const auto& v : model.vertices) {
            f.write(reinterpret_cast<const char*>(&v.position), 12);
            f.write(reinterpret_cast<const char*>(&v.normal), 12);
            f.write(reinterpret_cast<const char*>(&v.texCoord), 8);
        }
    }

    f.write(reinterpret_cast<const char*>(model.indices.data()), indexCount * 4);

    for (const auto& path : model.texturePaths) {
        uint16_t pathLen = static_cast<uint16_t>(path.size());
        f.write(reinterpret_cast<const char*>(&pathLen), 2);
        f.write(path.data(), pathLen);
    }

    // WOM2: write bones and animations
    if (hasAnim) {
        uint32_t boneCount = static_cast<uint32_t>(model.bones.size());
        f.write(reinterpret_cast<const char*>(&boneCount), 4);
        for (const auto& bone : model.bones) {
            f.write(reinterpret_cast<const char*>(&bone.keyBoneId), 4);
            f.write(reinterpret_cast<const char*>(&bone.parentBone), 2);
            f.write(reinterpret_cast<const char*>(&bone.pivot), 12);
            f.write(reinterpret_cast<const char*>(&bone.flags), 4);
        }

        uint32_t animCount = static_cast<uint32_t>(model.animations.size());
        f.write(reinterpret_cast<const char*>(&animCount), 4);
        for (const auto& anim : model.animations) {
            f.write(reinterpret_cast<const char*>(&anim.id), 4);
            f.write(reinterpret_cast<const char*>(&anim.durationMs), 4);
            f.write(reinterpret_cast<const char*>(&anim.movingSpeed), 4);

            for (size_t bi = 0; bi < model.bones.size(); bi++) {
                uint32_t kfCount = (bi < anim.boneKeyframes.size())
                    ? static_cast<uint32_t>(anim.boneKeyframes[bi].size()) : 0;
                f.write(reinterpret_cast<const char*>(&kfCount), 4);
                for (uint32_t ki = 0; ki < kfCount; ki++) {
                    const auto& kf = anim.boneKeyframes[bi][ki];
                    f.write(reinterpret_cast<const char*>(&kf.timeMs), 4);
                    f.write(reinterpret_cast<const char*>(&kf.translation), 12);
                    f.write(reinterpret_cast<const char*>(&kf.rotation), 16);
                    f.write(reinterpret_cast<const char*>(&kf.scale), 12);
                }
            }
        }
    }

    LOG_INFO("WOM", (hasAnim ? "2" : "1"), " saved: ", womPath, " (", vertCount, " verts, ",
             model.bones.size(), " bones, ", model.animations.size(), " anims)");
    return true;
}

WoweeModel WoweeModelLoader::fromM2(const std::string& m2Path, AssetManager* am) {
    WoweeModel model;
    if (!am) return model;

    auto data = am->readFile(m2Path);
    if (data.empty()) return model;

    auto m2 = M2Loader::load(data);

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

    // Convert vertices with bone data
    model.vertices.reserve(m2.vertices.size());
    for (const auto& v : m2.vertices) {
        WoweeModel::Vertex wv;
        wv.position = v.position;
        wv.normal = v.normal;
        wv.texCoord = v.texCoords[0];
        std::memcpy(wv.boneWeights, v.boneWeights, 4);
        std::memcpy(wv.boneIndices, v.boneIndices, 4);
        model.vertices.push_back(wv);

        model.boundMin = glm::min(model.boundMin, v.position);
        model.boundMax = glm::max(model.boundMax, v.position);
    }

    model.indices.reserve(m2.indices.size());
    for (uint16_t idx : m2.indices)
        model.indices.push_back(static_cast<uint32_t>(idx));

    for (const auto& tex : m2.textures) {
        std::string path = tex.filename;
        auto dot = path.rfind('.');
        if (dot != std::string::npos)
            path = path.substr(0, dot) + ".png";
        model.texturePaths.push_back(path);
    }

    // Convert bones
    for (const auto& b : m2.bones) {
        WoweeModel::Bone wb;
        wb.keyBoneId = b.keyBoneId;
        wb.parentBone = b.parentBone;
        wb.pivot = b.pivot;
        wb.flags = b.flags;
        model.bones.push_back(wb);
    }

    // Convert animations (first keyframe per bone per sequence)
    for (const auto& seq : m2.sequences) {
        WoweeModel::Animation anim;
        anim.id = seq.id;
        anim.durationMs = seq.duration;
        anim.movingSpeed = seq.movingSpeed;
        anim.boneKeyframes.resize(model.bones.size());

        for (size_t bi = 0; bi < m2.bones.size() && bi < model.bones.size(); bi++) {
            const auto& bone = m2.bones[bi];
            // Find keyframes for this sequence index
            uint32_t seqIdx = static_cast<uint32_t>(&seq - m2.sequences.data());

            auto extractKeys = [&](const M2AnimationTrack& track, size_t boneIdx) {
                if (seqIdx >= track.sequences.size()) return;
                const auto& sk = track.sequences[seqIdx];
                for (size_t ki = 0; ki < sk.timestamps.size(); ki++) {
                    // Check if we already have this timestamp
                    bool found = false;
                    for (auto& existing : anim.boneKeyframes[boneIdx]) {
                        if (existing.timeMs == sk.timestamps[ki]) {
                            if (!sk.vec3Values.empty() && ki < sk.vec3Values.size()) {
                                if (&track == &bone.translation)
                                    existing.translation = sk.vec3Values[ki];
                                else
                                    existing.scale = sk.vec3Values[ki];
                            }
                            if (!sk.quatValues.empty() && ki < sk.quatValues.size())
                                existing.rotation = sk.quatValues[ki];
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        WoweeModel::AnimKeyframe kf;
                        kf.timeMs = sk.timestamps[ki];
                        kf.translation = glm::vec3(0);
                        kf.rotation = glm::quat(1, 0, 0, 0);
                        kf.scale = glm::vec3(1);
                        if (!sk.vec3Values.empty() && ki < sk.vec3Values.size()) {
                            if (&track == &bone.translation)
                                kf.translation = sk.vec3Values[ki];
                            else
                                kf.scale = sk.vec3Values[ki];
                        }
                        if (!sk.quatValues.empty() && ki < sk.quatValues.size())
                            kf.rotation = sk.quatValues[ki];
                        anim.boneKeyframes[boneIdx].push_back(kf);
                    }
                }
            };

            extractKeys(bone.translation, bi);
            extractKeys(bone.rotation, bi);
            extractKeys(bone.scale, bi);
        }

        if (anim.durationMs > 0) model.animations.push_back(anim);
    }

    model.version = model.hasAnimation() ? 2 : 1;
    return model;
}

} // namespace pipeline
} // namespace wowee
