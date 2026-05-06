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
#include <nlohmann/json.hpp>

// ─── Open-format consistency checks ─────────────────────────────
// Both validators are called from the per-file CLI commands AND
// from --validate-all which walks a zone dir. Returning a vector
// of error strings (empty == passed) keeps callers simple.
static std::vector<std::string> validateWomErrors(
        const wowee::pipeline::WoweeModel& wom) {
    std::vector<std::string> errors;
    if (wom.version < 1 || wom.version > 3) {
        errors.push_back("version " + std::to_string(wom.version) +
                         " outside [1,3]");
    }
    if (!wom.isValid()) errors.push_back("empty geometry (no verts/indices)");
    if (wom.indices.size() % 3 != 0) {
        errors.push_back("indices.size()=" + std::to_string(wom.indices.size()) +
                         " not divisible by 3");
    }
    int oobIdx = 0;
    for (uint32_t idx : wom.indices) {
        if (idx >= wom.vertices.size()) {
            if (++oobIdx <= 3) {
                errors.push_back("index " + std::to_string(idx) +
                                 " >= vertexCount " +
                                 std::to_string(wom.vertices.size()));
            }
        }
    }
    if (oobIdx > 3) {
        errors.push_back("... and " + std::to_string(oobIdx - 3) +
                         " more out-of-range indices");
    }
    for (size_t b = 0; b < wom.bones.size(); ++b) {
        int16_t p = wom.bones[b].parentBone;
        if (p == -1) continue;
        if (p < 0 || p >= static_cast<int16_t>(wom.bones.size())) {
            errors.push_back("bone " + std::to_string(b) +
                             " parent=" + std::to_string(p) +
                             " out of range");
        } else if (p >= static_cast<int16_t>(b)) {
            errors.push_back("bone " + std::to_string(b) +
                             " parent=" + std::to_string(p) +
                             " not strictly less (DAG order)");
        }
    }
    int oobVB = 0;
    for (size_t v = 0; v < wom.vertices.size() && !wom.bones.empty(); ++v) {
        const auto& vert = wom.vertices[v];
        for (int k = 0; k < 4; ++k) {
            if (vert.boneWeights[k] == 0) continue;
            if (vert.boneIndices[k] >= wom.bones.size()) {
                if (++oobVB <= 3) {
                    errors.push_back("vertex " + std::to_string(v) +
                                     " boneIndex[" + std::to_string(k) +
                                     "]=" + std::to_string(vert.boneIndices[k]) +
                                     " >= boneCount " +
                                     std::to_string(wom.bones.size()));
                }
            }
        }
    }
    if (oobVB > 3) {
        errors.push_back("... and " + std::to_string(oobVB - 3) +
                         " more out-of-range vertex bone refs");
    }
    for (size_t a = 0; a < wom.animations.size(); ++a) {
        const auto& anim = wom.animations[a];
        if (!anim.boneKeyframes.empty() &&
            anim.boneKeyframes.size() != wom.bones.size()) {
            errors.push_back("animation " + std::to_string(a) +
                             " boneKeyframes.size()=" +
                             std::to_string(anim.boneKeyframes.size()) +
                             " != boneCount " +
                             std::to_string(wom.bones.size()));
        }
    }
    for (size_t b = 0; b < wom.batches.size(); ++b) {
        const auto& batch = wom.batches[b];
        uint64_t end = uint64_t(batch.indexStart) + batch.indexCount;
        if (end > wom.indices.size()) {
            errors.push_back("batch " + std::to_string(b) +
                             " indexStart+Count=" + std::to_string(end) +
                             " > indexCount " +
                             std::to_string(wom.indices.size()));
        }
        if (batch.indexCount % 3 != 0) {
            errors.push_back("batch " + std::to_string(b) +
                             " indexCount=" + std::to_string(batch.indexCount) +
                             " not divisible by 3");
        }
        if (!wom.texturePaths.empty() &&
            batch.textureIndex >= wom.texturePaths.size()) {
            errors.push_back("batch " + std::to_string(b) +
                             " textureIndex=" + std::to_string(batch.textureIndex) +
                             " >= textureCount " +
                             std::to_string(wom.texturePaths.size()));
        }
    }
    if (wom.boundMin.x > wom.boundMax.x ||
        wom.boundMin.y > wom.boundMax.y ||
        wom.boundMin.z > wom.boundMax.z) {
        errors.push_back("boundMin > boundMax on at least one axis");
    }
    if (wom.boundRadius < 0.0f) {
        errors.push_back("boundRadius=" + std::to_string(wom.boundRadius) +
                         " is negative");
    }
    return errors;
}

static std::vector<std::string> validateWobErrors(
        const wowee::pipeline::WoweeBuilding& bld) {
    std::vector<std::string> errors;
    if (!bld.isValid()) errors.push_back("empty building (no groups)");
    int badMatTexCount = 0;
    for (size_t g = 0; g < bld.groups.size(); ++g) {
        const auto& grp = bld.groups[g];
        if (grp.indices.size() % 3 != 0) {
            errors.push_back("group " + std::to_string(g) +
                             " indices.size()=" + std::to_string(grp.indices.size()) +
                             " not divisible by 3");
        }
        int oobIdx = 0;
        for (uint32_t idx : grp.indices) {
            if (idx >= grp.vertices.size()) ++oobIdx;
        }
        if (oobIdx > 0) {
            errors.push_back("group " + std::to_string(g) + " has " +
                             std::to_string(oobIdx) +
                             " indices out of range (vertCount=" +
                             std::to_string(grp.vertices.size()) + ")");
        }
        for (size_t m = 0; m < grp.materials.size(); ++m) {
            if (grp.materials[m].texturePath.empty()) {
                badMatTexCount++;
                if (badMatTexCount <= 3) {
                    errors.push_back("group " + std::to_string(g) +
                                     " material " + std::to_string(m) +
                                     " has empty texturePath");
                }
            }
        }
        if (grp.boundMin.x > grp.boundMax.x ||
            grp.boundMin.y > grp.boundMax.y ||
            grp.boundMin.z > grp.boundMax.z) {
            errors.push_back("group " + std::to_string(g) +
                             " boundMin > boundMax on at least one axis");
        }
    }
    if (badMatTexCount > 3) {
        errors.push_back("... and " + std::to_string(badMatTexCount - 3) +
                         " more empty material textures");
    }
    int badPortal = 0;
    for (size_t p = 0; p < bld.portals.size(); ++p) {
        const auto& portal = bld.portals[p];
        auto inRange = [&](int g) {
            return g == -1 ||
                   (g >= 0 && g < static_cast<int>(bld.groups.size()));
        };
        if (!inRange(portal.groupA) || !inRange(portal.groupB)) {
            if (++badPortal <= 3) {
                errors.push_back("portal " + std::to_string(p) +
                                 " refs out-of-range groups (" +
                                 std::to_string(portal.groupA) + ", " +
                                 std::to_string(portal.groupB) + ")");
            }
        }
        if (portal.vertices.size() < 3) {
            if (++badPortal <= 3) {
                errors.push_back("portal " + std::to_string(p) +
                                 " has only " +
                                 std::to_string(portal.vertices.size()) +
                                 " verts (need >= 3 for a polygon)");
            }
        }
    }
    if (badPortal > 3) {
        errors.push_back("... and " + std::to_string(badPortal - 3) +
                         " more bad portal entries");
    }
    int badDoodad = 0;
    for (size_t d = 0; d < bld.doodads.size(); ++d) {
        const auto& doodad = bld.doodads[d];
        if (doodad.modelPath.empty()) {
            if (++badDoodad <= 3) {
                errors.push_back("doodad " + std::to_string(d) +
                                 " has empty modelPath");
            }
        }
        if (!std::isfinite(doodad.scale) || doodad.scale <= 0.0f) {
            if (++badDoodad <= 3) {
                errors.push_back("doodad " + std::to_string(d) +
                                 " has non-positive scale " +
                                 std::to_string(doodad.scale));
            }
        }
    }
    if (badDoodad > 3) {
        errors.push_back("... and " + std::to_string(badDoodad - 3) +
                         " more bad doodad entries");
    }
    if (bld.boundRadius < 0.0f) {
        errors.push_back("boundRadius=" + std::to_string(bld.boundRadius) +
                         " is negative");
    }
    return errors;
}

