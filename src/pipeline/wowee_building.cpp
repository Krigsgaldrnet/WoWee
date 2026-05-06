#include "pipeline/wowee_building.hpp"
#include "pipeline/wmo_loader.hpp"
#include "core/logger.hpp"
#include <glm/gtc/quaternion.hpp>
#include <fstream>
#include <filesystem>
#include <cstring>

namespace wowee {
namespace pipeline {

static constexpr uint32_t WOB_MAGIC = 0x31424F57; // "WOB1"

bool WoweeBuildingLoader::exists(const std::string& basePath) {
    return std::filesystem::exists(basePath + ".wob");
}

WoweeBuilding WoweeBuildingLoader::load(const std::string& basePath) {
    WoweeBuilding bld;
    std::ifstream f(basePath + ".wob", std::ios::binary);
    if (!f) return bld;

    uint32_t magic;
    f.read(reinterpret_cast<char*>(&magic), 4);
    if (magic != WOB_MAGIC) return bld;

    uint32_t groupCount, portalCount, doodadCount;
    f.read(reinterpret_cast<char*>(&groupCount), 4);
    f.read(reinterpret_cast<char*>(&portalCount), 4);
    f.read(reinterpret_cast<char*>(&doodadCount), 4);
    f.read(reinterpret_cast<char*>(&bld.boundRadius), 4);

    uint16_t nameLen;
    f.read(reinterpret_cast<char*>(&nameLen), 2);
    bld.name.resize(nameLen);
    f.read(bld.name.data(), nameLen);

    for (uint32_t gi = 0; gi < groupCount; gi++) {
        WoweeBuilding::Group grp;
        uint16_t gnLen;
        f.read(reinterpret_cast<char*>(&gnLen), 2);
        grp.name.resize(gnLen);
        f.read(grp.name.data(), gnLen);

        uint32_t vc, ic, tc;
        f.read(reinterpret_cast<char*>(&vc), 4);
        f.read(reinterpret_cast<char*>(&ic), 4);
        f.read(reinterpret_cast<char*>(&tc), 4);
        uint8_t outdoor;
        f.read(reinterpret_cast<char*>(&outdoor), 1);
        grp.isOutdoor = (outdoor != 0);
        f.read(reinterpret_cast<char*>(&grp.boundMin), 12);
        f.read(reinterpret_cast<char*>(&grp.boundMax), 12);

        grp.vertices.resize(vc);
        f.read(reinterpret_cast<char*>(grp.vertices.data()), vc * sizeof(WoweeBuilding::Vertex));
        grp.indices.resize(ic);
        f.read(reinterpret_cast<char*>(grp.indices.data()), ic * 4);

        for (uint32_t ti = 0; ti < tc; ti++) {
            uint16_t tl;
            f.read(reinterpret_cast<char*>(&tl), 2);
            std::string tp(tl, '\0');
            f.read(tp.data(), tl);
            grp.texturePaths.push_back(tp);
        }

        // Read material data (v1.1+)
        uint32_t mc = 0;
        if (f.read(reinterpret_cast<char*>(&mc), 4) && mc > 0 && mc <= 256) {
            for (uint32_t mi = 0; mi < mc; mi++) {
                WoweeBuilding::Material mat;
                uint16_t pl;
                f.read(reinterpret_cast<char*>(&pl), 2);
                mat.texturePath.resize(pl);
                f.read(mat.texturePath.data(), pl);
                f.read(reinterpret_cast<char*>(&mat.flags), 4);
                f.read(reinterpret_cast<char*>(&mat.shader), 4);
                f.read(reinterpret_cast<char*>(&mat.blendMode), 4);
                grp.materials.push_back(mat);
            }
        }

        bld.groups.push_back(std::move(grp));
    }

    for (uint32_t pi = 0; pi < portalCount; pi++) {
        WoweeBuilding::Portal portal;
        f.read(reinterpret_cast<char*>(&portal.groupA), 4);
        f.read(reinterpret_cast<char*>(&portal.groupB), 4);
        uint32_t pvCount;
        f.read(reinterpret_cast<char*>(&pvCount), 4);
        portal.vertices.resize(pvCount);
        f.read(reinterpret_cast<char*>(portal.vertices.data()), pvCount * 12);
        bld.portals.push_back(portal);
    }

    for (uint32_t di = 0; di < doodadCount; di++) {
        WoweeBuilding::DoodadPlacement dp;
        uint16_t pl;
        f.read(reinterpret_cast<char*>(&pl), 2);
        dp.modelPath.resize(pl);
        f.read(dp.modelPath.data(), pl);
        f.read(reinterpret_cast<char*>(&dp.position), 12);
        f.read(reinterpret_cast<char*>(&dp.rotation), 12);
        f.read(reinterpret_cast<char*>(&dp.scale), 4);
        bld.doodads.push_back(dp);
    }

    LOG_INFO("WOB loaded: ", basePath, " (", groupCount, " groups, ",
             portalCount, " portals, ", doodadCount, " doodads)");
    return bld;
}

bool WoweeBuildingLoader::save(const WoweeBuilding& bld, const std::string& basePath) {
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(basePath).parent_path());

