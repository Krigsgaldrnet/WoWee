#include "cli_world_info.hpp"

#include "pipeline/wowee_building.hpp"
#include "pipeline/wowee_collision.hpp"
#include "pipeline/wowee_terrain_loader.hpp"
#include "pipeline/adt_loader.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

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
