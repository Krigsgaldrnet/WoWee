#include "cli_mesh_info.hpp"

#include "pipeline/wowee_model.hpp"
#include "pipeline/wowee_building.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

int handleInfoZoneModelsTotal(int& i, int argc, char** argv) {
    // Aggregate WOM/WOB stats across every model in a zone.
    // Useful for capacity planning ('how many bones across all
    // my creatures?') and perf budgeting ('total triangles
    // per frame if all loaded?').
    std::string zoneDir = argv[++i];
    bool jsonOut = (i + 1 < argc &&
                    std::strcmp(argv[i + 1], "--json") == 0);
    if (jsonOut) i++;
    namespace fs = std::filesystem;
    if (!fs::exists(zoneDir)) {
        std::fprintf(stderr,
            "info-zone-models-total: %s does not exist\n", zoneDir.c_str());
        return 1;
    }
    int womCount = 0, wobCount = 0;
    uint64_t womVerts = 0, womIndices = 0;
    uint64_t womBones = 0, womAnims = 0, womBatches = 0;
    uint64_t wobGroups = 0, wobVerts = 0, wobIndices = 0;
    uint64_t wobDoodads = 0, wobPortals = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        std::string base = e.path().string();
        if (base.size() > ext.size())
            base = base.substr(0, base.size() - ext.size());
        if (ext == ".wom") {
            womCount++;
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            womVerts += wom.vertices.size();
            womIndices += wom.indices.size();
            womBones += wom.bones.size();
            womAnims += wom.animations.size();
            womBatches += wom.batches.size();
        } else if (ext == ".wob") {
            wobCount++;
            auto wob = wowee::pipeline::WoweeBuildingLoader::load(base);
            wobGroups += wob.groups.size();
            for (const auto& g : wob.groups) {
                wobVerts += g.vertices.size();
                wobIndices += g.indices.size();
            }
            wobDoodads += wob.doodads.size();
            wobPortals += wob.portals.size();
        }
    }
    if (jsonOut) {
        nlohmann::json j;
        j["zone"] = zoneDir;
        j["wom"] = {{"count", womCount},
                     {"vertices", womVerts},
                     {"indices", womIndices},
                     {"triangles", womIndices / 3},
                     {"bones", womBones},
                     {"animations", womAnims},
                     {"batches", womBatches}};
        j["wob"] = {{"count", wobCount},
                     {"groups", wobGroups},
                     {"vertices", wobVerts},
                     {"indices", wobIndices},
                     {"triangles", wobIndices / 3},
                     {"doodads", wobDoodads},
                     {"portals", wobPortals}};
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("Zone models total: %s\n", zoneDir.c_str());
    std::printf("\n  WOM (open M2):\n");
    std::printf("    files     : %d\n", womCount);
    std::printf("    vertices  : %llu\n", static_cast<unsigned long long>(womVerts));
    std::printf("    triangles : %llu\n", static_cast<unsigned long long>(womIndices / 3));
    std::printf("    bones     : %llu\n", static_cast<unsigned long long>(womBones));
    std::printf("    anims     : %llu\n", static_cast<unsigned long long>(womAnims));
    std::printf("    batches   : %llu\n", static_cast<unsigned long long>(womBatches));
    std::printf("\n  WOB (open WMO):\n");
    std::printf("    files     : %d\n", wobCount);
    std::printf("    groups    : %llu\n", static_cast<unsigned long long>(wobGroups));
    std::printf("    vertices  : %llu\n", static_cast<unsigned long long>(wobVerts));
    std::printf("    triangles : %llu\n", static_cast<unsigned long long>(wobIndices / 3));
    std::printf("    doodads   : %llu\n", static_cast<unsigned long long>(wobDoodads));
    std::printf("    portals   : %llu\n", static_cast<unsigned long long>(wobPortals));
    std::printf("\n  Combined  :\n");
    std::printf("    vertices  : %llu\n", static_cast<unsigned long long>(womVerts + wobVerts));
    std::printf("    triangles : %llu\n", static_cast<unsigned long long>((womIndices + wobIndices) / 3));
    return 0;
}

