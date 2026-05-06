#include "editor_app.hpp"
#include "content_pack.hpp"
#include "pipeline/wowee_model.hpp"
#include "pipeline/wowee_building.hpp"
#include "pipeline/wowee_collision.hpp"
#include "pipeline/wowee_terrain_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/custom_zone_discovery.hpp"
#include "core/logger.hpp"
#include <string>
#include <cstdio>
#include <cstring>
#include <unordered_map>

static void printUsage(const char* argv0) {
    std::printf("Usage: %s --data <path> [options]\n\n", argv0);
    std::printf("Options:\n");
    std::printf("  --data <path>          Path to extracted WoW data (manifest.json)\n");
    std::printf("  --adt <map> <x> <y>    Load an ADT tile on startup\n");
    std::printf("  --convert-m2 <path>    Convert M2 model to WOM open format (no GUI)\n");
    std::printf("  --convert-wmo <path>   Convert WMO building to WOB open format (no GUI)\n");
    std::printf("  --list-zones           List discovered custom zones and exit\n");
    std::printf("  --validate <zoneDir>   Score zone open-format completeness and exit\n");
    std::printf("  --info <wom-base>      Print WOM file metadata (version, counts) and exit\n");
    std::printf("  --info-wob <wob-base>  Print WOB building metadata (groups, portals, doodads) and exit\n");
    std::printf("  --info-woc <woc-path>  Print WOC collision metadata (triangle counts, bounds) and exit\n");
    std::printf("  --info-wot <wot-base>  Print WOT/WHM terrain metadata (tile, chunks, height range) and exit\n");
    std::printf("  --info-wcp <wcp-path>  Print WCP archive metadata (name, files) and exit\n");
    std::printf("  --version              Show version and format info\n\n");
    std::printf("Wowee World Editor v1.0.0 — by Kelsi Davis\n");
    std::printf("Novel open formats: WOT/WHM/WOM/WOB/WOC/WCP + PNG/JSON\n");
}

