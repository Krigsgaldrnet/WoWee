#include "editor_app.hpp"
#include "content_pack.hpp"
#include "npc_spawner.hpp"
#include "object_placer.hpp"
#include "quest_editor.hpp"
#include "wowee_terrain.hpp"
#include "zone_manifest.hpp"
#include "terrain_editor.hpp"
#include "terrain_biomes.hpp"
#include <filesystem>
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
#include <algorithm>

static void printUsage(const char* argv0) {
    std::printf("Usage: %s --data <path> [options]\n\n", argv0);
    std::printf("Options:\n");
    std::printf("  --data <path>          Path to extracted WoW data (manifest.json)\n");
    std::printf("  --adt <map> <x> <y>    Load an ADT tile on startup\n");
    std::printf("  --convert-m2 <path>    Convert M2 model to WOM open format (no GUI)\n");
    std::printf("  --convert-wmo <path>   Convert WMO building to WOB open format (no GUI)\n");
    std::printf("  --list-zones           List discovered custom zones and exit\n");
    std::printf("  --scaffold-zone <name> [tx ty]  Create a blank zone in custom_zones/<name>/ and exit\n");
    std::printf("  --build-woc <wot-base> Generate a WOC collision mesh from WHM/WOT and exit\n");
    std::printf("  --validate <zoneDir>   Score zone open-format completeness and exit\n");
    std::printf("  --info <wom-base>      Print WOM file metadata (version, counts) and exit\n");
    std::printf("  --info-wob <wob-base>  Print WOB building metadata (groups, portals, doodads) and exit\n");
    std::printf("  --info-woc <woc-path>  Print WOC collision metadata (triangle counts, bounds) and exit\n");
    std::printf("  --info-wot <wot-base>  Print WOT/WHM terrain metadata (tile, chunks, height range) and exit\n");
    std::printf("  --info-creatures <p>   Print creatures.json summary (counts, behaviors) and exit\n");
    std::printf("  --info-objects <p>     Print objects.json summary (counts, types, scale range) and exit\n");
    std::printf("  --info-quests <p>      Print quests.json summary (counts, rewards, chain errors) and exit\n");
    std::printf("  --info-wcp <wcp-path>  Print WCP archive metadata (name, files) and exit\n");
    std::printf("  --list-wcp <wcp-path>  Print every file inside a WCP archive (sorted by path) and exit\n");
    std::printf("  --pack-wcp <zone> [dst]   Pack a zone dir/name into a .wcp archive and exit\n");
    std::printf("  --unpack-wcp <wcp> [dst]  Extract a WCP archive (default dst=custom_zones/) and exit\n");
    std::printf("  --version              Show version and format info\n\n");
    std::printf("Wowee World Editor v1.0.0 — by Kelsi Davis\n");
    std::printf("Novel open formats: WOT/WHM/WOM/WOB/WOC/WCP + PNG/JSON\n");
}