int handleListZoneMeshesDetail(int& i, int argc, char** argv) {
    // Per-mesh breakdown of every .wom file in <zoneDir>,
    // sorted by triangle count descending so the heaviest
    // meshes float to the top. Complements
    // --list-zone-meshes (per-zone summary) by surfacing
    // individual mesh metrics — useful for spotting
    // outliers ("which mesh is using 80% of my triangle
    // budget?") and for content audits.
    std::string zoneDir = argv[++i];
    bool jsonOut = (i + 1 < argc &&
                    std::strcmp(argv[i + 1], "--json") == 0);
    if (jsonOut) i++;
    namespace fs = std::filesystem;
    if (!fs::exists(zoneDir)) {
        std::fprintf(stderr,
            "list-zone-meshes-detail: %s does not exist\n", zoneDir.c_str());
        return 1;
    }
    struct Row {
        std::string path;
        size_t verts;
        size_t tris;
        size_t bones;
        size_t batches;
        size_t textures;
        uint64_t bytes;
        uint32_t version;
    };
    std::vector<Row> rows;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".wom") continue;
        std::string base = e.path().string();
        if (base.size() >= 4) base = base.substr(0, base.size() - 4);
        auto wom = wowee::pipeline::WoweeModelLoader::load(base);
        Row r;
        r.path = fs::relative(e.path(), zoneDir, ec).string();
        if (ec) r.path = e.path().filename().string();
        r.verts = wom.vertices.size();
        r.tris = wom.indices.size() / 3;
        r.bones = wom.bones.size();
        r.batches = wom.batches.size();
        r.textures = wom.texturePaths.size();
        r.bytes = e.file_size(ec);
        if (ec) r.bytes = 0;
        r.version = wom.version;
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.tris > b.tris; });
    uint64_t totVerts = 0, totTris = 0, totBones = 0, totBytes = 0;
    for (const auto& r : rows) {
        totVerts += r.verts; totTris += r.tris;
        totBones += r.bones; totBytes += r.bytes;
    }
    if (jsonOut) {
        nlohmann::json j;
        j["zone"] = zoneDir;
        j["meshCount"] = rows.size();
        j["totals"] = {{"vertices", totVerts},
                        {"triangles", totTris},
                        {"bones", totBones},
                        {"bytes", totBytes}};
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : rows) {
            arr.push_back({{"path", r.path},
                            {"version", r.version},
                            {"vertices", r.verts},
                            {"triangles", r.tris},
                            {"bones", r.bones},
                            {"batches", r.batches},
                            {"textures", r.textures},
                            {"bytes", r.bytes}});
        }
        j["meshes"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("Zone meshes: %s\n", zoneDir.c_str());
    std::printf("  meshes : %zu\n", rows.size());
    std::printf("  totals : %llu verts, %llu tris, %llu bones, %.1f KB\n",
                static_cast<unsigned long long>(totVerts),
                static_cast<unsigned long long>(totTris),
                static_cast<unsigned long long>(totBones),
                totBytes / 1024.0);
    if (rows.empty()) {
        std::printf("\n  *no .wom files in this zone*\n");
        return 0;
    }
    std::printf("\n  v   verts    tris   bones  batch  tex    bytes  path\n");
    for (const auto& r : rows) {
        std::printf("  v%u %6zu  %6zu  %5zu  %5zu  %3zu  %7llu  %s\n",
                    r.version, r.verts, r.tris, r.bones,
                    r.batches, r.textures,
                    static_cast<unsigned long long>(r.bytes),
                    r.path.c_str());
    }
    return 0;
}