    std::ofstream f(basePath + ".wob", std::ios::binary);
    if (!f) return false;

    f.write(reinterpret_cast<const char*>(&WOB_MAGIC), 4);
    uint32_t gc = static_cast<uint32_t>(bld.groups.size());
    uint32_t pc = static_cast<uint32_t>(bld.portals.size());
    uint32_t dc = static_cast<uint32_t>(bld.doodads.size());
    f.write(reinterpret_cast<const char*>(&gc), 4);
    f.write(reinterpret_cast<const char*>(&pc), 4);
    f.write(reinterpret_cast<const char*>(&dc), 4);
    f.write(reinterpret_cast<const char*>(&bld.boundRadius), 4);

    uint16_t nl = static_cast<uint16_t>(bld.name.size());
    f.write(reinterpret_cast<const char*>(&nl), 2);
    f.write(bld.name.data(), nl);

    for (const auto& grp : bld.groups) {
        uint16_t gnl = static_cast<uint16_t>(grp.name.size());
        f.write(reinterpret_cast<const char*>(&gnl), 2);
        f.write(grp.name.data(), gnl);

        uint32_t vc = static_cast<uint32_t>(grp.vertices.size());
        uint32_t ic = static_cast<uint32_t>(grp.indices.size());
        uint32_t tc = static_cast<uint32_t>(grp.texturePaths.size());
        f.write(reinterpret_cast<const char*>(&vc), 4);
        f.write(reinterpret_cast<const char*>(&ic), 4);
        f.write(reinterpret_cast<const char*>(&tc), 4);
        uint8_t outdoor = grp.isOutdoor ? 1 : 0;
        f.write(reinterpret_cast<const char*>(&outdoor), 1);
        f.write(reinterpret_cast<const char*>(&grp.boundMin), 12);
        f.write(reinterpret_cast<const char*>(&grp.boundMax), 12);

        f.write(reinterpret_cast<const char*>(grp.vertices.data()),
                vc * sizeof(WoweeBuilding::Vertex));
        f.write(reinterpret_cast<const char*>(grp.indices.data()), ic * 4);

        for (const auto& tp : grp.texturePaths) {
            uint16_t tl = static_cast<uint16_t>(tp.size());
            f.write(reinterpret_cast<const char*>(&tl), 2);
            f.write(tp.data(), tl);
        }

        // Write material data
        uint32_t mc = static_cast<uint32_t>(grp.materials.size());
        f.write(reinterpret_cast<const char*>(&mc), 4);
        for (const auto& mat : grp.materials) {
            uint16_t pl = static_cast<uint16_t>(mat.texturePath.size());
            f.write(reinterpret_cast<const char*>(&pl), 2);
            f.write(mat.texturePath.data(), pl);
            f.write(reinterpret_cast<const char*>(&mat.flags), 4);
            f.write(reinterpret_cast<const char*>(&mat.shader), 4);
            f.write(reinterpret_cast<const char*>(&mat.blendMode), 4);
        }
    }

    for (const auto& portal : bld.portals) {
        f.write(reinterpret_cast<const char*>(&portal.groupA), 4);
        f.write(reinterpret_cast<const char*>(&portal.groupB), 4);
        uint32_t pvCount = static_cast<uint32_t>(portal.vertices.size());
        f.write(reinterpret_cast<const char*>(&pvCount), 4);
        f.write(reinterpret_cast<const char*>(portal.vertices.data()), pvCount * 12);
    }

    for (const auto& dp : bld.doodads) {
        uint16_t pl = static_cast<uint16_t>(dp.modelPath.size());
        f.write(reinterpret_cast<const char*>(&pl), 2);
        f.write(dp.modelPath.data(), pl);
        f.write(reinterpret_cast<const char*>(&dp.position), 12);
        f.write(reinterpret_cast<const char*>(&dp.rotation), 12);
        f.write(reinterpret_cast<const char*>(&dp.scale), 4);
    }