int main(int argc, char* argv[]) {
    std::string dataPath;
    std::string adtMap;
    int adtX = -1, adtY = -1;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            dataPath = argv[++i];
        } else if (std::strcmp(argv[i], "--adt") == 0 && i + 3 < argc) {
            adtMap = argv[++i];
            adtX = std::atoi(argv[++i]);
            adtY = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--info") == 0 && i + 1 < argc) {
            std::string base = argv[++i];
            // Allow either "/path/to/file.wom" or "/path/to/file"; load() expects no extension.
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr, "WOM not found: %s.wom\n", base.c_str());
                return 1;
            }
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            std::printf("WOM: %s.wom\n", base.c_str());
            std::printf("  version    : %u%s\n", wom.version,
                        wom.version == 3 ? " (multi-batch)" :
                        wom.version == 2 ? " (animated)" : " (static)");
            std::printf("  name       : %s\n", wom.name.c_str());
            std::printf("  vertices   : %zu\n", wom.vertices.size());
            std::printf("  indices    : %zu (%zu tris)\n", wom.indices.size(), wom.indices.size() / 3);
            std::printf("  textures   : %zu\n", wom.texturePaths.size());
            std::printf("  bones      : %zu\n", wom.bones.size());
            std::printf("  animations : %zu\n", wom.animations.size());
            std::printf("  batches    : %zu\n", wom.batches.size());
            std::printf("  boundRadius: %.2f\n", wom.boundRadius);
            return 0;
        } else if (std::strcmp(argv[i], "--info-wob") == 0 && i + 1 < argc) {
            std::string base = argv[++i];
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wob")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeBuildingLoader::exists(base)) {
                std::fprintf(stderr, "WOB not found: %s.wob\n", base.c_str());
                return 1;
            }
            auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
            std::printf("WOB: %s.wob\n", base.c_str());
            std::printf("  name        : %s\n", bld.name.c_str());
            std::printf("  groups      : %zu\n", bld.groups.size());
            std::printf("  portals     : %zu\n", bld.portals.size());
            std::printf("  doodads     : %zu\n", bld.doodads.size());
            std::printf("  boundRadius : %.2f\n", bld.boundRadius);
            size_t totalVerts = 0, totalIdx = 0, totalMats = 0;
            for (const auto& g : bld.groups) {
                totalVerts += g.vertices.size();
                totalIdx += g.indices.size();
                totalMats += g.materials.size();
            }
            std::printf("  total verts : %zu\n", totalVerts);
            std::printf("  total tris  : %zu\n", totalIdx / 3);
            std::printf("  total mats  : %zu (across all groups)\n", totalMats);
            return 0;
        } else if (std::strcmp(argv[i], "--info-wcp") == 0 && i + 1 < argc) {
            std::string path = argv[++i];
            wowee::editor::ContentPackInfo info;
            if (!wowee::editor::ContentPacker::readInfo(path, info)) {
                std::fprintf(stderr, "Failed to read WCP: %s\n", path.c_str());
                return 1;
            }
            std::printf("WCP: %s\n", path.c_str());
            std::printf("  name        : %s\n", info.name.c_str());
            std::printf("  author      : %s\n", info.author.c_str());
            std::printf("  description : %s\n", info.description.c_str());
            std::printf("  version     : %s\n", info.version.c_str());
            std::printf("  format      : %s\n", info.format.c_str());
            std::printf("  mapId       : %u\n", info.mapId);
            std::printf("  files       : %zu\n", info.files.size());
            // Per-category file totals
            std::unordered_map<std::string, size_t> byCat;
            uint64_t totalSize = 0;
            for (const auto& f : info.files) {
                byCat[f.category]++;
                totalSize += f.size;
            }
            for (const auto& [cat, count] : byCat) {
                std::printf("    %-10s : %zu\n", cat.c_str(), count);
            }
            std::printf("  total bytes : %.2f MB\n", totalSize / (1024.0 * 1024.0));
            return 0;
        } else if (std::strcmp(argv[i], "--info-wot") == 0 && i + 1 < argc) {
            std::string base = argv[++i];
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
        } else if (std::strcmp(argv[i], "--info-woc") == 0 && i + 1 < argc) {
            std::string path = argv[++i];
            if (path.size() < 4 || path.substr(path.size() - 4) != ".woc")
                path += ".woc";
            auto col = wowee::pipeline::WoweeCollisionBuilder::load(path);
            if (!col.isValid()) {
                std::fprintf(stderr, "WOC not found or invalid: %s\n", path.c_str());
                return 1;
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
        } else if (std::strcmp(argv[i], "--validate") == 0 && i + 1 < argc) {
            std::string zoneDir = argv[++i];
            auto v = wowee::editor::ContentPacker::validateZone(zoneDir);
            int score = v.openFormatScore();
            std::printf("Zone: %s\n", zoneDir.c_str());
            std::printf("Open format score: %d/7\n", score);
            std::printf("Formats: %s\n", v.summary().c_str());
            std::printf("Files present:\n");
            std::printf("  WOT  (terrain meta)   : %s (%d)\n",
                        v.hasWot ? "yes" : "no", v.wotCount);
            std::printf("  WHM  (heightmap)      : %s (%d)%s\n",
                        v.hasWhm ? "yes" : "no", v.whmCount,
                        v.hasWhm && !v.whmValid ? " (BAD MAGIC)" : "");
            std::printf("  WOM  (models)         : %s (%d)%s\n",
                        v.hasWom ? "yes" : "no", v.womCount,
                        v.womInvalidCount > 0 ?
                            (" (" + std::to_string(v.womInvalidCount) + " invalid)").c_str() : "");
            std::printf("  WOB  (buildings)      : %s (%d)%s\n",
                        v.hasWob ? "yes" : "no", v.wobCount,
                        v.wobInvalidCount > 0 ?
                            (" (" + std::to_string(v.wobInvalidCount) + " invalid)").c_str() : "");
            std::printf("  WOC  (collision)      : %s (%d)%s\n",
                        v.hasWoc ? "yes" : "no", v.wocCount,
                        v.wocInvalidCount > 0 ?
                            (" (" + std::to_string(v.wocInvalidCount) + " invalid)").c_str() : "");
            std::printf("  PNG  (textures)       : %s (%d)\n",
                        v.hasPng ? "yes" : "no", v.pngCount);
            std::printf("  zone.json             : %s\n", v.hasZoneJson ? "yes" : "no");
            std::printf("  creatures.json        : %s\n", v.hasCreatures ? "yes" : "no");
            std::printf("  quests.json           : %s\n", v.hasQuests ? "yes" : "no");
            std::printf("  objects.json          : %s\n", v.hasObjects ? "yes" : "no");
            return score == 7 ? 0 : 1;
        } else if (std::strcmp(argv[i], "--list-zones") == 0) {
            auto zones = wowee::pipeline::CustomZoneDiscovery::scan({"custom_zones", "output"});
            if (zones.empty()) {
                std::printf("No custom zones found in custom_zones/ or output/\n");
            } else {
                std::printf("Custom zones found:\n");
                for (const auto& z : zones) {
                    std::printf("  %s — %s%s%s\n", z.name.c_str(), z.directory.c_str(),
                             z.hasCreatures ? " [NPCs]" : "",
                             z.hasQuests ? " [Quests]" : "");
                }
            }
            return 0;
        } else if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("Wowee World Editor v1.0.0\n");
            std::printf("Open formats: WOT/WHM/WOM/WOB/WOC/WCP + PNG/JSON (all novel)\n");
            std::printf("By Kelsi Davis\n");
            return 0;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    // Batch convert mode: --convert <m2path> converts M2 to WOM
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--convert-m2") == 0 && i + 1 < argc) {
            std::string m2Path = argv[++i];
            std::printf("Converting M2→WOM: %s\n", m2Path.c_str());
            if (dataPath.empty()) dataPath = "Data";
            wowee::pipeline::AssetManager am;
            if (am.initialize(dataPath)) {
                auto wom = wowee::pipeline::WoweeModelLoader::fromM2(m2Path, &am);
                if (wom.isValid()) {
                    std::string outPath = m2Path;
                    auto dot = outPath.rfind('.');
                    if (dot != std::string::npos) outPath = outPath.substr(0, dot);
                    wowee::pipeline::WoweeModelLoader::save(wom, "output/models/" + outPath);
                    std::printf("OK: output/models/%s.wom (v%u, %zu verts, %zu bones, %zu batches)\n",
                        outPath.c_str(), wom.version, wom.vertices.size(),
                        wom.bones.size(), wom.batches.size());
                } else {
                    std::fprintf(stderr, "FAILED: %s\n", m2Path.c_str());
                    am.shutdown();
                    return 1;
                }
                am.shutdown();
            } else {
                std::fprintf(stderr, "FAILED: cannot initialize asset manager\n");
                return 1;
            }
            return 0;
        }
    }

    // Batch convert mode: --convert-wmo converts WMO to WOB
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--convert-wmo") == 0 && i + 1 < argc) {
            std::string wmoPath = argv[++i];
            std::printf("Converting WMO→WOB: %s\n", wmoPath.c_str());
            if (dataPath.empty()) dataPath = "Data";
            wowee::pipeline::AssetManager am;
            if (am.initialize(dataPath)) {
                auto wmoData = am.readFile(wmoPath);
                if (!wmoData.empty()) {
                    auto wmoModel = wowee::pipeline::WMOLoader::load(wmoData);
                    if (wmoModel.nGroups > 0) {
                        std::string wmoBase = wmoPath;
                        if (wmoBase.size() > 4) wmoBase = wmoBase.substr(0, wmoBase.size() - 4);
                        for (uint32_t gi = 0; gi < wmoModel.nGroups; gi++) {
                            char suffix[16];
                            snprintf(suffix, sizeof(suffix), "_%03u.wmo", gi);
                            auto gd = am.readFile(wmoBase + suffix);
                            if (!gd.empty()) wowee::pipeline::WMOLoader::loadGroup(gd, wmoModel, gi);
                        }
                    }
                    auto wob = wowee::pipeline::WoweeBuildingLoader::fromWMO(wmoModel, wmoPath);
                    if (wob.isValid()) {
                        std::string outPath = wmoPath;
                        auto dot = outPath.rfind('.');
                        if (dot != std::string::npos) outPath = outPath.substr(0, dot);
                        wowee::pipeline::WoweeBuildingLoader::save(wob, "output/buildings/" + outPath);
                        std::printf("OK: output/buildings/%s.wob (%zu groups)\n",
                            outPath.c_str(), wob.groups.size());
                    } else {
                        std::fprintf(stderr, "FAILED: %s\n", wmoPath.c_str());
                        am.shutdown();
                        return 1;
                    }
                } else {
                    std::fprintf(stderr, "FAILED: file not found: %s\n", wmoPath.c_str());
                    am.shutdown();
                    return 1;
                }
                am.shutdown();
            } else {
                std::fprintf(stderr, "FAILED: cannot initialize asset manager\n");
                return 1;
            }
            return 0;
        }
    }

    if (dataPath.empty()) {
        dataPath = "Data";
        LOG_INFO("No --data path specified, using default: ", dataPath);
    }

    wowee::editor::EditorApp app;
    if (!app.initialize(dataPath)) {
        LOG_ERROR("Failed to initialize editor");
        return 1;
    }

    if (!adtMap.empty()) {
        app.loadADT(adtMap, adtX, adtY);
    }

    app.run();
    app.shutdown();

    return 0;
}