int handleInfoMesh(int& i, int argc, char** argv) {
    // Single-mesh detail view aggregating bounds, version,
    // batches, bones, animations, and texture slots into one
    // report. Composite of what --info-batches / --info-bones
    // / --info-batches show separately. Useful authoring
    // command: pass a WOM and see everything about it without
    // running three sub-commands.
    std::string base = argv[++i];
    bool jsonOut = (i + 1 < argc &&
                    std::strcmp(argv[i + 1], "--json") == 0);
    if (jsonOut) i++;
    if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom") {
        base = base.substr(0, base.size() - 4);
    }
    if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
        std::fprintf(stderr,
            "info-mesh: %s.wom does not exist\n", base.c_str());
        return 1;
    }
    auto wom = wowee::pipeline::WoweeModelLoader::load(base);
    if (!wom.isValid()) {
        std::fprintf(stderr,
            "info-mesh: failed to load %s.wom\n", base.c_str());
        return 1;
    }
    // Per-batch material summary.
    static const char* blendNames[] = {
        "opaque", "alpha-test", "alpha", "additive", "?", "?", "?", "?"
    };
    if (jsonOut) {
        nlohmann::json j;
        j["base"] = base;
        j["name"] = wom.name;
        j["version"] = wom.version;
        j["bounds"] = {{"min", {wom.boundMin.x, wom.boundMin.y, wom.boundMin.z}},
                        {"max", {wom.boundMax.x, wom.boundMax.y, wom.boundMax.z}},
                        {"radius", wom.boundRadius}};
        j["counts"] = {{"vertices", wom.vertices.size()},
                        {"indices", wom.indices.size()},
                        {"triangles", wom.indices.size() / 3},
                        {"bones", wom.bones.size()},
                        {"animations", wom.animations.size()},
                        {"batches", wom.batches.size()},
                        {"textures", wom.texturePaths.size()}};
        nlohmann::json bs = nlohmann::json::array();
        for (const auto& b : wom.batches) {
            std::string tex;
            if (b.textureIndex < wom.texturePaths.size())
                tex = wom.texturePaths[b.textureIndex];
            bs.push_back({{"indexStart", b.indexStart},
                           {"indexCount", b.indexCount},
                           {"triangles", b.indexCount / 3},
                           {"textureIndex", b.textureIndex},
                           {"texture", tex},
                           {"blendMode", b.blendMode},
                           {"flags", b.flags}});
        }
        j["batchDetail"] = bs;
        j["texturePaths"] = wom.texturePaths;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("Mesh: %s.wom\n", base.c_str());
    std::printf("  name       : %s\n", wom.name.c_str());
    std::printf("  version    : v%u\n", wom.version);
    std::printf("\n  Counts:\n");
    std::printf("    vertices  : %zu\n", wom.vertices.size());
    std::printf("    triangles : %zu\n", wom.indices.size() / 3);
    std::printf("    bones     : %zu\n", wom.bones.size());
    std::printf("    anims     : %zu\n", wom.animations.size());
    std::printf("    batches   : %zu\n", wom.batches.size());
    std::printf("    textures  : %zu\n", wom.texturePaths.size());
    std::printf("\n  Bounds:\n");
    std::printf("    min       : (%.3f, %.3f, %.3f)\n",
                wom.boundMin.x, wom.boundMin.y, wom.boundMin.z);
    std::printf("    max       : (%.3f, %.3f, %.3f)\n",
                wom.boundMax.x, wom.boundMax.y, wom.boundMax.z);
    std::printf("    radius    : %.3f\n", wom.boundRadius);
    if (!wom.batches.empty()) {
        std::printf("\n  Batches:\n");
        std::printf("    idx  iStart  iCount  tris   blend       texture\n");
        for (size_t k = 0; k < wom.batches.size(); ++k) {
            const auto& b = wom.batches[k];
            std::string tex = "<oob>";
            if (b.textureIndex < wom.texturePaths.size())
                tex = wom.texturePaths[b.textureIndex];
            if (tex.empty()) tex = "(empty)";
            int blend = b.blendMode < 8 ? b.blendMode : 0;
            std::printf("    %3zu  %6u  %6u  %4u   %-10s  %s\n",
                        k, b.indexStart, b.indexCount,
                        b.indexCount / 3, blendNames[blend],
                        tex.c_str());
        }
    }
    if (!wom.texturePaths.empty()) {
        std::printf("\n  Texture slots:\n");
        for (size_t k = 0; k < wom.texturePaths.size(); ++k) {
            std::printf("    [%zu] %s\n", k,
                        wom.texturePaths[k].empty()
                        ? "(empty placeholder)"
                        : wom.texturePaths[k].c_str());
        }
    }
    return 0;
}


}  // namespace

bool handleMeshInfo(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--info-zone-models-total") == 0 && i + 1 < argc) {
        outRc = handleInfoZoneModelsTotal(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--list-zone-meshes-detail") == 0 && i + 1 < argc) {
        outRc = handleListZoneMeshesDetail(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-mesh") == 0 && i + 1 < argc) {
        outRc = handleInfoMesh(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