    LOG_INFO("WOB saved: ", basePath, ".wob (", gc, " groups)");
    return true;
}

bool WoweeBuildingLoader::toWMOModel(const WoweeBuilding& building, WMOModel& outModel) {
    if (building.groups.empty()) return false;

    outModel.nGroups = static_cast<uint32_t>(building.groups.size());
    outModel.groups.clear();
    outModel.textures.clear();
    outModel.materials.clear();
    outModel.portals.clear();
    outModel.portalVertices.clear();
    outModel.portalRefs.clear();

    // Build a global texture index from per-material texturePath strings.
    // First-group materials become the WMO material list; each unique texture
    // gets one entry in outModel.textures.
    auto textureIndex = [&](const std::string& path) -> uint32_t {
        if (path.empty()) return 0;
        for (uint32_t i = 0; i < outModel.textures.size(); i++) {
            if (outModel.textures[i] == path) return i;
        }
        outModel.textures.push_back(path);
        return static_cast<uint32_t>(outModel.textures.size() - 1);
    };

    if (!building.groups.empty()) {
        for (const auto& mat : building.groups[0].materials) {
            WMOMaterial wm{};
            wm.flags = mat.flags;
            wm.shader = mat.shader;
            wm.blendMode = mat.blendMode;
            wm.texture1 = textureIndex(mat.texturePath);
            wm.color1 = 0;
            wm.texture2 = 0;
            wm.color2 = 0;
            wm.texture3 = 0;
            wm.color3 = 0;
            outModel.materials.push_back(wm);
        }
    }

    for (const auto& grp : building.groups) {
        WMOGroup wmoGroup;
        wmoGroup.name = grp.name;
        wmoGroup.boundingBoxMin = grp.boundMin;
        wmoGroup.boundingBoxMax = grp.boundMax;
        if (grp.isOutdoor) wmoGroup.flags |= 0x08;

        wmoGroup.vertices.reserve(grp.vertices.size());
        for (const auto& v : grp.vertices) {
            WMOVertex wv;
            wv.position = v.position;
            wv.normal = v.normal;
            wv.texCoord = v.texCoord;
            wv.color = v.color;
            wmoGroup.vertices.push_back(wv);
        }

        wmoGroup.indices.reserve(grp.indices.size());
        for (uint32_t idx : grp.indices)
            wmoGroup.indices.push_back(static_cast<uint16_t>(idx));

        outModel.groups.push_back(std::move(wmoGroup));
    }

    // Reconstruct portal vertices + refs from WoB's higher-level portal struct.
    for (const auto& wp : building.portals) {
        WMOPortal portal{};
        portal.startVertex = static_cast<uint16_t>(outModel.portalVertices.size());
        portal.vertexCount = static_cast<uint16_t>(wp.vertices.size());
        portal.planeIndex = 0;
        for (const auto& v : wp.vertices) outModel.portalVertices.push_back(v);
        uint16_t portalIdx = static_cast<uint16_t>(outModel.portals.size());
        outModel.portals.push_back(portal);
        if (wp.groupA >= 0) {
            outModel.portalRefs.push_back({portalIdx, static_cast<uint16_t>(wp.groupA), -1, 0});
        }
        if (wp.groupB >= 0) {
            outModel.portalRefs.push_back({portalIdx, static_cast<uint16_t>(wp.groupB), 1, 0});
        }
    }

    // Restore doodads. WMODoodad keys nameIndex by MODN byte offset; we just
    // assign sequential offsets here since none of our paths share suffixes.
    outModel.doodads.clear();
    outModel.doodadNames.clear();
    uint32_t doodadOffset = 0;
    for (const auto& dp : building.doodads) {
        // Convert WOM extension back to M2 for the runtime that may not have a
        // WOM-aware loader for in-WMO doodads yet.
        std::string mp = dp.modelPath;
        auto dot = mp.rfind('.');
        if (dot != std::string::npos) {
            std::string ext = mp.substr(dot + 1);
            if (ext == "wom") mp = mp.substr(0, dot) + ".m2";
        }
        outModel.doodadNames[doodadOffset] = mp;

        WMODoodad d{};
        d.nameIndex = doodadOffset;
        d.position = dp.position;
        // Convert euler degrees -> quaternion (XYZ order)
        glm::vec3 r = glm::radians(dp.rotation);
        glm::quat qx = glm::angleAxis(r.x, glm::vec3(1, 0, 0));
        glm::quat qy = glm::angleAxis(r.y, glm::vec3(0, 1, 0));
        glm::quat qz = glm::angleAxis(r.z, glm::vec3(0, 0, 1));
        d.rotation = qx * qy * qz;
        d.scale = dp.scale;
        d.color = glm::vec4(1.0f);
        outModel.doodads.push_back(d);
        doodadOffset += static_cast<uint32_t>(mp.size() + 1);
    }

    return true;
}

WoweeBuilding WoweeBuildingLoader::fromWMO(const WMOModel& wmo, const std::string& name) {
    WoweeBuilding bld;
    bld.name = name.empty() ? "Converted WMO" : name;

    float maxDist = 0.0f;
    for (const auto& grp : wmo.groups) {
        WoweeBuilding::Group wobGroup;
        wobGroup.name = grp.name;
        wobGroup.isOutdoor = (grp.flags & 0x08) != 0;
        wobGroup.boundMin = grp.boundingBoxMin;
        wobGroup.boundMax = grp.boundingBoxMax;

        wobGroup.vertices.reserve(grp.vertices.size());
        for (const auto& v : grp.vertices) {
            WoweeBuilding::Vertex wv;
            wv.position = v.position;
            wv.normal = v.normal;
            wv.texCoord = v.texCoord;
            wv.color = v.color;
            wobGroup.vertices.push_back(wv);

            float d = glm::length(v.position);
            if (d > maxDist) maxDist = d;
        }

        wobGroup.indices.reserve(grp.indices.size());
        for (uint16_t idx : grp.indices)
            wobGroup.indices.push_back(static_cast<uint32_t>(idx));

        for (const auto& mat : wmo.materials) {
            WoweeBuilding::Material wobMat;
            wobMat.flags = mat.flags;
            wobMat.shader = mat.shader;
            wobMat.blendMode = mat.blendMode;
            if (mat.texture1 < wmo.textures.size()) {
                std::string texPath = wmo.textures[mat.texture1];
                auto dot = texPath.rfind('.');
                if (dot != std::string::npos)
                    texPath = texPath.substr(0, dot) + ".png";
                wobMat.texturePath = texPath;
                wobGroup.texturePaths.push_back(texPath);
            }
            wobGroup.materials.push_back(wobMat);
        }

        bld.groups.push_back(std::move(wobGroup));
    }

    bld.boundRadius = maxDist;

    // Extract portals (vertex polygons + group links via MOPR refs).
    // Each MOPR ref links a portal to one of its two adjacent groups; pairing
    // refs with the same portalIndex gives us groupA/groupB for that portal.
    for (size_t pi = 0; pi < wmo.portals.size(); pi++) {
        const auto& wmoPortal = wmo.portals[pi];
        WoweeBuilding::Portal wp;
        wp.groupA = -1;
        wp.groupB = -1;
        for (const auto& ref : wmo.portalRefs) {
            if (ref.portalIndex != static_cast<uint16_t>(pi)) continue;
            if (wp.groupA < 0) wp.groupA = ref.groupIndex;
            else if (wp.groupB < 0) wp.groupB = ref.groupIndex;
        }
        for (uint16_t vi = 0; vi < wmoPortal.vertexCount; vi++) {
            uint32_t idx = wmoPortal.startVertex + vi;
            if (idx < wmo.portalVertices.size()) {
                wp.vertices.push_back(wmo.portalVertices[idx]);
            }
        }
        if (!wp.vertices.empty()) bld.portals.push_back(std::move(wp));
    }

    for (const auto& doodad : wmo.doodads) {
        auto nameIt = wmo.doodadNames.find(doodad.nameIndex);
        if (nameIt == wmo.doodadNames.end()) continue;

        WoweeBuilding::DoodadPlacement dp;
        dp.modelPath = nameIt->second;
        auto dot = dp.modelPath.rfind('.');
        if (dot != std::string::npos)
            dp.modelPath = dp.modelPath.substr(0, dot) + ".wom";
        dp.position = doodad.position;
        // Convert quaternion rotation to euler angles
        glm::quat q(doodad.rotation.w, doodad.rotation.x,
                     doodad.rotation.y, doodad.rotation.z);
        dp.rotation = glm::degrees(glm::eulerAngles(q));
        dp.scale = doodad.scale;
        bld.doodads.push_back(dp);
    }

    LOG_INFO("WOB from WMO: ", bld.name, " (", bld.groups.size(), " groups, ",
             bld.doodads.size(), " doodads)");
    return bld;
}

} // namespace pipeline
} // namespace wowee