static std::vector<std::string> validateWocErrors(
        const wowee::pipeline::WoweeCollision& woc) {
    std::vector<std::string> errors;
    if (!woc.isValid()) errors.push_back("empty collision (no triangles)");
    if (woc.tileX >= 64 || woc.tileY >= 64) {
        errors.push_back("tile coords out of WoW grid: (" +
                         std::to_string(woc.tileX) + ", " +
                         std::to_string(woc.tileY) + ") — must be < 64");
    }
    int nanTris = 0, degenerate = 0, badFlags = 0;
    auto isFiniteVec = [](const glm::vec3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    constexpr uint8_t kKnownFlags = 0x0F;  // walkable|water|steep|indoor
    for (size_t t = 0; t < woc.triangles.size(); ++t) {
        const auto& tri = woc.triangles[t];
        if (!isFiniteVec(tri.v0) || !isFiniteVec(tri.v1) || !isFiniteVec(tri.v2)) {
            if (++nanTris <= 3) {
                errors.push_back("triangle " + std::to_string(t) +
                                 " has non-finite vertex coord");
            }
        }
        if (tri.v0 == tri.v1 || tri.v1 == tri.v2 || tri.v0 == tri.v2) {
            if (++degenerate <= 3) {
                errors.push_back("triangle " + std::to_string(t) +
                                 " is degenerate (two vertices identical)");
            }
        }
        if (tri.flags & ~kKnownFlags) {
            if (++badFlags <= 3) {
                errors.push_back("triangle " + std::to_string(t) +
                                 " has unknown flag bits 0x" +
                                 [&]{ char b[8]; std::snprintf(b,sizeof b,"%02X",tri.flags); return std::string(b); }());
            }
        }
    }
    if (nanTris > 3) errors.push_back("... and " + std::to_string(nanTris - 3) +
                                       " more non-finite triangles");
    if (degenerate > 3) errors.push_back("... and " + std::to_string(degenerate - 3) +
                                          " more degenerate triangles");
    if (badFlags > 3) errors.push_back("... and " + std::to_string(badFlags - 3) +
                                        " more triangles with unknown flag bits");
    if (woc.bounds.min.x > woc.bounds.max.x ||
        woc.bounds.min.y > woc.bounds.max.y ||
        woc.bounds.min.z > woc.bounds.max.z) {
        errors.push_back("bounds.min > bounds.max on at least one axis");
    }
    return errors;
}

static std::vector<std::string> validateWhmErrors(
        const wowee::pipeline::ADTTerrain& terrain) {
    std::vector<std::string> errors;
    if (!terrain.isLoaded()) {
        errors.push_back("terrain not loaded");
        return errors;
    }
    if (terrain.coord.x < 0 || terrain.coord.x >= 64 ||
        terrain.coord.y < 0 || terrain.coord.y >= 64) {
        errors.push_back("tile coord out of WoW grid: (" +
                         std::to_string(terrain.coord.x) + ", " +
                         std::to_string(terrain.coord.y) + ")");
    }
    int nanHeightChunks = 0, nanPosChunks = 0;
    int loadedChunks = 0;
    float minH = 1e30f, maxH = -1e30f;
    for (size_t c = 0; c < 256; ++c) {
        const auto& chunk = terrain.chunks[c];
        if (!chunk.heightMap.isLoaded()) continue;
        loadedChunks++;
        if (!std::isfinite(chunk.position[0]) ||
            !std::isfinite(chunk.position[1]) ||
            !std::isfinite(chunk.position[2])) {
            if (++nanPosChunks <= 3) {
                errors.push_back("chunk " + std::to_string(c) +
                                 " has non-finite position");
            }
        }
        bool chunkHasBadHeight = false;
        for (float h : chunk.heightMap.heights) {
            if (!std::isfinite(h)) {
                chunkHasBadHeight = true;
            } else {
                if (h < minH) minH = h;
                if (h > maxH) maxH = h;
            }
        }
        if (chunkHasBadHeight) {
            if (++nanHeightChunks <= 3) {
                errors.push_back("chunk " + std::to_string(c) +
                                 " contains non-finite heights");
            }
        }
    }
    if (nanHeightChunks > 3) {
        errors.push_back("... and " + std::to_string(nanHeightChunks - 3) +
                         " more chunks with non-finite heights");
    }
    if (nanPosChunks > 3) {
        errors.push_back("... and " + std::to_string(nanPosChunks - 3) +
                         " more chunks with non-finite positions");
    }
    if (loadedChunks == 0) {
        errors.push_back("no chunks loaded (heightmap empty)");
    }
    // Heights outside the WoW world envelope often signal a units-confusion
    // bug — most maps stay in [-3000, 3000]. Warn-class, not fail.
    if (loadedChunks > 0 && (minH < -10000.0f || maxH > 10000.0f)) {
        errors.push_back("height range [" + std::to_string(minH) +
                         ", " + std::to_string(maxH) +
                         "] is outside reasonable WoW envelope");
    }
    int badPlacements = 0;
    for (size_t p = 0; p < terrain.doodadPlacements.size(); ++p) {
        const auto& d = terrain.doodadPlacements[p];
        if (!std::isfinite(d.position[0]) ||
            !std::isfinite(d.position[1]) ||
            !std::isfinite(d.position[2])) {
            if (++badPlacements <= 3) {
                errors.push_back("doodad placement " + std::to_string(p) +
                                 " has non-finite position");
            }
        }
        if (d.scale == 0) {
            if (++badPlacements <= 3) {
                errors.push_back("doodad placement " + std::to_string(p) +
                                 " has scale=0");
            }
        }
        if (!terrain.doodadNames.empty() && d.nameId >= terrain.doodadNames.size()) {
            if (++badPlacements <= 3) {
                errors.push_back("doodad placement " + std::to_string(p) +
                                 " nameId=" + std::to_string(d.nameId) +
                                 " >= doodadNames " +
                                 std::to_string(terrain.doodadNames.size()));
            }
        }
    }
    for (size_t p = 0; p < terrain.wmoPlacements.size(); ++p) {
        const auto& w = terrain.wmoPlacements[p];
        if (!std::isfinite(w.position[0]) ||
            !std::isfinite(w.position[1]) ||
            !std::isfinite(w.position[2])) {
            if (++badPlacements <= 3) {
                errors.push_back("wmo placement " + std::to_string(p) +
                                 " has non-finite position");
            }
        }
        if (!terrain.wmoNames.empty() && w.nameId >= terrain.wmoNames.size()) {
            if (++badPlacements <= 3) {
                errors.push_back("wmo placement " + std::to_string(p) +
                                 " nameId=" + std::to_string(w.nameId) +
                                 " >= wmoNames " +
                                 std::to_string(terrain.wmoNames.size()));
            }
        }
    }
    if (badPlacements > 3) {
        errors.push_back("... and " + std::to_string(badPlacements - 3) +
                         " more bad placement entries");
    }
    return errors;
}

static void printUsage(const char* argv0) {
    std::printf("Usage: %s --data <path> [options]\n\n", argv0);
    std::printf("Options:\n");
    std::printf("  --data <path>          Path to extracted WoW data (manifest.json)\n");
    std::printf("  --adt <map> <x> <y>    Load an ADT tile on startup\n");
    std::printf("  --convert-m2 <path>    Convert M2 model to WOM open format (no GUI)\n");
    std::printf("  --convert-wmo <path>   Convert WMO building to WOB open format (no GUI)\n");
    std::printf("  --list-zones [--json]  List discovered custom zones and exit\n");
    std::printf("  --scaffold-zone <name> [tx ty]  Create a blank zone in custom_zones/<name>/ and exit\n");
    std::printf("  --add-creature <zoneDir> <name> <x> <y> <z> [displayId] [level]\n");
    std::printf("                         Append one creature spawn to <zoneDir>/creatures.json and exit\n");
    std::printf("  --add-object <zoneDir> <m2|wmo> <gamePath> <x> <y> <z> [scale]\n");
    std::printf("                         Append one object placement to <zoneDir>/objects.json and exit\n");
    std::printf("  --add-quest <zoneDir> <title> [giverId] [turnInId] [xp] [level]\n");
    std::printf("                         Append one quest to <zoneDir>/quests.json and exit\n");
    std::printf("  --copy-zone <srcDir> <newName>\n");
    std::printf("                         Duplicate a zone to custom_zones/<slug>/ with renamed slug-prefixed files\n");
    std::printf("  --build-woc <wot-base> Generate a WOC collision mesh from WHM/WOT and exit\n");
    std::printf("  --regen-collision <zoneDir>  Rebuild every WOC under a zone dir and exit\n");
    std::printf("  --fix-zone <zoneDir>   Re-parse + re-save zone JSONs to apply latest scrubs/caps and exit\n");
    std::printf("  --export-png <wot-base> Render heightmap, normal-map, and zone-map PNG previews\n");
    std::printf("  --validate <zoneDir> [--json]\n");
    std::printf("                         Score zone open-format completeness and exit\n");
    std::printf("  --validate-wom <wom-base> [--json]\n");
    std::printf("                         Deep-check a WOM file for index/bone/batch/bound invariants\n");
    std::printf("  --validate-wob <wob-base> [--json]\n");
    std::printf("                         Deep-check a WOB file for group/portal/doodad invariants\n");
    std::printf("  --validate-woc <woc-path> [--json]\n");
    std::printf("                         Deep-check a WOC collision mesh for finite verts and degeneracy\n");
    std::printf("  --validate-whm <wot-base> [--json]\n");
    std::printf("                         Deep-check a WHM/WOT terrain pair for NaN heights and bad placements\n");
    std::printf("  --validate-all <dir> [--json]\n");
    std::printf("                         Recursively run all per-format validators on every file\n");
    std::printf("  --zone-summary <zoneDir> [--json]\n");
    std::printf("                         One-shot validate + creature/object/quest counts and exit\n");
    std::printf("  --info <wom-base> [--json]\n");
    std::printf("                         Print WOM file metadata (version, counts) and exit\n");
    std::printf("  --info-wob <wob-base> [--json]\n");
    std::printf("                         Print WOB building metadata (groups, portals, doodads) and exit\n");
    std::printf("  --info-woc <woc-path> [--json]\n");
    std::printf("                         Print WOC collision metadata (triangle counts, bounds) and exit\n");
    std::printf("  --info-wot <wot-base> [--json]\n");
    std::printf("                         Print WOT/WHM terrain metadata (tile, chunks, height range) and exit\n");
    std::printf("  --info-extract <dir> [--json]\n");
    std::printf("                         Walk extracted asset tree and report open-format coverage and exit\n");
    std::printf("  --list-missing-sidecars <dir> [--json]\n");
    std::printf("                         List proprietary files lacking open-format sidecars (one per line)\n");
    std::printf("  --info-zone <dir|json> [--json]\n");
    std::printf("                         Print zone.json fields (manifest, tiles, audio, flags) and exit\n");
    std::printf("  --info-creatures <p> [--json]\n");
    std::printf("                         Print creatures.json summary (counts, behaviors) and exit\n");
    std::printf("  --info-objects <p> [--json]\n");
    std::printf("                         Print objects.json summary (counts, types, scale range) and exit\n");
    std::printf("  --info-quests <p> [--json]\n");
    std::printf("                         Print quests.json summary (counts, rewards, chain errors) and exit\n");
    std::printf("  --info-wcp <wcp-path> [--json]\n");
    std::printf("                         Print WCP archive metadata (name, files) and exit\n");
    std::printf("  --list-wcp <wcp-path>  Print every file inside a WCP archive (sorted by path) and exit\n");
    std::printf("  --diff-wcp <a> <b> [--json]\n");
    std::printf("                         Compare two WCPs file-by-file; exit 0 if identical, 1 otherwise\n");
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
        "--info-extract", "--list-missing-sidecars",
        "--info-zone", "--info-wcp", "--list-wcp",
        "--unpack-wcp", "--pack-wcp",
        "--validate", "--validate-wom", "--validate-wob", "--validate-woc",
        "--validate-whm", "--validate-all", "--zone-summary",
        "--scaffold-zone", "--add-creature", "--add-object", "--add-quest",
        "--copy-zone",
        "--build-woc", "--regen-collision", "--fix-zone",
        "--export-png",
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
        if (std::strcmp(argv[i], "--diff-wcp") == 0 && i + 2 >= argc) {
            std::fprintf(stderr, "--diff-wcp requires two paths\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--add-creature") == 0 && i + 5 >= argc) {
            std::fprintf(stderr,
                "--add-creature requires <zoneDir> <name> <x> <y> <z>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--add-object") == 0 && i + 6 >= argc) {
            std::fprintf(stderr,
                "--add-object requires <zoneDir> <m2|wmo> <gamePath> <x> <y> <z>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--add-quest") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--add-quest requires <zoneDir> <title>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--copy-zone") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--copy-zone requires <srcDir> <newName>\n");
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
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            // Allow either "/path/to/file.wom" or "/path/to/file"; load() expects no extension.
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr, "WOM not found: %s.wom\n", base.c_str());
                return 1;
            }
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            if (jsonOut) {
                nlohmann::json j;
                j["wom"] = base + ".wom";
                j["version"] = wom.version;
                j["name"] = wom.name;
                j["vertices"] = wom.vertices.size();
                j["indices"] = wom.indices.size();
                j["triangles"] = wom.indices.size() / 3;
                j["textures"] = wom.texturePaths.size();
                j["bones"] = wom.bones.size();
                j["animations"] = wom.animations.size();
                j["batches"] = wom.batches.size();
                j["boundRadius"] = wom.boundRadius;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
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
        } else if (std::strcmp(argv[i], "--info-quests") == 0 && i + 1 < argc) {
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
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
            if (jsonOut) {
                nlohmann::json j;
                j["file"] = path;
                j["total"] = quests.size();
                j["chained"] = chained;
                j["withReward"] = withReward;
                j["withItems"] = withItems;
                j["totalXp"] = totalXp;
                j["avgXpPerQuest"] = quests.empty() ? 0.0
                                        : double(totalXp) / quests.size();
                j["objectives"] = {{"kill", objKill},
                                    {"collect", objCollect},
                                    {"talk", objTalk}};
                j["chainErrors"] = errors;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
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
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
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
            if (jsonOut) {
                nlohmann::json j;
                j["file"] = path;
                j["total"] = objs.size();
                j["m2"] = m2Count;
                j["wmo"] = wmoCount;
                j["uniquePaths"] = pathHist.size();
                if (!objs.empty()) {
                    j["scaleMin"] = minScale;
                    j["scaleMax"] = maxScale;
                }
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
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
        } else if (std::strcmp(argv[i], "--info-extract") == 0 && i + 1 < argc) {
            // Walk an extracted-asset directory and report counts by
            // extension + open-format coverage. Useful for seeing whether
            // a user ran asset_extract with --emit-open.
            std::string dataDir = argv[++i];
            // Optional --json after the dir for machine-readable output.
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(dataDir)) {
                std::fprintf(stderr, "info-extract: %s does not exist\n", dataDir.c_str());
                return 1;
            }
            // Per-format counts. Pair proprietary with open-format sidecar
            // so the report can show coverage percentages. Track bytes
            // separately for proprietary vs open so the user can see how
            // much disk a "purge proprietary after open conversion"
            // workflow would save (or cost — open formats are sometimes
            // larger, e.g. PNG vs DXT-compressed BLP).
            uint64_t blpCount = 0, pngSidecar = 0;
            uint64_t dbcCount = 0, jsonSidecar = 0;
            uint64_t m2Count  = 0, womSidecar = 0;
            uint64_t wmoCount = 0, wobSidecar = 0;
            uint64_t adtCount = 0, whmSidecar = 0;
            uint64_t totalBytes = 0;
            uint64_t propBytes = 0, openBytes = 0;
            for (auto& entry : fs::recursive_directory_iterator(dataDir)) {
                if (!entry.is_regular_file()) continue;
                uint64_t fsz = entry.file_size();
                totalBytes += fsz;
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                std::string base = entry.path().string();
                if (base.size() > ext.size()) base = base.substr(0, base.size() - ext.size());
                auto sidecarExists = [&](const char* sidecarExt) {
                    return fs::exists(base + sidecarExt);
                };
                if      (ext == ".blp") { blpCount++; propBytes += fsz; if (sidecarExists(".png"))  pngSidecar++; }
                else if (ext == ".dbc") { dbcCount++; propBytes += fsz; if (sidecarExists(".json")) jsonSidecar++; }
                else if (ext == ".m2")  { m2Count++;  propBytes += fsz; if (sidecarExists(".wom"))  womSidecar++; }
                else if (ext == ".wmo") {
                    propBytes += fsz;
                    std::string fname = entry.path().filename().string();
                    auto under = fname.rfind('_');
                    bool isGroup = (under != std::string::npos &&
                                    fname.size() - under == 8);
                    if (!isGroup) {
                        wmoCount++; if (sidecarExists(".wob")) wobSidecar++;
                    }
                }
                else if (ext == ".adt") { adtCount++; propBytes += fsz; if (sidecarExists(".whm")) whmSidecar++; }
                else if (ext == ".png" || ext == ".json" || ext == ".wom" ||
                         ext == ".wob" || ext == ".whm" || ext == ".wot" ||
                         ext == ".woc") {
                    openBytes += fsz;
                }
            }
            auto pct = [](uint64_t x, uint64_t total) {
                return total == 0 ? 0.0 : (100.0 * x) / total;
            };
            if (jsonOut) {
                // Machine-readable summary for CI scripts; matches the
                // structure of the human-readable lines below.
                nlohmann::json j;
                j["dir"] = dataDir;
                j["totalBytes"] = totalBytes;
                j["proprietaryBytes"] = propBytes;
                j["openBytes"] = openBytes;
                auto fmtFmt = [&](const char* name, uint64_t prop, uint64_t open) {
                    nlohmann::json f;
                    f["proprietary"] = prop;
                    f["sidecar"] = open;
                    f["coverage"] = pct(open, prop);
                    j[name] = f;
                };
                fmtFmt("blp_png",   blpCount, pngSidecar);
                fmtFmt("dbc_json",  dbcCount, jsonSidecar);
                fmtFmt("m2_wom",    m2Count,  womSidecar);
                fmtFmt("wmo_wob",   wmoCount, wobSidecar);
                fmtFmt("adt_whm",   adtCount, whmSidecar);
                uint64_t openTotal = pngSidecar + jsonSidecar + womSidecar +
                                     wobSidecar + whmSidecar;
                uint64_t propTotal = blpCount + dbcCount + m2Count +
                                     wmoCount + adtCount;
                j["overallCoverage"] = pct(openTotal, propTotal);
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Extracted asset tree: %s\n", dataDir.c_str());
            std::printf("  total bytes  : %.2f GB\n", totalBytes / (1024.0 * 1024.0 * 1024.0));
            std::printf("  BLP textures : %lu  (%lu PNG sidecar = %.1f%% open)\n",
                        blpCount, pngSidecar, pct(pngSidecar, blpCount));
            std::printf("  DBC tables   : %lu  (%lu JSON sidecar = %.1f%% open)\n",
                        dbcCount, jsonSidecar, pct(jsonSidecar, dbcCount));
            std::printf("  M2 models    : %lu  (%lu WOM sidecar = %.1f%% open)\n",
                        m2Count, womSidecar, pct(womSidecar, m2Count));
            std::printf("  WMO buildings: %lu  (%lu WOB sidecar = %.1f%% open)\n",
                        wmoCount, wobSidecar, pct(wobSidecar, wmoCount));
            std::printf("  ADT terrain  : %lu  (%lu WHM sidecar = %.1f%% open)\n",
                        adtCount, whmSidecar, pct(whmSidecar, adtCount));
            uint64_t openTotal = pngSidecar + jsonSidecar + womSidecar + wobSidecar + whmSidecar;
            uint64_t propTotal = blpCount + dbcCount + m2Count + wmoCount + adtCount;
            std::printf("  overall open-format coverage: %.1f%%\n", pct(openTotal, propTotal));
            // Disk-usage breakdown: shows roughly how big a purge-proprietary
            // workflow would shrink the tree (or how much extra a dual-format
            // extraction costs).
            const double mb = 1024.0 * 1024.0;
            std::printf("  proprietary bytes: %.1f MB\n", propBytes / mb);
            std::printf("  open-format bytes: %.1f MB", openBytes / mb);
            if (propBytes > 0) {
                std::printf(" (%.1f%% of proprietary)",
                            100.0 * static_cast<double>(openBytes) / propBytes);
            }
            std::printf("\n");
            std::printf("  (run `asset_extract --emit-open` to fill missing sidecars)\n");
            return 0;
        } else if (std::strcmp(argv[i], "--list-missing-sidecars") == 0 && i + 1 < argc) {
            // Actionable counterpart to --info-extract: emit one line per
            // proprietary file lacking its open-format sidecar. Pipe into
            // xargs to drive a targeted re-extract:
            //   wowee_editor --list-missing-sidecars Data/ |
            //     awk '/\.blp$/ {print}' |
            //     xargs asset_extract --emit-png-only
            std::string dataDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(dataDir)) {
                std::fprintf(stderr, "list-missing-sidecars: %s does not exist\n",
                             dataDir.c_str());
                return 1;
            }
            std::vector<std::string> missingPng, missingJson, missingWom,
                                     missingWob, missingWhm;
            for (auto& entry : fs::recursive_directory_iterator(dataDir)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                std::string base = entry.path().string();
                if (base.size() > ext.size())
                    base = base.substr(0, base.size() - ext.size());
                auto missing = [&](const char* sidecarExt) {
                    return !fs::exists(base + sidecarExt);
                };
                if (ext == ".blp" && missing(".png"))
                    missingPng.push_back(entry.path().string());
                else if (ext == ".dbc" && missing(".json"))
                    missingJson.push_back(entry.path().string());
                else if (ext == ".m2" && missing(".wom"))
                    missingWom.push_back(entry.path().string());
                else if (ext == ".wmo") {
                    // Group files (Foo_NNN.wmo) don't get individual sidecars
                    // — only the parent file gets a .wob.
                    std::string fname = entry.path().filename().string();
                    auto under = fname.rfind('_');
                    bool isGroup = (under != std::string::npos &&
                                    fname.size() - under == 8);
                    if (!isGroup && missing(".wob"))
                        missingWob.push_back(entry.path().string());
                }
                else if (ext == ".adt" && missing(".whm"))
                    missingWhm.push_back(entry.path().string());
            }
            size_t total = missingPng.size() + missingJson.size() +
                           missingWom.size() + missingWob.size() +
                           missingWhm.size();
            if (jsonOut) {
                nlohmann::json j;
                j["dir"] = dataDir;
                j["totalMissing"] = total;
                j["missing"] = {
                    {"png",  missingPng},
                    {"json", missingJson},
                    {"wom",  missingWom},
                    {"wob",  missingWob},
                    {"whm",  missingWhm},
                };
                std::printf("%s\n", j.dump(2).c_str());
                return total == 0 ? 0 : 1;
            }
            // Plain mode: one path per line, sorted by group, prefixed with
            // the missing extension so awk/grep can filter.
            auto emit = [](const char* tag, const std::vector<std::string>& files) {
                for (const auto& f : files) std::printf("%s\t%s\n", tag, f.c_str());
            };
            emit("png",  missingPng);
            emit("json", missingJson);
            emit("wom",  missingWom);
            emit("wob",  missingWob);
            emit("whm",  missingWhm);
            std::fprintf(stderr,
                "%zu missing (PNG=%zu JSON=%zu WOM=%zu WOB=%zu WHM=%zu)\n",
                total, missingPng.size(), missingJson.size(),
                missingWom.size(), missingWob.size(), missingWhm.size());
            return total == 0 ? 0 : 1;
        } else if (std::strcmp(argv[i], "--info-zone") == 0 && i + 1 < argc) {
            // Parse a zone.json and print every manifest field. Useful when
            // diffing two zones or auditing the audio/flag setup before
            // packing into a WCP.
            std::string zonePath = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            // Accept either a directory or the zone.json itself.
            if (fs::is_directory(zonePath)) zonePath += "/zone.json";
            wowee::editor::ZoneManifest manifest;
            if (!manifest.load(zonePath)) {
                std::fprintf(stderr, "Failed to load zone.json: %s\n", zonePath.c_str());
                return 1;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["file"] = zonePath;
                j["mapName"] = manifest.mapName;
                j["displayName"] = manifest.displayName;
                j["mapId"] = manifest.mapId;
                j["biome"] = manifest.biome;
                j["baseHeight"] = manifest.baseHeight;
                j["hasCreatures"] = manifest.hasCreatures;
                j["description"] = manifest.description;
                nlohmann::json tilesArr = nlohmann::json::array();
                for (const auto& t : manifest.tiles)
                    tilesArr.push_back({t.first, t.second});
                j["tiles"] = tilesArr;
                j["flags"] = {{"allowFlying", manifest.allowFlying},
                               {"pvpEnabled", manifest.pvpEnabled},
                               {"isIndoor", manifest.isIndoor},
                               {"isSanctuary", manifest.isSanctuary}};
                if (!manifest.musicTrack.empty() || !manifest.ambienceDay.empty()) {
                    nlohmann::json audio;
                    if (!manifest.musicTrack.empty()) {
                        audio["music"] = manifest.musicTrack;
                        audio["musicVolume"] = manifest.musicVolume;
                    }
                    if (!manifest.ambienceDay.empty()) {
                        audio["ambienceDay"] = manifest.ambienceDay;
                        audio["ambienceVolume"] = manifest.ambienceVolume;
                    }
                    if (!manifest.ambienceNight.empty())
                        audio["ambienceNight"] = manifest.ambienceNight;
                    j["audio"] = audio;
                }
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("zone.json: %s\n", zonePath.c_str());
            std::printf("  mapName     : %s\n", manifest.mapName.c_str());
            std::printf("  displayName : %s\n", manifest.displayName.c_str());
            std::printf("  mapId       : %u\n", manifest.mapId);
            std::printf("  biome       : %s\n", manifest.biome.c_str());
            std::printf("  baseHeight  : %.2f\n", manifest.baseHeight);
            std::printf("  hasCreatures: %s\n", manifest.hasCreatures ? "yes" : "no");
            std::printf("  description : %s\n", manifest.description.c_str());
            std::printf("  tiles       : %zu\n", manifest.tiles.size());
            for (const auto& t : manifest.tiles)
                std::printf("    (%d, %d)\n", t.first, t.second);
            std::printf("  flags       : %s%s%s%s\n",
                        manifest.allowFlying  ? "fly " : "",
                        manifest.pvpEnabled   ? "pvp " : "",
                        manifest.isIndoor     ? "indoor " : "",
                        manifest.isSanctuary  ? "sanctuary" : "");
            if (!manifest.musicTrack.empty() || !manifest.ambienceDay.empty()) {
                std::printf("  audio       :\n");
                if (!manifest.musicTrack.empty())
                    std::printf("    music     : %s (vol=%.2f)\n",
                                manifest.musicTrack.c_str(), manifest.musicVolume);
                if (!manifest.ambienceDay.empty())
                    std::printf("    ambience  : %s (vol=%.2f)\n",
                                manifest.ambienceDay.c_str(), manifest.ambienceVolume);
                if (!manifest.ambienceNight.empty())
                    std::printf("    night amb : %s\n", manifest.ambienceNight.c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-creatures") == 0 && i + 1 < argc) {
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
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
            if (jsonOut) {
                nlohmann::json j;
                j["file"] = path;
                j["total"] = spawns.size();
                j["hostile"] = hostile;
                j["questgiver"] = questgiver;
                j["vendor"] = vendor;
                j["trainer"] = trainer;
                j["behavior"] = {{"stationary", stationary},
                                  {"wander", wander},
                                  {"patrol", patrol}};
                j["uniqueDisplayIds"] = displayIdHist.size();
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
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
        } else if (std::strcmp(argv[i], "--diff-wcp") == 0 && i + 2 < argc) {
            // Print which files differ between two WCP archives. Useful
            // when verifying that an authoring tweak only changed what
            // it claimed to change, or when comparing pack-WCP output
            // across editor versions for regression detection.
            std::string aPath = argv[++i];
            std::string bPath = argv[++i];
            // Optional --json after both paths for machine-readable output.
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            wowee::editor::ContentPackInfo aInfo, bInfo;
            if (!wowee::editor::ContentPacker::readInfo(aPath, aInfo) ||
                !wowee::editor::ContentPacker::readInfo(bPath, bInfo)) {
                std::fprintf(stderr, "Failed to read WCP info\n");
                return 1;
            }
            std::unordered_map<std::string, uint64_t> aFiles, bFiles;
            for (const auto& f : aInfo.files) aFiles[f.path] = f.size;
            for (const auto& f : bInfo.files) bFiles[f.path] = f.size;

            int onlyA = 0, onlyB = 0, sizeChanged = 0, identical = 0;
            std::vector<std::string> onlyAList, onlyBList, changedList;
            // For JSON we want size-change rows as structured records, not
            // pre-formatted strings — collect both forms in one pass.
            struct ChangedRow { std::string path; uint64_t aSize, bSize; };
            std::vector<ChangedRow> changedRows;
            for (const auto& [p, sz] : aFiles) {
                auto it = bFiles.find(p);
                if (it == bFiles.end()) { onlyA++; onlyAList.push_back(p); }
                else if (it->second != sz) {
                    sizeChanged++;
                    changedList.push_back(p + " (" + std::to_string(sz) + " -> " +
                                          std::to_string(it->second) + ")");
                    changedRows.push_back({p, sz, it->second});
                } else identical++;
            }
            for (const auto& [p, sz] : bFiles) {
                if (aFiles.find(p) == aFiles.end()) { onlyB++; onlyBList.push_back(p); }
            }
            std::sort(onlyAList.begin(), onlyAList.end());
            std::sort(onlyBList.begin(), onlyBList.end());
            std::sort(changedList.begin(), changedList.end());
            if (jsonOut) {
                nlohmann::json j;
                j["a"] = aPath;
                j["b"] = bPath;
                j["identical"] = identical;
                j["changed"] = sizeChanged;
                j["onlyA"] = onlyA;
                j["onlyB"] = onlyB;
                std::sort(changedRows.begin(), changedRows.end(),
                          [](const auto& x, const auto& y) { return x.path < y.path; });
                nlohmann::json changedArr = nlohmann::json::array();
                for (const auto& c : changedRows) {
                    changedArr.push_back({{"path", c.path},
                                           {"aSize", c.aSize},
                                           {"bSize", c.bSize}});
                }
                j["changedFiles"] = changedArr;
                j["onlyAFiles"] = onlyAList;
                j["onlyBFiles"] = onlyBList;
                std::printf("%s\n", j.dump(2).c_str());
                return (onlyA + onlyB + sizeChanged) == 0 ? 0 : 1;
            }
            std::printf("Diff: %s vs %s\n", aPath.c_str(), bPath.c_str());
            std::printf("  identical : %d\n", identical);
            std::printf("  changed   : %d\n", sizeChanged);
            std::printf("  only in A : %d\n", onlyA);
            std::printf("  only in B : %d\n", onlyB);
            for (const auto& s : changedList) std::printf("  ~  %s\n", s.c_str());
            for (const auto& s : onlyAList)   std::printf("  -  %s\n", s.c_str());
            for (const auto& s : onlyBList)   std::printf("  +  %s\n", s.c_str());
            return (onlyA + onlyB + sizeChanged) == 0 ? 0 : 1;
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
            // Optional --json after the path for machine-readable output.
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            wowee::editor::ContentPackInfo info;
            if (!wowee::editor::ContentPacker::readInfo(path, info)) {
                std::fprintf(stderr, "Failed to read WCP: %s\n", path.c_str());
                return 1;
            }
            // Per-category file totals
            std::unordered_map<std::string, size_t> byCat;
            uint64_t totalSize = 0;
            for (const auto& f : info.files) {
                byCat[f.category]++;
                totalSize += f.size;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["wcp"] = path;
                j["name"] = info.name;
                j["author"] = info.author;
                j["description"] = info.description;
                j["version"] = info.version;
                j["format"] = info.format;
                j["mapId"] = info.mapId;
                j["fileCount"] = info.files.size();
                j["totalBytes"] = totalSize;
                nlohmann::json categories = nlohmann::json::object();
                for (const auto& [cat, count] : byCat) categories[cat] = count;
                j["categories"] = categories;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("WCP: %s\n", path.c_str());
            std::printf("  name        : %s\n", info.name.c_str());
            std::printf("  author      : %s\n", info.author.c_str());
            std::printf("  description : %s\n", info.description.c_str());
            std::printf("  version     : %s\n", info.version.c_str());
            std::printf("  format      : %s\n", info.format.c_str());
            std::printf("  mapId       : %u\n", info.mapId);
            std::printf("  files       : %zu\n", info.files.size());
            for (const auto& [cat, count] : byCat) {
                std::printf("    %-10s : %zu\n", cat.c_str(), count);
            }
            std::printf("  total bytes : %.2f MB\n", totalSize / (1024.0 * 1024.0));
            return 0;
        } else if (std::strcmp(argv[i], "--info-wot") == 0 && i + 1 < argc) {
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
        } else if (std::strcmp(argv[i], "--info-woc") == 0 && i + 1 < argc) {
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
        } else if (std::strcmp(argv[i], "--zone-summary") == 0 && i + 1 < argc) {
            // One-shot zone overview: validate + creature/object/quest counts.
            // Collapses the most common multi-step inspection into a single
            // command; useful for CI reports and quick sanity checks.
            std::string zoneDir = argv[++i];
            // Optional --json after the dir for machine-readable output.
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr, "zone-summary: %s does not exist\n", zoneDir.c_str());
                return 1;
            }
            auto v = wowee::editor::ContentPacker::validateZone(zoneDir);

            // Read creature/object/quest data once so both human and JSON
            // outputs share the same numbers.
            int creatureTotal = 0, hostile = 0, qg = 0, vendor = 0;
            int objectTotal = 0, m2Count = 0, wmoCount = 0;
            int questTotal = 0, chainWarnings = 0;
            std::string creaturesPath = zoneDir + "/creatures.json";
            if (fs::exists(creaturesPath)) {
                wowee::editor::NpcSpawner sp;
                if (sp.loadFromFile(creaturesPath)) {
                    creatureTotal = static_cast<int>(sp.getSpawns().size());
                    for (const auto& s : sp.getSpawns()) {
                        if (s.hostile) hostile++;
                        if (s.questgiver) qg++;
                        if (s.vendor) vendor++;
                    }
                }
            }
            std::string objectsPath = zoneDir + "/objects.json";
            if (fs::exists(objectsPath)) {
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile(objectsPath)) {
                    objectTotal = static_cast<int>(op.getObjects().size());
                    for (const auto& o : op.getObjects()) {
                        if (o.type == wowee::editor::PlaceableType::M2) m2Count++;
                        else wmoCount++;
                    }
                }
            }
            std::string questsPath = zoneDir + "/quests.json";
            if (fs::exists(questsPath)) {
                wowee::editor::QuestEditor qe;
                if (qe.loadFromFile(questsPath)) {
                    questTotal = static_cast<int>(qe.getQuests().size());
                    std::vector<std::string> errors;
                    qe.validateChains(errors);
                    chainWarnings = static_cast<int>(errors.size());
                }
            }

            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["score"] = v.openFormatScore();
                j["maxScore"] = 7;
                j["formats"] = v.summary();
                j["counts"] = {
                    {"wot", v.wotCount}, {"whm", v.whmCount},
                    {"wom", v.womCount}, {"wob", v.wobCount},
                    {"woc", v.wocCount}, {"png", v.pngCount},
                };
                j["creatures"] = {
                    {"total", creatureTotal},
                    {"hostile", hostile},
                    {"questgiver", qg},
                    {"vendor", vendor},
                };
                j["objects"] = {
                    {"total", objectTotal},
                    {"m2", m2Count},
                    {"wmo", wmoCount},
                };
                j["quests"] = {
                    {"total", questTotal},
                    {"chainWarnings", chainWarnings},
                };
                std::printf("%s\n", j.dump(2).c_str());
                return v.openFormatScore() == 7 ? 0 : 1;
            }
            std::printf("Zone: %s\n", zoneDir.c_str());
            std::printf("  open formats : %d/7  (%s)\n",
                        v.openFormatScore(), v.summary().c_str());
            std::printf("  WOT/WHM      : %d/%d   WOM: %d   WOB: %d   WOC: %d   PNG: %d\n",
                        v.wotCount, v.whmCount, v.womCount, v.wobCount,
                        v.wocCount, v.pngCount);
            if (creatureTotal > 0) {
                std::printf("  creatures    : %d  (%d hostile, %d quest, %d vendor)\n",
                            creatureTotal, hostile, qg, vendor);
            }
            if (objectTotal > 0) {
                std::printf("  objects      : %d  (%d M2, %d WMO)\n",
                            objectTotal, m2Count, wmoCount);
            }
            if (questTotal > 0) {
                std::printf("  quests       : %d  (%d chain warnings)\n",
                            questTotal, chainWarnings);
            }
            return v.openFormatScore() == 7 ? 0 : 1;
        } else if (std::strcmp(argv[i], "--validate") == 0 && i + 1 < argc) {
            std::string zoneDir = argv[++i];
            // Optional --json after the dir for machine-readable output
            // (matches --info-extract --json).
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            auto v = wowee::editor::ContentPacker::validateZone(zoneDir);
            int score = v.openFormatScore();
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["score"] = score;
                j["maxScore"] = 7;
                j["formats"] = v.summary();
                auto fmt = [&](const char* name, bool present, int count,
                                bool valid = true, int invalid = 0) {
                    nlohmann::json f;
                    f["present"] = present;
                    f["count"] = count;
                    f["valid"] = valid;
                    if (invalid > 0) f["invalid"] = invalid;
                    j[name] = f;
                };
                fmt("wot", v.hasWot, v.wotCount);
                fmt("whm", v.hasWhm, v.whmCount, v.whmValid);
                fmt("wom", v.hasWom, v.womCount, v.womValid, v.womInvalidCount);
                fmt("wob", v.hasWob, v.wobCount, v.wobValid, v.wobInvalidCount);
                fmt("woc", v.hasWoc, v.wocCount, v.wocValid, v.wocInvalidCount);
                fmt("png", v.hasPng, v.pngCount);
                j["zoneJson"]   = v.hasZoneJson;
                j["creatures"]  = v.hasCreatures;
                j["quests"]     = v.hasQuests;
                j["objects"]    = v.hasObjects;
                std::printf("%s\n", j.dump(2).c_str());
                return score == 7 ? 0 : 1;
            }
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
        } else if (std::strcmp(argv[i], "--validate-wom") == 0 && i + 1 < argc) {
            // Deep consistency check on a single WOM. The loader is
            // deliberately lenient (it accepts older/partial files), so
            // silent corruption can survive load. This walks every cross-
            // reference and reports anything out of range.
            std::string base = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr, "WOM not found: %s.wom\n", base.c_str());
                return 1;
            }
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            auto errors = validateWomErrors(wom);
            if (jsonOut) {
                nlohmann::json j;
                j["wom"] = base + ".wom";
                j["version"] = wom.version;
                j["errorCount"] = errors.size();
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return errors.empty() ? 0 : 1;
            }
            std::printf("WOM: %s.wom (v%u)\n", base.c_str(), wom.version);
            if (errors.empty()) {
                std::printf("  PASSED — %zu verts, %zu indices, %zu bones, %zu anims, %zu batches\n",
                            wom.vertices.size(), wom.indices.size(),
                            wom.bones.size(), wom.animations.size(),
                            wom.batches.size());
                return 0;
            }
            std::printf("  FAILED — %zu error(s):\n", errors.size());
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--validate-wob") == 0 && i + 1 < argc) {
            // Deep consistency check on a single WOB. Like --validate-wom
            // but covering buildings: per-group index/material refs, portal
            // group references, doodad scales, and bounds.
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
            auto errors = validateWobErrors(bld);
            if (jsonOut) {
                nlohmann::json j;
                j["wob"] = base + ".wob";
                j["name"] = bld.name;
                j["groups"] = bld.groups.size();
                j["portals"] = bld.portals.size();
                j["doodads"] = bld.doodads.size();
                j["errorCount"] = errors.size();
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return errors.empty() ? 0 : 1;
            }
            std::printf("WOB: %s.wob\n", base.c_str());
            std::printf("  name      : %s\n", bld.name.c_str());
            if (errors.empty()) {
                std::printf("  PASSED — %zu groups, %zu portals, %zu doodads\n",
                            bld.groups.size(), bld.portals.size(), bld.doodads.size());
                return 0;
            }
            std::printf("  FAILED — %zu error(s):\n", errors.size());
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--validate-woc") == 0 && i + 1 < argc) {
            // Deep check on a WOC collision mesh — finite vertex coords,
            // non-degenerate triangles, valid flag bits, sane bounds.
            // Catches corruption that breaks movement queries silently.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "WOC not found: %s\n", path.c_str());
                return 1;
            }
            auto woc = wowee::pipeline::WoweeCollisionBuilder::load(path);
            auto errors = validateWocErrors(woc);
            if (jsonOut) {
                nlohmann::json j;
                j["woc"] = path;
                j["triangles"] = woc.triangles.size();
                j["walkable"] = woc.walkableCount();
                j["steep"] = woc.steepCount();
                j["tile"] = {woc.tileX, woc.tileY};
                j["errorCount"] = errors.size();
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return errors.empty() ? 0 : 1;
            }
            std::printf("WOC: %s\n", path.c_str());
            std::printf("  tile      : (%u, %u)\n", woc.tileX, woc.tileY);
            if (errors.empty()) {
                std::printf("  PASSED — %zu triangles (%zu walkable, %zu steep)\n",
                            woc.triangles.size(),
                            woc.walkableCount(), woc.steepCount());
                return 0;
            }
            std::printf("  FAILED — %zu error(s):\n", errors.size());
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--validate-whm") == 0 && i + 1 < argc) {
            // Deep check on a WHM/WOT terrain pair — finite heights,
            // chunks present, placements within name-table bounds.
            std::string base = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            for (const char* ext : {".wot", ".whm"}) {
                if (base.size() >= 4 && base.substr(base.size() - 4) == ext) {
                    base = base.substr(0, base.size() - 4);
                    break;
                }
            }
            if (!wowee::pipeline::WoweeTerrainLoader::exists(base)) {
                std::fprintf(stderr, "WHM/WOT not found: %s.{whm,wot}\n", base.c_str());
                return 1;
            }
            wowee::pipeline::ADTTerrain terrain;
            wowee::pipeline::WoweeTerrainLoader::load(base, terrain);
            auto errors = validateWhmErrors(terrain);
            if (jsonOut) {
                nlohmann::json j;
                j["whm"] = base + ".whm";
                j["wot"] = base + ".wot";
                j["coord"] = {terrain.coord.x, terrain.coord.y};
                j["doodadPlacements"] = terrain.doodadPlacements.size();
                j["wmoPlacements"] = terrain.wmoPlacements.size();
                int loadedChunks = 0;
                for (const auto& c : terrain.chunks) if (c.heightMap.isLoaded()) loadedChunks++;
                j["loadedChunks"] = loadedChunks;
                j["errorCount"] = errors.size();
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return errors.empty() ? 0 : 1;
            }
            std::printf("WHM/WOT: %s.{whm,wot}\n", base.c_str());
            std::printf("  tile      : (%d, %d)\n", terrain.coord.x, terrain.coord.y);
            if (errors.empty()) {
                int loaded = 0;
                for (const auto& c : terrain.chunks) if (c.heightMap.isLoaded()) loaded++;
                std::printf("  PASSED — %d/256 chunks, %zu doodad + %zu wmo placements\n",
                            loaded, terrain.doodadPlacements.size(),
                            terrain.wmoPlacements.size());
                return 0;
            }
            std::printf("  FAILED — %zu error(s):\n", errors.size());
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--validate-all") == 0 && i + 1 < argc) {
            // CI gate: walk a directory, run every per-format validator on
            // every matching file. Aggregate counts for fast triage; per-
            // file errors are listed (capped at 20) so the user knows which
            // file to drill into with --validate-{wom,wob,woc,whm}.
            std::string root = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(root)) {
                std::fprintf(stderr, "validate-all: not found: %s\n", root.c_str());
                return 1;
            }
            int womTotal = 0, womFail = 0, wobTotal = 0, wobFail = 0;
            int wocTotal = 0, wocFail = 0, whmTotal = 0, whmFail = 0;
            int totalErrors = 0;
            std::vector<std::pair<std::string, std::vector<std::string>>> failures;
            auto recordFailure = [&](const std::string& path,
                                      const std::vector<std::string>& errs) {
                totalErrors += errs.size();
                if (failures.size() < 20) failures.push_back({path, errs});
            };
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                std::string base = entry.path().string();
                base = base.substr(0, base.size() - ext.size());
                if (ext == ".wom") {
                    womTotal++;
                    auto wom = wowee::pipeline::WoweeModelLoader::load(base);
                    auto errs = validateWomErrors(wom);
                    if (!errs.empty()) { womFail++; recordFailure(entry.path().string(), errs); }
                } else if (ext == ".wob") {
                    wobTotal++;
                    auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
                    auto errs = validateWobErrors(bld);
                    if (!errs.empty()) { wobFail++; recordFailure(entry.path().string(), errs); }
                } else if (ext == ".woc") {
                    wocTotal++;
                    auto woc = wowee::pipeline::WoweeCollisionBuilder::load(entry.path().string());
                    auto errs = validateWocErrors(woc);
                    if (!errs.empty()) { wocFail++; recordFailure(entry.path().string(), errs); }
                } else if (ext == ".whm") {
                    // Only validate via the .whm half — .wot is its sidecar
                    // and gets pulled in by load(base).
                    whmTotal++;
                    wowee::pipeline::ADTTerrain terrain;
                    wowee::pipeline::WoweeTerrainLoader::load(base, terrain);
                    auto errs = validateWhmErrors(terrain);
                    if (!errs.empty()) { whmFail++; recordFailure(entry.path().string(), errs); }
                }
            }
            int allPassed = (womFail == 0 && wobFail == 0 &&
                             wocFail == 0 && whmFail == 0);
            int totalFiles = womTotal + wobTotal + wocTotal + whmTotal;
            if (jsonOut) {
                nlohmann::json j;
                j["root"] = root;
                j["wom"] = {{"total", womTotal}, {"failed", womFail}};
                j["wob"] = {{"total", wobTotal}, {"failed", wobFail}};
                j["woc"] = {{"total", wocTotal}, {"failed", wocFail}};
                j["whm"] = {{"total", whmTotal}, {"failed", whmFail}};
                j["totalErrors"] = totalErrors;
                j["passed"] = bool(allPassed);
                nlohmann::json failArr = nlohmann::json::array();
                for (const auto& [path, errs] : failures) {
                    failArr.push_back({{"file", path}, {"errors", errs}});
                }
                j["failures"] = failArr;
                std::printf("%s\n", j.dump(2).c_str());
                return allPassed ? 0 : 1;
            }
            std::printf("validate-all: %s\n", root.c_str());
            std::printf("  WOM: %d total, %d failed\n", womTotal, womFail);
            std::printf("  WOB: %d total, %d failed\n", wobTotal, wobFail);
            std::printf("  WOC: %d total, %d failed\n", wocTotal, wocFail);
            std::printf("  WHM: %d total, %d failed\n", whmTotal, whmFail);
            if (allPassed) {
                std::printf("  PASSED — all %d file(s) clean\n", totalFiles);
                return 0;
            }
            std::printf("  FAILED — %d total error(s) across %zu file(s):\n",
                        totalErrors, failures.size());
            for (const auto& [path, errs] : failures) {
                std::printf("  %s:\n", path.c_str());
                for (const auto& e : errs) std::printf("    - %s\n", e.c_str());
            }
            return 1;
        } else if (std::strcmp(argv[i], "--export-png") == 0 && i + 1 < argc) {
            // Render heightmap, normal-map, and zone-map PNG previews for a
            // terrain. Useful for portfolio screenshots, ground-truth map
            // comparison, and quick visual validation without launching GUI.
            std::string base = argv[++i];
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
                std::fprintf(stderr, "Failed to load terrain: %s\n", base.c_str());
                return 1;
            }
            wowee::editor::WoweeTerrain::exportHeightmapPreview(terrain, base + "_heightmap.png");
            wowee::editor::WoweeTerrain::exportNormalMap(terrain, base + "_normals.png");
            wowee::editor::WoweeTerrain::exportZoneMap(terrain, base + "_zone.png", 512);
            std::printf("Exported PNGs: %s_{heightmap,normals,zone}.png\n", base.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--fix-zone") == 0 && i + 1 < argc) {
            // Re-parse + re-save every JSON/binary file in a zone to apply
            // the editor's load-time scrubs and save-time caps. Useful when
            // an old zone was created before recent hardening — running
            // this once cleans up NaN/oversize fields without touching
            // the editor GUI.
            std::string zoneDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr, "fix-zone: %s does not exist\n", zoneDir.c_str());
                return 1;
            }
            int touched = 0;
            // zone.json
            {
                wowee::editor::ZoneManifest m;
                std::string p = zoneDir + "/zone.json";
                if (fs::exists(p) && m.load(p) && m.save(p)) touched++;
            }
            // creatures.json
            {
                wowee::editor::NpcSpawner sp;
                std::string p = zoneDir + "/creatures.json";
                if (fs::exists(p) && sp.loadFromFile(p) && sp.saveToFile(p)) touched++;
            }
            // objects.json
            {
                wowee::editor::ObjectPlacer op;
                std::string p = zoneDir + "/objects.json";
                if (fs::exists(p) && op.loadFromFile(p) && op.saveToFile(p)) touched++;
            }
            // quests.json
            {
                wowee::editor::QuestEditor qe;
                std::string p = zoneDir + "/quests.json";
                if (fs::exists(p) && qe.loadFromFile(p) && qe.saveToFile(p)) touched++;
            }
            // WHM/WOT pairs and WoB files would need full pipeline access;
            // skip them — the editor opens them on next zone load anyway,
            // and the load-time scrubs run then.
            std::printf("fix-zone: cleaned %d files in %s\n", touched, zoneDir.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--regen-collision") == 0 && i + 1 < argc) {
            // Find all WHM/WOT pairs under a zone dir and rebuild WOC for each.
            // Useful after sculpting changes when you want to re-derive
            // collision in batch instead of one tile at a time.
            std::string zoneDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr, "regen-collision: %s does not exist\n",
                             zoneDir.c_str());
                return 1;
            }
            int rebuilt = 0, failed = 0;
            for (auto& entry : fs::recursive_directory_iterator(zoneDir)) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() != ".whm") continue;
                std::string base = entry.path().string();
                base = base.substr(0, base.size() - 4); // strip .whm
                wowee::pipeline::ADTTerrain terrain;
                if (!wowee::pipeline::WoweeTerrainLoader::load(base, terrain)) {
                    std::fprintf(stderr, "  FAILED to load: %s\n", base.c_str());
                    failed++;
                    continue;
                }
                auto col = wowee::pipeline::WoweeCollisionBuilder::fromTerrain(terrain);
                std::string outPath = base + ".woc";
                if (wowee::pipeline::WoweeCollisionBuilder::save(col, outPath)) {
                    std::printf("  WOC rebuilt: %s (%zu triangles)\n",
                                outPath.c_str(), col.triangles.size());
                    rebuilt++;
                } else {
                    std::fprintf(stderr, "  FAILED to save: %s\n", outPath.c_str());
                    failed++;
                }
            }
            std::printf("regen-collision: %d rebuilt, %d failed\n", rebuilt, failed);
            return failed > 0 ? 1 : 0;
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
        } else if (std::strcmp(argv[i], "--add-quest") == 0 && i + 2 < argc) {
            // Append a single quest to a zone's quests.json.
            // Args: <zoneDir> <title> [giverId] [turnInId] [xp] [level]
            std::string zoneDir = argv[++i];
            std::string title = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr, "add-quest: zone '%s' does not exist\n",
                             zoneDir.c_str());
                return 1;
            }
            wowee::editor::Quest q;
            q.title = title;
            // Optional positional args after title. Each is read in order;
            // an empty string or '-' stops consumption so users can omit
            // later fields.
            auto tryReadUint = [&](uint32_t& target) {
                if (i + 1 >= argc || argv[i + 1][0] == '-') return false;
                try {
                    target = static_cast<uint32_t>(std::stoul(argv[i + 1]));
                    ++i;
                    return true;
                } catch (...) { return false; }
            };
            tryReadUint(q.questGiverNpcId);
            tryReadUint(q.turnInNpcId);
            tryReadUint(q.reward.xp);
            tryReadUint(q.requiredLevel);
            wowee::editor::QuestEditor qe;
            std::string path = zoneDir + "/quests.json";
            if (fs::exists(path)) qe.loadFromFile(path);
            qe.addQuest(q);
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "add-quest: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Added quest '%s' to %s (now %zu total)\n",
                        title.c_str(), path.c_str(), qe.questCount());
            return 0;
        } else if (std::strcmp(argv[i], "--add-object") == 0 && i + 5 < argc) {
            // Append a single object placement to a zone's objects.json.
            // Args: <zoneDir> <m2|wmo> <gamePath> <x> <y> <z> [scale]
            std::string zoneDir = argv[++i];
            std::string typeStr = argv[++i];
            std::string gamePath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr, "add-object: zone '%s' does not exist\n",
                             zoneDir.c_str());
                return 1;
            }
            wowee::editor::PlaceableType ptype;
            if (typeStr == "m2") ptype = wowee::editor::PlaceableType::M2;
            else if (typeStr == "wmo") ptype = wowee::editor::PlaceableType::WMO;
            else {
                std::fprintf(stderr, "add-object: type must be 'm2' or 'wmo'\n");
                return 1;
            }
            glm::vec3 pos;
            try {
                pos.x = std::stof(argv[++i]);
                pos.y = std::stof(argv[++i]);
                pos.z = std::stof(argv[++i]);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "add-object: bad coordinate (%s)\n", e.what());
                return 1;
            }
            wowee::editor::ObjectPlacer placer;
            std::string path = zoneDir + "/objects.json";
            if (fs::exists(path)) placer.loadFromFile(path);
            placer.setActivePath(gamePath, ptype);
            placer.placeObject(pos);
            // Optional scale after coordinates.
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try {
                    float scale = std::stof(argv[++i]);
                    if (std::isfinite(scale) && scale > 0.0f) {
                        // Set scale on the just-placed object (last in list).
                        placer.getObjects().back().scale = scale;
                    }
                } catch (...) {}
            }
            if (!placer.saveToFile(path)) {
                std::fprintf(stderr, "add-object: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Added %s '%s' to %s (now %zu total)\n",
                        typeStr.c_str(), gamePath.c_str(), path.c_str(),
                        placer.getObjects().size());
            return 0;
        } else if (std::strcmp(argv[i], "--add-creature") == 0 && i + 4 < argc) {
            // Append a single creature spawn to a zone's creatures.json.
            // Args: <zoneDir> <name> <x> <y> <z> [displayId] [level]
            // Useful for batch-populating zones via shell script without
            // launching the GUI placement tool.
            std::string zoneDir = argv[++i];
            std::string name = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr, "add-creature: zone '%s' does not exist\n",
                             zoneDir.c_str());
                return 1;
            }
            wowee::editor::CreatureSpawn s;
            s.name = name;
            try {
                s.position.x = std::stof(argv[++i]);
                s.position.y = std::stof(argv[++i]);
                s.position.z = std::stof(argv[++i]);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "add-creature: bad coordinate (%s)\n", e.what());
                return 1;
            }
            // Optional displayId (positional, after coordinates).
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try {
                    s.displayId = static_cast<uint32_t>(std::stoul(argv[++i]));
                } catch (...) { /* leave 0 → SQL exporter substitutes 11707 */ }
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try {
                    s.level = static_cast<uint32_t>(std::stoul(argv[++i]));
                } catch (...) { /* leave default 1 */ }
            }
            // Load existing spawns (if any), append, save.
            wowee::editor::NpcSpawner spawner;
            std::string path = zoneDir + "/creatures.json";
            if (fs::exists(path)) spawner.loadFromFile(path);
            spawner.placeCreature(s);
            if (!spawner.saveToFile(path)) {
                std::fprintf(stderr, "add-creature: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Added creature '%s' to %s (now %zu total)\n",
                        name.c_str(), path.c_str(), spawner.spawnCount());
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
        } else if (std::strcmp(argv[i], "--copy-zone") == 0 && i + 2 < argc) {
            // Duplicate a zone — copy every file then rename slug-prefixed
            // ones (heightmap/terrain/collision sidecars carry the slug in
            // their filenames, e.g. "Sample_28_30.whm") so the new zone is
            // self-consistent. Useful for templating: scaffold once, then
            // copy-zone N times to create variants.
            std::string srcDir = argv[++i];
            std::string rawName = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(srcDir) || !fs::is_directory(srcDir)) {
                std::fprintf(stderr, "copy-zone: source dir not found: %s\n",
                             srcDir.c_str());
                return 1;
            }
            if (!fs::exists(srcDir + "/zone.json")) {
                std::fprintf(stderr, "copy-zone: %s has no zone.json — not a zone dir\n",
                             srcDir.c_str());
                return 1;
            }
            // Slugify new name (matches scaffold-zone rules so the result
            // round-trips through unpackZone / server module gen).
            std::string newSlug;
            for (char c : rawName) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-') {
                    newSlug += c;
                } else if (c == ' ') {
                    newSlug += '_';
                }
            }
            if (newSlug.empty()) {
                std::fprintf(stderr, "copy-zone: name '%s' has no valid characters\n",
                             rawName.c_str());
                return 1;
            }
            std::string dstDir = "custom_zones/" + newSlug;
            if (fs::exists(dstDir)) {
                std::fprintf(stderr, "copy-zone: destination already exists: %s\n",
                             dstDir.c_str());
                return 1;
            }
            // Read the source slug from its zone.json so we know what
            // prefix to rewrite. Don't trust the directory name — a user
            // could have renamed the dir without touching the manifest.
            wowee::editor::ZoneManifest src;
            if (!src.load(srcDir + "/zone.json")) {
                std::fprintf(stderr, "copy-zone: failed to parse %s/zone.json\n",
                             srcDir.c_str());
                return 1;
            }
            std::string oldSlug = src.mapName;
            if (oldSlug == newSlug) {
                std::fprintf(stderr, "copy-zone: new slug matches old (%s); nothing to do\n",
                             oldSlug.c_str());
                return 1;
            }
            // Recursive copy preserves any subdirs (e.g. data/ for DBC sidecars).
            std::error_code ec;
            fs::create_directories(dstDir);
            fs::copy(srcDir, dstDir,
                     fs::copy_options::recursive | fs::copy_options::copy_symlinks,
                     ec);
            if (ec) {
                std::fprintf(stderr, "copy-zone: copy failed: %s\n", ec.message().c_str());
                return 1;
            }
            // Rename slug-prefixed files inside the destination. Match
            // "<oldSlug>_..." or "<oldSlug>." so we catch both
            // "Sample_28_30.whm" and a hypothetical "Sample.wdt".
            int renamed = 0;
            for (const auto& entry : fs::recursive_directory_iterator(dstDir)) {
                if (!entry.is_regular_file()) continue;
                std::string fname = entry.path().filename().string();
                bool match = (fname.size() > oldSlug.size() + 1 &&
                              fname.compare(0, oldSlug.size(), oldSlug) == 0 &&
                              (fname[oldSlug.size()] == '_' ||
                               fname[oldSlug.size()] == '.'));
                if (!match) continue;
                std::string newName = newSlug + fname.substr(oldSlug.size());
                fs::rename(entry.path(), entry.path().parent_path() / newName, ec);
                if (!ec) renamed++;
            }
            // Rewrite the destination's zone.json with the new slug so its
            // files-block (rebuilt from mapName by save()) matches the
            // renamed files on disk.
            wowee::editor::ZoneManifest dst = src;
            dst.mapName = newSlug;
            dst.displayName = rawName;
            if (!dst.save(dstDir + "/zone.json")) {
                std::fprintf(stderr, "copy-zone: failed to write %s/zone.json\n",
                             dstDir.c_str());
                return 1;
            }
            std::printf("Copied %s -> %s\n", srcDir.c_str(), dstDir.c_str());
            std::printf("  mapName  : %s -> %s\n", oldSlug.c_str(), newSlug.c_str());
            std::printf("  renamed  : %d slug-prefixed file(s)\n", renamed);
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
            // Optional --json after the flag for machine-readable output.
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            auto zones = wowee::pipeline::CustomZoneDiscovery::scan({"custom_zones", "output"});
            if (jsonOut) {
                nlohmann::json j = nlohmann::json::array();
                for (const auto& z : zones) {
                    nlohmann::json zoneObj;
                    zoneObj["name"] = z.name;
                    zoneObj["directory"] = z.directory;
                    zoneObj["mapId"] = z.mapId;
                    zoneObj["author"] = z.author;
                    zoneObj["description"] = z.description;
                    zoneObj["hasCreatures"] = z.hasCreatures;
                    zoneObj["hasQuests"] = z.hasQuests;
                    nlohmann::json tiles = nlohmann::json::array();
                    for (const auto& t : z.tiles) tiles.push_back({t.first, t.second});
                    zoneObj["tiles"] = tiles;
                    j.push_back(std::move(zoneObj));
                }
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
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
