#include "cli_world_info.hpp"
#include "cli_weld.hpp"

#include "pipeline/wowee_building.hpp"
#include "pipeline/wowee_collision.hpp"
#include "pipeline/wowee_terrain_loader.hpp"
#include "pipeline/adt_loader.hpp"
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

int handleInfoWob(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = (i + 1 < argc &&
                    std::strcmp(argv[i + 1], "--json") == 0);
    if (jsonOut) i++;
    if (base.size() >= 4 && base.substr(base.size() - 4) == ".wob")
        base = base.substr(0, base.size() - 4);
    if (!wowee::pipeline::WoweeBuildingLoader::exists(base)) {
        std::fprintf(stderr, "WOB not found: %s.wob\n", base.c_str());
        return 1;
    }
    auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
    size_t totalVerts = 0, totalIdx = 0, totalMats = 0;
    for (const auto& g : bld.groups) {
        totalVerts += g.vertices.size();
        totalIdx += g.indices.size();
        totalMats += g.materials.size();
    }
    if (jsonOut) {
        nlohmann::json j;
        j["wob"] = base + ".wob";
        j["name"] = bld.name;
        j["groups"] = bld.groups.size();
        j["portals"] = bld.portals.size();
        j["doodads"] = bld.doodads.size();
        j["boundRadius"] = bld.boundRadius;
        j["totalVerts"] = totalVerts;
        j["totalTris"] = totalIdx / 3;
        j["totalMats"] = totalMats;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WOB: %s.wob\n", base.c_str());
    std::printf("  name        : %s\n", bld.name.c_str());
    std::printf("  groups      : %zu\n", bld.groups.size());
    std::printf("  portals     : %zu\n", bld.portals.size());
    std::printf("  doodads     : %zu\n", bld.doodads.size());
    std::printf("  boundRadius : %.2f\n", bld.boundRadius);
    std::printf("  total verts : %zu\n", totalVerts);
    std::printf("  total tris  : %zu\n", totalIdx / 3);
    std::printf("  total mats  : %zu (across all groups)\n", totalMats);
    return 0;
}

int handleInfoWobStats(int& i, int argc, char** argv) {
    // Geometric stats on a WOB building, per-group and aggregated
    // across all groups: triangle count, surface area, watertight
    // check via the same edge analysis as --info-mesh-stats. Pass
    // --weld <eps> to merge per-face vertex duplicates before edge
    // analysis (true topological closure check).
    std::string base = argv[++i];
    bool jsonOut = false;
    bool useWeld = false;
    float weldEps = 1e-5f;
    while (i + 1 < argc && argv[i + 1][0] == '-') {
        if (std::strcmp(argv[i + 1], "--json") == 0) {
            jsonOut = true; ++i;
        } else if (std::strcmp(argv[i + 1], "--weld") == 0 && i + 2 < argc) {
            useWeld = true;
            try { weldEps = std::stof(argv[i + 2]); } catch (...) {}
            i += 2;
        } else {
            break;
        }
    }
    if (base.size() >= 4 && base.substr(base.size() - 4) == ".wob")
        base = base.substr(0, base.size() - 4);
    if (!wowee::pipeline::WoweeBuildingLoader::exists(base)) {
        std::fprintf(stderr, "WOB not found: %s.wob\n", base.c_str());
        return 1;
    }
    auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
    auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        if (a > b) std::swap(a, b);
        return (uint64_t(a) << 32) | uint64_t(b);
    };
    struct GroupStats {
        std::string name;
        std::size_t tris = 0;
        std::size_t degenerate = 0;
        std::size_t uniquePositions = 0;
        std::size_t totalVerts = 0;
        std::size_t boundary = 0, manifold = 0, nonManifold = 0;
        bool watertight = false;
        double surfaceArea = 0.0;
    };
    std::vector<GroupStats> perGroup;
    perGroup.reserve(bld.groups.size());
    std::size_t aggBoundary = 0, aggManifold = 0, aggNonManifold = 0;
    std::size_t aggTris = 0, aggDegenerate = 0;
    double aggArea = 0.0;
    for (const auto& g : bld.groups) {
        GroupStats gs;
        gs.name = g.name;
        gs.totalVerts = g.vertices.size();
        if (g.indices.size() % 3 != 0) {
            std::fprintf(stderr,
                "info-wob-stats: group '%s' has indices %% 3 != 0\n",
                g.name.c_str());
            return 1;
        }
        gs.tris = g.indices.size() / 3;
        // Build canon[] for this group, optionally welding via the
        // shared cli_weld utility.
        std::vector<uint32_t> canon;
        if (useWeld) {
            std::vector<glm::vec3> positions;
            positions.reserve(g.vertices.size());
            for (const auto& v : g.vertices) positions.push_back(v.position);
            canon = buildWeldMap(positions, weldEps, gs.uniquePositions);
        } else {
            canon.resize(g.vertices.size());
            for (std::size_t v = 0; v < g.vertices.size(); ++v) {
                canon[v] = static_cast<uint32_t>(v);
            }
            gs.uniquePositions = g.vertices.size();
        }
        std::unordered_map<uint64_t, uint32_t> edgeUses;
        edgeUses.reserve(gs.tris * 3);
        for (std::size_t t = 0; t < gs.tris; ++t) {
            uint32_t i0 = g.indices[t * 3 + 0];
            uint32_t i1 = g.indices[t * 3 + 1];
            uint32_t i2 = g.indices[t * 3 + 2];
            if (i0 >= g.vertices.size() ||
                i1 >= g.vertices.size() ||
                i2 >= g.vertices.size()) {
                std::fprintf(stderr,
                    "info-wob-stats: group '%s' has out-of-range index\n",
                    g.name.c_str());
                return 1;
            }
            glm::vec3 a = g.vertices[i0].position;
            glm::vec3 b = g.vertices[i1].position;
            glm::vec3 c = g.vertices[i2].position;
            double area = 0.5 * glm::length(glm::cross(b - a, c - a));
            if (area < 1e-12) ++gs.degenerate;
            gs.surfaceArea += area;
            uint32_t c0 = canon[i0], c1 = canon[i1], c2 = canon[i2];
            if (c0 != c1) ++edgeUses[edgeKey(c0, c1)];
            if (c1 != c2) ++edgeUses[edgeKey(c1, c2)];
            if (c2 != c0) ++edgeUses[edgeKey(c2, c0)];
        }
        for (const auto& [_k, count] : edgeUses) {
            if (count == 1) ++gs.boundary;
            else if (count == 2) ++gs.manifold;
            else ++gs.nonManifold;
        }
        gs.watertight = (gs.boundary == 0 && gs.nonManifold == 0);
        aggBoundary += gs.boundary;
        aggManifold += gs.manifold;
        aggNonManifold += gs.nonManifold;
        aggTris += gs.tris;
        aggDegenerate += gs.degenerate;
        aggArea += gs.surfaceArea;
        perGroup.push_back(std::move(gs));
    }
    if (jsonOut) {
        nlohmann::json j;
        j["wob"] = base + ".wob";
        j["welded"] = useWeld;
        if (useWeld) j["weldEps"] = weldEps;
        j["aggregate"] = {{"groups", perGroup.size()},
                           {"triangles", aggTris},
                           {"degenerateTriangles", aggDegenerate},
                           {"surfaceArea", aggArea},
                           {"boundary", aggBoundary},
                           {"manifold", aggManifold},
                           {"nonManifold", aggNonManifold}};
        nlohmann::json gs = nlohmann::json::array();
        for (const auto& g : perGroup) {
            gs.push_back({{"name", g.name},
                           {"triangles", g.tris},
                           {"degenerate", g.degenerate},
                           {"surfaceArea", g.surfaceArea},
                           {"uniquePositions", g.uniquePositions},
                           {"totalVerts", g.totalVerts},
                           {"boundary", g.boundary},
                           {"manifold", g.manifold},
                           {"nonManifold", g.nonManifold},
                           {"watertight", g.watertight}});
        }
        j["groups"] = gs;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WOB stats: %s.wob\n", base.c_str());
    std::printf("  groups          : %zu\n", perGroup.size());
    std::printf("  total tris      : %zu (%zu degenerate)\n",
                aggTris, aggDegenerate);
    std::printf("  total area      : %.4f\n", aggArea);
    std::printf("  aggregate edges : %zu boundary, %zu manifold, %zu non-manifold\n",
                aggBoundary, aggManifold, aggNonManifold);
    if (useWeld) {
        std::printf("  weld eps        : %.6f\n", weldEps);
    }
    std::printf("\n  Per group:\n");
    std::printf("    idx  tris   area      verts→uniq  boundary  manifold  non-m  closed\n");
    for (std::size_t k = 0; k < perGroup.size(); ++k) {
        const auto& g = perGroup[k];
        std::printf("    %3zu  %5zu  %8.3f  %5zu→%-5zu  %8zu  %8zu  %5zu  %s\n",
                    k, g.tris, g.surfaceArea,
                    g.totalVerts, g.uniquePositions,
                    g.boundary, g.manifold, g.nonManifold,
                    g.watertight ? "YES" : "no");
    }
    return 0;
}

int handleInfoWot(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = (i + 1 < argc &&
                    std::strcmp(argv[i + 1], "--json") == 0);
    if (jsonOut) i++;
    // Accept "/path/file.wot", "/path/file.whm", or "/path/file"; the
    // loader pairs both extensions from the same base path.
    for (const char* ext : {".wot", ".whm"}) {
        if (base.size() >= 4 && base.substr(base.size() - 4) == ext) {
            base = base.substr(0, base.size() - 4);
            break;
        }
    }
    if (!wowee::pipeline::WoweeTerrainLoader::exists(base)) {
        std::fprintf(stderr, "WOT/WHM not found at base: %s\n", base.c_str());
        return 1;
    }
    wowee::pipeline::ADTTerrain terrain;
    if (!wowee::pipeline::WoweeTerrainLoader::load(base, terrain)) {
        std::fprintf(stderr, "Failed to load WOT/WHM: %s\n", base.c_str());
        return 1;
    }
    int chunksWithHeights = 0, chunksWithLayers = 0, chunksWithWater = 0;
    float minH = 1e30f, maxH = -1e30f;
    for (int ci = 0; ci < 256; ci++) {
        const auto& c = terrain.chunks[ci];
        if (c.hasHeightMap()) {
            chunksWithHeights++;
            for (float h : c.heightMap.heights) {
                float total = c.position[2] + h;
                if (total < minH) minH = total;
                if (total > maxH) maxH = total;
            }
        }
        if (!c.layers.empty()) chunksWithLayers++;
        if (terrain.waterData[ci].hasWater()) chunksWithWater++;
    }
    if (jsonOut) {
        nlohmann::json j;
        j["base"] = base;
        j["tileX"] = terrain.coord.x;
        j["tileY"] = terrain.coord.y;
        j["chunks"] = {{"withHeightmap", chunksWithHeights},
                        {"withLayers", chunksWithLayers},
                        {"withWater", chunksWithWater}};
        j["textures"] = terrain.textures.size();
        j["doodads"] = terrain.doodadPlacements.size();
        j["wmos"] = terrain.wmoPlacements.size();
        if (chunksWithHeights > 0) {
            j["heightMin"] = minH;
            j["heightMax"] = maxH;
        }
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WOT/WHM: %s\n", base.c_str());
    std::printf("  tile         : (%d, %d)\n", terrain.coord.x, terrain.coord.y);
    std::printf("  chunks       : %d/256 with heightmap\n", chunksWithHeights);
    std::printf("  layers       : %d/256 chunks with texture layers\n", chunksWithLayers);
    std::printf("  water        : %d/256 chunks with water\n", chunksWithWater);
    std::printf("  textures     : %zu\n", terrain.textures.size());
    std::printf("  doodads      : %zu\n", terrain.doodadPlacements.size());
    std::printf("  WMOs         : %zu\n", terrain.wmoPlacements.size());
    if (chunksWithHeights > 0) {
        std::printf("  height range : [%.2f, %.2f]\n", minH, maxH);
    }
    return 0;
}

int handleInfoWoc(int& i, int argc, char** argv) {
    std::string path = argv[++i];
    bool jsonOut = (i + 1 < argc &&
                    std::strcmp(argv[i + 1], "--json") == 0);
    if (jsonOut) i++;
    if (path.size() < 4 || path.substr(path.size() - 4) != ".woc")
        path += ".woc";
    auto col = wowee::pipeline::WoweeCollisionBuilder::load(path);
    if (!col.isValid()) {
        std::fprintf(stderr, "WOC not found or invalid: %s\n", path.c_str());
        return 1;
    }
    if (jsonOut) {
        nlohmann::json j;
        j["woc"] = path;
        j["tileX"] = col.tileX;
        j["tileY"] = col.tileY;
        j["triangles"] = col.triangles.size();
        j["walkable"] = col.walkableCount();
        j["steep"] = col.steepCount();
        j["boundsMin"] = {col.bounds.min.x, col.bounds.min.y, col.bounds.min.z};
        j["boundsMax"] = {col.bounds.max.x, col.bounds.max.y, col.bounds.max.z};
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WOC: %s\n", path.c_str());
    std::printf("  tile        : (%u, %u)\n", col.tileX, col.tileY);
    std::printf("  triangles   : %zu\n", col.triangles.size());
    std::printf("  walkable    : %zu\n", col.walkableCount());
    std::printf("  steep       : %zu\n", col.steepCount());
    std::printf("  bounds.min  : (%.1f, %.1f, %.1f)\n",
                col.bounds.min.x, col.bounds.min.y, col.bounds.min.z);
    std::printf("  bounds.max  : (%.1f, %.1f, %.1f)\n",
                col.bounds.max.x, col.bounds.max.y, col.bounds.max.z);
    return 0;
}

}  // namespace

bool handleWorldInfo(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--info-wob") == 0 && i + 1 < argc) {
        outRc = handleInfoWob(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wob-stats") == 0 && i + 1 < argc) {
        outRc = handleInfoWobStats(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wot") == 0 && i + 1 < argc) {
        outRc = handleInfoWot(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-woc") == 0 && i + 1 < argc) {
        outRc = handleInfoWoc(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