int main(int argc, char* argv[]) {
    std::string dataPath;
    std::string adtMap;
    int adtX = -1, adtY = -1;

    // Detect non-GUI options that are missing their argument and bail out
    // with a helpful message instead of silently dropping into the GUI.
    static const char* kArgRequired[] = {
        "--data", "--info", "--info-wob", "--info-woc", "--info-wot",
        "--info-creatures", "--info-objects", "--info-quests",
        "--info-wcp", "--list-wcp", "--unpack-wcp", "--pack-wcp",
        "--validate", "--scaffold-zone", "--build-woc",
        "--convert-m2", "--convert-wmo",
    };
    for (int i = 1; i < argc; i++) {
        for (const char* opt : kArgRequired) {
            if (std::strcmp(argv[i], opt) == 0 && i + 1 >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", opt);
                return 1;
            }
        }
        if (std::strcmp(argv[i], "--adt") == 0 && i + 3 >= argc) {
            std::fprintf(stderr, "--adt requires <map> <x> <y>\n");
            return 1;
        }
    }

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
        } else if (std::strcmp(argv[i], "--info-quests") == 0 && i + 1 < argc) {
            std::string path = argv[++i];
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "Failed to load quests.json: %s\n", path.c_str());
                return 1;
            }
            const auto& quests = qe.getQuests();
            int chained = 0, withReward = 0, withItems = 0;
            int objKill = 0, objCollect = 0, objTalk = 0;
            uint32_t totalXp = 0;
            for (const auto& q : quests) {
                if (q.nextQuestId != 0) chained++;
                if (q.reward.xp > 0 || q.reward.gold > 0 ||
                    q.reward.silver > 0 || q.reward.copper > 0) withReward++;
                if (!q.reward.itemRewards.empty()) withItems++;
                totalXp += q.reward.xp;
                using OT = wowee::editor::QuestObjectiveType;
                for (const auto& obj : q.objectives) {
                    if (obj.type == OT::KillCreature) objKill++;
                    else if (obj.type == OT::CollectItem) objCollect++;
                    else if (obj.type == OT::TalkToNPC) objTalk++;
                }
            }
            std::vector<std::string> errors;
            qe.validateChains(errors);
            std::printf("quests.json: %s\n", path.c_str());
            std::printf("  total       : %zu\n", quests.size());
            std::printf("  chained     : %d (have nextQuestId)\n", chained);
            std::printf("  with reward : %d\n", withReward);
            std::printf("  with items  : %d\n", withItems);
            std::printf("  total XP    : %u (avg %.0f per quest)\n", totalXp,
                        quests.empty() ? 0.0 : double(totalXp) / quests.size());
            std::printf("  objectives  : %d kill, %d collect, %d talk\n",
                        objKill, objCollect, objTalk);
            if (!errors.empty()) {
                std::printf("  chain errors: %zu\n", errors.size());
                for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-objects") == 0 && i + 1 < argc) {
            std::string path = argv[++i];
            wowee::editor::ObjectPlacer placer;
            if (!placer.loadFromFile(path)) {
                std::fprintf(stderr, "Failed to load objects.json: %s\n", path.c_str());
                return 1;
            }
            const auto& objs = placer.getObjects();
            int m2Count = 0, wmoCount = 0;
            std::unordered_map<std::string, int> pathHist;
            float minScale = 1e30f, maxScale = -1e30f;
            for (const auto& o : objs) {
                if (o.type == wowee::editor::PlaceableType::M2) m2Count++;
                else if (o.type == wowee::editor::PlaceableType::WMO) wmoCount++;
                pathHist[o.path]++;
                if (o.scale < minScale) minScale = o.scale;
                if (o.scale > maxScale) maxScale = o.scale;
            }
            std::printf("objects.json: %s\n", path.c_str());
            std::printf("  total       : %zu\n", objs.size());
            std::printf("  M2 doodads  : %d\n", m2Count);
            std::printf("  WMO buildings: %d\n", wmoCount);
            std::printf("  unique paths: %zu\n", pathHist.size());
            if (!objs.empty()) {
                std::printf("  scale range : [%.2f, %.2f]\n", minScale, maxScale);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-creatures") == 0 && i + 1 < argc) {
            std::string path = argv[++i];
            wowee::editor::NpcSpawner spawner;
            if (!spawner.loadFromFile(path)) {
                std::fprintf(stderr, "Failed to load creatures.json: %s\n", path.c_str());
                return 1;
            }
            const auto& spawns = spawner.getSpawns();
            int hostile = 0, vendor = 0, questgiver = 0, trainer = 0;
            int patrol = 0, wander = 0, stationary = 0;
            std::unordered_map<uint32_t, int> displayIdHist;
            for (const auto& s : spawns) {
                if (s.hostile) hostile++;
                if (s.vendor) vendor++;
                if (s.questgiver) questgiver++;
                if (s.trainer) trainer++;
                using B = wowee::editor::CreatureBehavior;
                if (s.behavior == B::Patrol) patrol++;
                else if (s.behavior == B::Wander) wander++;
                else if (s.behavior == B::Stationary) stationary++;
                displayIdHist[s.displayId]++;
            }
            std::printf("creatures.json: %s\n", path.c_str());
            std::printf("  total       : %zu\n", spawns.size());
            std::printf("  hostile     : %d\n", hostile);
            std::printf("  questgiver  : %d\n", questgiver);
            std::printf("  vendor      : %d\n", vendor);
            std::printf("  trainer     : %d\n", trainer);
            std::printf("  behavior    : %d stationary, %d wander, %d patrol\n",
                        stationary, wander, patrol);
            std::printf("  unique displayIds: %zu\n", displayIdHist.size());
            return 0;
        } else if (std::strcmp(argv[i], "--list-wcp") == 0 && i + 1 < argc) {
            // Like --info-wcp but prints every file path. Useful for spotting
            // missing or unexpected entries before unpacking.
            std::string path = argv[++i];
            wowee::editor::ContentPackInfo info;
            if (!wowee::editor::ContentPacker::readInfo(path, info)) {
                std::fprintf(stderr, "Failed to read WCP: %s\n", path.c_str());
                return 1;
            }
            std::printf("WCP: %s — %zu files\n", path.c_str(), info.files.size());
            // Sort by path so identical packs produce identical output (the
            // packer order depends on the directory_iterator implementation).
            auto files = info.files;
            std::sort(files.begin(), files.end(),
                      [](const auto& a, const auto& b) { return a.path < b.path; });
            for (const auto& f : files) {
                std::printf("  %-10s %10llu  %s\n",
                            f.category.c_str(),
                            static_cast<unsigned long long>(f.size),
                            f.path.c_str());
            }
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
        } else if (std::strcmp(argv[i], "--build-woc") == 0 && i + 1 < argc) {
            // Generate a WOC collision mesh from a WHM/WOT terrain pair.
            // Uses terrain triangles only (no WMO overlays); useful as a
            // first-pass collision build before the editor adds buildings.
            std::string base = argv[++i];
            for (const char* ext : {".wot", ".whm", ".woc"}) {
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
                std::fprintf(stderr, "Failed to load terrain: %s\n", base.c_str());
                return 1;
            }
            auto col = wowee::pipeline::WoweeCollisionBuilder::fromTerrain(terrain);
            std::string outPath = base + ".woc";
            if (!wowee::pipeline::WoweeCollisionBuilder::save(col, outPath)) {
                std::fprintf(stderr, "WOC save failed: %s\n", outPath.c_str());
                return 1;
            }
            std::printf("WOC built: %s (%zu triangles, %zu walkable, %zu steep)\n",
                        outPath.c_str(),
                        col.triangles.size(), col.walkableCount(), col.steepCount());
            return 0;
        } else if (std::strcmp(argv[i], "--scaffold-zone") == 0 && i + 1 < argc) {
            // Generate a minimal valid empty zone — useful for kickstarting
            // a new authoring session without needing to launch the GUI.
            std::string rawName = argv[++i];
            int sx = 32, sy = 32;
            if (i + 2 < argc) {
                int parsedX = std::atoi(argv[i + 1]);
                int parsedY = std::atoi(argv[i + 2]);
                if (parsedX >= 0 && parsedX <= 63 &&
                    parsedY >= 0 && parsedY <= 63) {
                    sx = parsedX; sy = parsedY;
                    i += 2;
                }
            }
            // Slugify name to match unpackZone / server module rules.
            std::string slug;
            for (char c : rawName) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-') {
                    slug += c;
                } else if (c == ' ') {
                    slug += '_';
                }
            }
            if (slug.empty()) {
                std::fprintf(stderr, "--scaffold-zone: name '%s' has no valid characters\n",
                             rawName.c_str());
                return 1;
            }
            namespace fs = std::filesystem;
            std::string dir = "custom_zones/" + slug;
            if (fs::exists(dir)) {
                std::fprintf(stderr, "--scaffold-zone: directory already exists: %s\n",
                             dir.c_str());
                return 1;
            }
            fs::create_directories(dir);

            // Blank flat terrain at the requested tile.
            auto terrain = wowee::editor::TerrainEditor::createBlankTerrain(
                sx, sy, 100.0f, wowee::editor::Biome::Grassland);
            std::string base = dir + "/" + slug + "_" +
                               std::to_string(sx) + "_" + std::to_string(sy);
            wowee::editor::WoweeTerrain::exportOpen(terrain, base, sx, sy);

            // Minimal zone.json
            wowee::editor::ZoneManifest manifest;
            manifest.mapName = slug;
            manifest.displayName = rawName;
            manifest.mapId = 9000;
            manifest.baseHeight = 100.0f;
            manifest.tiles.push_back({sx, sy});
            manifest.save(dir + "/zone.json");

            std::printf("Scaffolded zone: %s\n", dir.c_str());
            std::printf("  tile     : (%d, %d)\n", sx, sy);
            std::printf("  files    : %s.wot, %s.whm, zone.json\n",
                        slug.c_str(), slug.c_str());
            std::printf("  next step: run editor without args, then File → Open Zone\n");
            return 0;
        } else if (std::strcmp(argv[i], "--pack-wcp") == 0 && i + 1 < argc) {
            // Pack a zone directory into a .wcp archive.
            // Usage: --pack-wcp <zoneDirOrName> [destPath]
            // If <zoneDirOrName> looks like a path (contains '/' or starts
            // with '.'), use it directly; otherwise resolve under
            // custom_zones/ then output/ (matching the discovery search
            // order).
            std::string nameOrDir = argv[++i];
            std::string destPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                destPath = argv[++i];
            }
            namespace fs = std::filesystem;
            std::string outputDir, mapName;
            if (nameOrDir.find('/') != std::string::npos || nameOrDir[0] == '.') {
                fs::path p = fs::absolute(nameOrDir);
                outputDir = p.parent_path().string();
                mapName = p.filename().string();
            } else {
                mapName = nameOrDir;
                if (fs::exists("custom_zones/" + mapName)) outputDir = "custom_zones";
                else if (fs::exists("output/" + mapName)) outputDir = "output";
                else {
                    std::fprintf(stderr,
                        "--pack-wcp: zone '%s' not found in custom_zones/ or output/\n",
                        mapName.c_str());
                    return 1;
                }
            }
            if (destPath.empty()) destPath = mapName + ".wcp";
            wowee::editor::ContentPackInfo info;
            info.name = mapName;
            info.format = "wcp-1.0";
            if (!wowee::editor::ContentPacker::packZone(outputDir, mapName, destPath, info)) {
                std::fprintf(stderr, "WCP pack failed for %s/%s\n",
                             outputDir.c_str(), mapName.c_str());
                return 1;
            }
            std::printf("WCP packed: %s\n", destPath.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--unpack-wcp") == 0 && i + 1 < argc) {
            std::string wcpPath = argv[++i];
            std::string destDir = "custom_zones";
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                destDir = argv[++i];
            }
            if (!wowee::editor::ContentPacker::unpackZone(wcpPath, destDir)) {
                std::fprintf(stderr, "WCP unpack failed: %s\n", wcpPath.c_str());
                return 1;
            }
            std::printf("WCP unpacked to: %s\n", destDir.c_str());
            return 0;
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
