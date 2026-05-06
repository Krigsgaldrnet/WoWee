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
#include <fstream>
#include <sstream>
#include "pipeline/wowee_model.hpp"
#include "pipeline/wowee_building.hpp"
#include "pipeline/wowee_collision.hpp"
#include "pipeline/wowee_terrain_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/custom_zone_discovery.hpp"
#include "core/logger.hpp"
#include <string>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "stb_image_write.h"

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
    std::printf("  --convert-dbc-json <dbc-path> [out.json]\n");
    std::printf("                         Convert one DBC file to wowee JSON sidecar format\n");
    std::printf("  --convert-json-dbc <json-path> [out.dbc]\n");
    std::printf("                         Convert a wowee JSON DBC back to binary DBC for private-server compat\n");
    std::printf("  --convert-blp-png <blp-path> [out.png]\n");
    std::printf("                         Convert one BLP texture to PNG sidecar\n");
    std::printf("  --migrate-wom <wom-base> [out-base]\n");
    std::printf("                         Upgrade an older WOM (v1/v2) to WOM3 with a default single-batch entry\n");
    std::printf("  --migrate-zone <zoneDir>\n");
    std::printf("                         Run --migrate-wom in-place on every WOM under <zoneDir>\n");
    std::printf("  --migrate-jsondbc <path> [out.json]\n");
    std::printf("                         Auto-fix a JSON DBC sidecar: add missing format/source, sync recordCount\n");
    std::printf("  --list-zones [--json]  List discovered custom zones and exit\n");
    std::printf("  --zone-stats <projectDir> [--json]\n");
    std::printf("                         Aggregate counts across every zone in <projectDir>\n");
    std::printf("  --list-zone-deps <zoneDir> [--json]\n");
    std::printf("                         List external M2/WMO model paths a zone references (objects + WOB doodads)\n");
    std::printf("  --export-zone-deps-md <zoneDir> [out.md]\n");
    std::printf("                         Markdown dep table for a zone (with on-disk presence column)\n");
    std::printf("  --check-zone-refs <zoneDir> [--json]\n");
    std::printf("                         Verify every referenced model/quest NPC actually exists; exit 1 on missing refs\n");
    std::printf("  --for-each-zone <projectDir> -- <cmd...>\n");
    std::printf("                         Run <cmd...> for every zone in <projectDir>; '{}' in cmd is replaced with the zone path\n");
    std::printf("  --scaffold-zone <name> [tx ty]  Create a blank zone in custom_zones/<name>/ and exit\n");
    std::printf("  --add-tile <zoneDir> <tx> <ty> [baseHeight]\n");
    std::printf("                         Add a new ADT tile to an existing zone (extends the manifest's tiles list)\n");
    std::printf("  --remove-tile <zoneDir> <tx> <ty>\n");
    std::printf("                         Remove a tile from a zone (drops manifest entry + deletes WHM/WOT/WOC files)\n");
    std::printf("  --list-tiles <zoneDir> [--json]\n");
    std::printf("                         List every tile in a zone manifest with on-disk file presence\n");
    std::printf("  --add-creature <zoneDir> <name> <x> <y> <z> [displayId] [level]\n");
    std::printf("                         Append one creature spawn to <zoneDir>/creatures.json and exit\n");
    std::printf("  --add-object <zoneDir> <m2|wmo> <gamePath> <x> <y> <z> [scale]\n");
    std::printf("                         Append one object placement to <zoneDir>/objects.json and exit\n");
    std::printf("  --add-quest <zoneDir> <title> [giverId] [turnInId] [xp] [level]\n");
    std::printf("                         Append one quest to <zoneDir>/quests.json and exit\n");
    std::printf("  --add-quest-objective <zoneDir> <questIdx> <kill|collect|talk|explore|escort|use> <targetName> [count]\n");
    std::printf("                         Append one objective to a quest by index\n");
    std::printf("  --remove-quest-objective <zoneDir> <questIdx> <objIdx>\n");
    std::printf("                         Remove the objective at given 0-based index from a quest\n");
    std::printf("  --clone-quest <zoneDir> <questIdx> [newTitle]\n");
    std::printf("                         Duplicate a quest (with all objectives + rewards) and append it\n");
    std::printf("  --clone-creature <zoneDir> <idx> [newName] [dx dy dz]\n");
    std::printf("                         Duplicate a creature spawn (defaults: '<orig> (copy)' offset by 5 yards)\n");
    std::printf("  --clone-object <zoneDir> <idx> [dx dy dz]\n");
    std::printf("                         Duplicate an object placement (defaults: offset by 5 yards X)\n");
    std::printf("  --add-quest-reward-item <zoneDir> <questIdx> <itemPath> [more...]\n");
    std::printf("                         Append item reward(s) to a quest's reward.itemRewards list\n");
    std::printf("  --set-quest-reward <zoneDir> <questIdx> [--xp N] [--gold N] [--silver N] [--copper N]\n");
    std::printf("                         Update XP/coin reward fields on a quest by index\n");
    std::printf("  --remove-creature <zoneDir> <index>\n");
    std::printf("                         Remove creature at given 0-based index from <zoneDir>/creatures.json\n");
    std::printf("  --remove-object <zoneDir> <index>\n");
    std::printf("                         Remove object at given 0-based index from <zoneDir>/objects.json\n");
    std::printf("  --remove-quest <zoneDir> <index>\n");
    std::printf("                         Remove quest at given 0-based index from <zoneDir>/quests.json\n");
    std::printf("  --copy-zone <srcDir> <newName>\n");
    std::printf("                         Duplicate a zone to custom_zones/<slug>/ with renamed slug-prefixed files\n");
    std::printf("  --rename-zone <srcDir> <newName>\n");
    std::printf("                         In-place rename (zone.json + slug-prefixed files + dir); no copy\n");
    std::printf("  --clear-zone-content <zoneDir> [--creatures] [--objects] [--quests] [--all]\n");
    std::printf("                         Wipe one or more content files (terrain + manifest preserved)\n");
    std::printf("  --build-woc <wot-base> Generate a WOC collision mesh from WHM/WOT and exit\n");
    std::printf("  --regen-collision <zoneDir>  Rebuild every WOC under a zone dir and exit\n");
    std::printf("  --fix-zone <zoneDir>   Re-parse + re-save zone JSONs to apply latest scrubs/caps and exit\n");
    std::printf("  --export-png <wot-base> Render heightmap, normal-map, and zone-map PNG previews\n");
    std::printf("  --export-obj <wom-base> [out.obj]\n");
    std::printf("                         Convert a WOM model to Wavefront OBJ for use in Blender/MeshLab\n");
    std::printf("  --export-glb <wom-base> [out.glb]\n");
    std::printf("                         Convert a WOM model to glTF 2.0 binary (.glb) — modern industry standard\n");
    std::printf("  --export-stl <wom-base> [out.stl]\n");
    std::printf("                         Convert a WOM model to ASCII STL — works with any 3D printer slicer\n");
    std::printf("  --import-stl <stl-path> [wom-base]\n");
    std::printf("                         Convert an ASCII STL back into WOM (round-trips with --export-stl)\n");
    std::printf("  --export-wob-glb <wob-base> [out.glb]\n");
    std::printf("                         Convert a WOB building to glTF 2.0 binary (one mesh, per-group primitives)\n");
    std::printf("  --export-whm-glb <wot-base> [out.glb]\n");
    std::printf("                         Convert WHM heightmap to glTF 2.0 binary terrain mesh (per-chunk primitives)\n");
    std::printf("  --bake-zone-glb <zoneDir> [out.glb]\n");
    std::printf("                         Bake every WHM tile in a zone into one glTF (one node per tile)\n");
    std::printf("  --bake-zone-stl <zoneDir> [out.stl]\n");
    std::printf("                         Bake every WHM tile in a zone into one STL for 3D-printing the terrain\n");
    std::printf("  --bake-zone-obj <zoneDir> [out.obj]\n");
    std::printf("                         Bake every WHM tile in a zone into one Wavefront OBJ (one g-block per tile)\n");
    std::printf("  --import-obj <obj-path> [wom-base]\n");
    std::printf("                         Convert a Wavefront OBJ back into WOM (round-trips with --export-obj)\n");
    std::printf("  --export-wob-obj <wob-base> [out.obj]\n");
    std::printf("                         Convert a WOB building to Wavefront OBJ (one group per WOB group)\n");
    std::printf("  --import-wob-obj <obj-path> [wob-base]\n");
    std::printf("                         Convert a Wavefront OBJ back into WOB (round-trips with --export-wob-obj)\n");
    std::printf("  --export-woc-obj <woc-path> [out.obj]\n");
    std::printf("                         Convert a WOC collision mesh to OBJ for visualization (per-flag color groups)\n");
    std::printf("  --export-whm-obj <wot-base> [out.obj]\n");
    std::printf("                         Convert a WHM heightmap to OBJ terrain mesh (9x9 outer grid per chunk)\n");
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
    std::printf("  --validate-glb <path> [--json]\n");
    std::printf("                         Verify a glTF 2.0 binary's structure (magic, chunks, JSON, accessors)\n");
    std::printf("  --check-glb-bounds <path> [--json]\n");
    std::printf("                         Verify position accessor min/max in a .glb actually matches the data\n");
    std::printf("  --validate-stl <path> [--json]\n");
    std::printf("                         Verify an ASCII STL's structure (solid framing, facet/vertex shape, no NaN)\n");
    std::printf("  --validate-png <path> [--json]\n");
    std::printf("                         Verify a PNG's structure (signature, chunks, CRC, IHDR/IDAT/IEND order)\n");
    std::printf("  --validate-jsondbc <path> [--json]\n");
    std::printf("                         Verify a JSON DBC sidecar's full schema (per-cell types, row width, format tag)\n");
    std::printf("  --info-glb <path> [--json]\n");
    std::printf("                         Print glTF 2.0 binary metadata (chunks, mesh/primitive counts, accessors)\n");
    std::printf("  --info-glb-tree <path>\n");
    std::printf("                         Render glTF structure as a tree (scenes/nodes/meshes/primitives)\n");
    std::printf("  --zone-summary <zoneDir> [--json]\n");
    std::printf("                         One-shot validate + creature/object/quest counts and exit\n");
    std::printf("  --info-zone-tree <zoneDir>\n");
    std::printf("                         Render a hierarchical tree view of a zone's contents (no --json)\n");
    std::printf("  --export-zone-summary-md <zoneDir> [out.md]\n");
    std::printf("                         Render a markdown documentation page for a zone (manifest + content)\n");
    std::printf("  --export-zone-csv <zoneDir> [outDir]\n");
    std::printf("                         Emit creatures.csv / objects.csv / quests.csv for spreadsheet workflows\n");
    std::printf("  --export-zone-html <zoneDir> [out.html]\n");
    std::printf("                         Emit a single-file HTML viewer next to the zone .glb (model-viewer based)\n");
    std::printf("  --export-quest-graph <zoneDir> [out.dot]\n");
    std::printf("                         Render quest-chain DAG as Graphviz DOT (pipe to `dot -Tpng -o quests.png`)\n");
    std::printf("  --info <wom-base> [--json]\n");
    std::printf("                         Print WOM file metadata (version, counts) and exit\n");
    std::printf("  --info-batches <wom-base> [--json]\n");
    std::printf("                         Per-batch breakdown of a WOM3 (index range, texture, blend mode, flags)\n");
    std::printf("  --info-textures <wom-base> [--json]\n");
    std::printf("                         List every texture path referenced by a WOM (with on-disk presence)\n");
    std::printf("  --info-doodads <wob-base> [--json]\n");
    std::printf("                         List every doodad placement in a WOB (model path, position, rotation, scale)\n");
    std::printf("  --info-attachments <m2-path> [--json]\n");
    std::printf("                         List M2 attachment points (weapon mounts, etc.) with bone + offset\n");
    std::printf("  --info-particles <m2-path> [--json]\n");
    std::printf("                         List M2 particle + ribbon emitters (texture, blend, bone)\n");
    std::printf("  --info-sequences <m2-path> [--json]\n");
    std::printf("                         List M2 animation sequences (id, duration, flags)\n");
    std::printf("  --info-bones <m2-path> [--json]\n");
    std::printf("                         List M2 bones with parent tree, key-bone IDs, pivot offsets\n");
    std::printf("  --list-zone-textures <zoneDir> [--json]\n");
    std::printf("                         Aggregate texture refs across all WOM models in a zone (deduped)\n");
    std::printf("  --info-wob <wob-base> [--json]\n");
    std::printf("                         Print WOB building metadata (groups, portals, doodads) and exit\n");
    std::printf("  --info-woc <woc-path> [--json]\n");
    std::printf("                         Print WOC collision metadata (triangle counts, bounds) and exit\n");
    std::printf("  --info-wot <wot-base> [--json]\n");
    std::printf("                         Print WOT/WHM terrain metadata (tile, chunks, height range) and exit\n");
    std::printf("  --info-extract <dir> [--json]\n");
    std::printf("                         Walk extracted asset tree and report open-format coverage and exit\n");
    std::printf("  --info-png <path> [--json]\n");
    std::printf("                         Print PNG header (width, height, channels, bit depth) and exit\n");
    std::printf("  --info-blp <path> [--json]\n");
    std::printf("                         Print BLP texture header (format, compression, mips, dimensions) and exit\n");
    std::printf("  --info-m2 <path> [--json]\n");
    std::printf("                         Print proprietary M2 model metadata (verts, bones, anims, particles)\n");
    std::printf("  --info-wmo <path> [--json]\n");
    std::printf("                         Print proprietary WMO building metadata (groups, portals, doodads)\n");
    std::printf("  --info-adt <path> [--json]\n");
    std::printf("                         Print proprietary ADT terrain metadata (chunks, placements, textures)\n");
    std::printf("  --info-jsondbc <path> [--json]\n");
    std::printf("                         Print JSON DBC sidecar metadata (records, fields, source) and exit\n");
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
    std::printf("  --list-creatures <p> [--json]\n");
    std::printf("                         List every creature with index, name, position, level (for --remove-creature)\n");
    std::printf("  --list-objects <p> [--json]\n");
    std::printf("                         List every object with index, type, path, position\n");
    std::printf("  --list-quests <p> [--json]\n");
    std::printf("                         List every quest with index, title, giver, XP\n");
    std::printf("  --list-quest-objectives <p> <questIdx> [--json]\n");
    std::printf("                         List every objective on a quest (for --remove-quest-objective)\n");
    std::printf("  --list-quest-rewards <p> <questIdx> [--json]\n");
    std::printf("                         List XP/coin/item rewards on a quest\n");
    std::printf("  --info-creature <p> <idx> [--json]\n");
    std::printf("                         Print every field for one creature spawn (stats, behavior, AI, flags)\n");
    std::printf("  --info-quest <p> <idx> [--json]\n");
    std::printf("                         Print every field for one quest (objectives + reward + chain in one shot)\n");
    std::printf("  --info-object <p> <idx> [--json]\n");
    std::printf("                         Print every field for one object placement (type, path, transform)\n");
    std::printf("  --info-wcp <wcp-path> [--json]\n");
    std::printf("                         Print WCP archive metadata (name, files) and exit\n");
    std::printf("  --list-wcp <wcp-path>  Print every file inside a WCP archive (sorted by path) and exit\n");
    std::printf("  --diff-wcp <a> <b> [--json]\n");
    std::printf("                         Compare two WCPs file-by-file; exit 0 if identical, 1 otherwise\n");
    std::printf("  --diff-zone <a> <b> [--json]\n");
    std::printf("                         Compare two zone dirs (creatures/objects/quests/manifest); exit 0 if identical\n");
    std::printf("  --diff-glb <a> <b> [--json]\n");
    std::printf("                         Compare two glTF 2.0 binaries structurally; exit 0 if identical\n");
    std::printf("  --diff-wom <a-base> <b-base> [--json]\n");
    std::printf("                         Compare two WOM models (verts, indices, bones, anims, batches, bounds)\n");
    std::printf("  --diff-wob <a-base> <b-base> [--json]\n");
    std::printf("                         Compare two WOB buildings (groups, portals, doodads, totals)\n");
    std::printf("  --diff-whm <a-base> <b-base> [--json]\n");
    std::printf("                         Compare two WHM/WOT terrain pairs (chunks, height range, placements)\n");
    std::printf("  --diff-woc <a> <b> [--json]\n");
    std::printf("                         Compare two WOC collision meshes (triangles, walkable/steep counts, tile)\n");
    std::printf("  --diff-jsondbc <a> <b> [--json]\n");
    std::printf("                         Compare two JSON DBC sidecars (format/source/recordCount/fieldCount)\n");
    std::printf("  --pack-wcp <zone> [dst]   Pack a zone dir/name into a .wcp archive and exit\n");
    std::printf("  --unpack-wcp <wcp> [dst]  Extract a WCP archive (default dst=custom_zones/) and exit\n");
    std::printf("  --list-commands        Print every recognized --flag, one per line, and exit\n");
    std::printf("  --gen-completion <bash|zsh>\n");
    std::printf("                         Print a shell-completion script for wowee_editor (source it from your rc file)\n");
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
        "--data", "--info", "--info-batches", "--info-textures", "--info-doodads",
        "--info-attachments", "--info-particles", "--info-sequences",
        "--info-bones", "--list-zone-textures",
        "--info-wob", "--info-woc", "--info-wot",
        "--info-creatures", "--info-objects", "--info-quests",
        "--info-extract", "--list-missing-sidecars",
        "--info-png", "--info-jsondbc", "--info-blp",
        "--info-m2", "--info-wmo", "--info-adt",
        "--info-zone", "--info-wcp", "--list-wcp",
        "--list-creatures", "--list-objects", "--list-quests",
        "--list-quest-objectives", "--list-quest-rewards",
        "--info-creature", "--info-quest", "--info-object",
        "--unpack-wcp", "--pack-wcp",
        "--validate", "--validate-wom", "--validate-wob", "--validate-woc",
        "--validate-whm", "--validate-all", "--validate-glb", "--info-glb",
        "--info-glb-tree",
        "--validate-jsondbc", "--check-glb-bounds", "--validate-stl",
        "--validate-png",
        "--zone-summary", "--info-zone-tree",
        "--export-zone-summary-md", "--export-quest-graph",
        "--export-zone-csv", "--export-zone-html",
        "--scaffold-zone", "--add-tile", "--remove-tile", "--list-tiles",
        "--for-each-zone", "--zone-stats", "--list-zone-deps",
        "--check-zone-refs", "--export-zone-deps-md",
        "--add-creature", "--add-object", "--add-quest",
        "--add-quest-objective", "--add-quest-reward-item", "--set-quest-reward",
        "--remove-quest-objective", "--clone-quest", "--clone-creature",
        "--clone-object",
        "--remove-creature", "--remove-object", "--remove-quest",
        "--copy-zone", "--rename-zone", "--clear-zone-content",
        "--build-woc", "--regen-collision", "--fix-zone",
        "--export-png", "--export-obj", "--import-obj",
        "--export-wob-obj", "--import-wob-obj",
        "--export-woc-obj", "--export-whm-obj",
        "--export-glb", "--export-wob-glb", "--export-whm-glb",
        "--export-stl", "--import-stl",
        "--bake-zone-glb", "--bake-zone-stl", "--bake-zone-obj",
        "--convert-m2", "--convert-wmo",
        "--convert-dbc-json", "--convert-json-dbc", "--convert-blp-png",
        "--migrate-wom", "--migrate-zone", "--migrate-jsondbc",
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
        if (std::strcmp(argv[i], "--diff-zone") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-zone requires <zoneA> <zoneB>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-glb") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-glb requires <a.glb> <b.glb>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-wom") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-wom requires <a-base> <b-base>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-wob") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-wob requires <a-base> <b-base>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-whm") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-whm requires <a-base> <b-base>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-woc") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-woc requires <a.woc> <b.woc>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-jsondbc") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-jsondbc requires <a.json> <b.json>\n");
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
        if (std::strcmp(argv[i], "--add-quest-objective") == 0 && i + 4 >= argc) {
            std::fprintf(stderr,
                "--add-quest-objective requires <zoneDir> <questIdx> <type> <targetName>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--remove-quest-objective") == 0 && i + 3 >= argc) {
            std::fprintf(stderr,
                "--remove-quest-objective requires <zoneDir> <questIdx> <objIdx>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--clone-quest") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--clone-quest requires <zoneDir> <questIdx>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--clone-creature") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--clone-creature requires <zoneDir> <idx>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--clone-object") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--clone-object requires <zoneDir> <idx>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--add-quest-reward-item") == 0 && i + 3 >= argc) {
            std::fprintf(stderr,
                "--add-quest-reward-item requires <zoneDir> <questIdx> <itemPath>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--set-quest-reward") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--set-quest-reward requires <zoneDir> <questIdx> [--xp N] [--gold N] [--silver N] [--copper N]\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--add-tile") == 0 && i + 3 >= argc) {
            std::fprintf(stderr,
                "--add-tile requires <zoneDir> <tx> <ty>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--remove-tile") == 0 && i + 3 >= argc) {
            std::fprintf(stderr,
                "--remove-tile requires <zoneDir> <tx> <ty>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--copy-zone") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--copy-zone requires <srcDir> <newName>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--rename-zone") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--rename-zone requires <srcDir> <newName>\n");
            return 1;
        }
        for (const char* opt : {"--remove-creature", "--remove-object",
                                 "--remove-quest"}) {
            if (std::strcmp(argv[i], opt) == 0 && i + 2 >= argc) {
                std::fprintf(stderr, "%s requires <zoneDir> <index>\n", opt);
                return 1;
            }
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
        } else if (std::strcmp(argv[i], "--info-batches") == 0 && i + 1 < argc) {
            // Per-batch breakdown of a WOM3 (multi-material) model.
            // --info shows the total batch count; this drills into each
            // one's index range, texture, blend mode, and flags. Useful
            // for debugging 'why is this submesh transparent?' or
            // 'which batch has the bad UV?'.
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
            // Blend modes per WoweeModel::Batch comment:
            //   0=opaque, 1=alpha-test, 2=alpha, 3=add
            auto blendName = [](uint16_t b) {
                switch (b) {
                    case 0: return "opaque";
                    case 1: return "alpha-test";
                    case 2: return "alpha";
                    case 3: return "add";
                }
                return "?";
            };
            // Flags bits:
            //   bit 0 (0x01) = unlit
            //   bit 1 (0x02) = two-sided
            //   bit 2 (0x04) = no z-write
            auto flagsStr = [](uint16_t f) {
                std::string s;
                if (f & 0x01) s += "unlit ";
                if (f & 0x02) s += "two-sided ";
                if (f & 0x04) s += "no-zwrite ";
                if (s.empty()) s = "-";
                else s.pop_back();  // drop trailing space
                return s;
            };
            if (jsonOut) {
                nlohmann::json j;
                j["wom"] = base + ".wom";
                j["version"] = wom.version;
                j["totalBatches"] = wom.batches.size();
                nlohmann::json arr = nlohmann::json::array();
                for (size_t k = 0; k < wom.batches.size(); ++k) {
                    const auto& b = wom.batches[k];
                    std::string tex = (b.textureIndex < wom.texturePaths.size())
                                       ? wom.texturePaths[b.textureIndex]
                                       : std::string("<oob>");
                    arr.push_back({
                        {"index", k},
                        {"indexStart", b.indexStart},
                        {"indexCount", b.indexCount},
                        {"triangles", b.indexCount / 3},
                        {"textureIndex", b.textureIndex},
                        {"texturePath", tex},
                        {"blendMode", b.blendMode},
                        {"blendName", blendName(b.blendMode)},
                        {"flags", b.flags},
                        {"flagsStr", flagsStr(b.flags)},
                    });
                }
                j["batches"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("WOM batches: %s.wom (v%u, %zu batches)\n",
                        base.c_str(), wom.version, wom.batches.size());
            if (wom.batches.empty()) {
                std::printf("  *no batches (WOM1/WOM2 single-material model)*\n");
                return 0;
            }
            std::printf("  idx  iStart  iCount  tris   blend       flags          texture\n");
            for (size_t k = 0; k < wom.batches.size(); ++k) {
                const auto& b = wom.batches[k];
                std::string tex = (b.textureIndex < wom.texturePaths.size())
                                   ? wom.texturePaths[b.textureIndex]
                                   : std::string("<oob>");
                std::printf("  %3zu  %6u  %6u  %5u  %-10s  %-13s  %s\n",
                            k, b.indexStart, b.indexCount, b.indexCount / 3,
                            blendName(b.blendMode),
                            flagsStr(b.flags).c_str(),
                            tex.c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-textures") == 0 && i + 1 < argc) {
            // List every texture path a WOM references, with on-disk
            // presence for both BLP (proprietary) and PNG (sidecar)
            // forms. Useful for tracking which textures are missing
            // before --pack-wcp would fail at runtime.
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
            namespace fs = std::filesystem;
            // Texture paths in WOMs are usually game-relative
            // ('World/Generic/Tree.blp'); resolve them against the
            // common Data/ root for the on-disk presence check. Skip
            // the check when the path doesn't exist as either an
            // absolute or relative file (avoids false 'missing'
            // reports when the user runs from outside the data root).
            auto checkBlp = [&](const std::string& p) {
                if (fs::exists(p)) return true;
                std::string lower = p;
                for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
                if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".blp") {
                    lower += ".blp";
                }
                return fs::exists("Data/" + lower);
            };
            auto sidecarPng = [&](const std::string& p) {
                std::string base = p;
                if (base.size() >= 4 &&
                    (base.substr(base.size() - 4) == ".blp" ||
                     base.substr(base.size() - 4) == ".BLP")) {
                    base = base.substr(0, base.size() - 4);
                }
                std::string png = base + ".png";
                if (fs::exists(png)) return true;
                std::string lower = png;
                for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
                return fs::exists("Data/" + lower);
            };
            if (jsonOut) {
                nlohmann::json j;
                j["wom"] = base + ".wom";
                j["textureCount"] = wom.texturePaths.size();
                nlohmann::json arr = nlohmann::json::array();
                for (size_t k = 0; k < wom.texturePaths.size(); ++k) {
                    const auto& p = wom.texturePaths[k];
                    arr.push_back({
                        {"index", k},
                        {"path", p},
                        {"blpPresent", checkBlp(p)},
                        {"pngPresent", sidecarPng(p)},
                    });
                }
                j["textures"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("WOM textures: %s.wom (%zu textures)\n",
                        base.c_str(), wom.texturePaths.size());
            if (wom.texturePaths.empty()) {
                std::printf("  *no texture references*\n");
                return 0;
            }
            std::printf("  idx  blp  png  path\n");
            for (size_t k = 0; k < wom.texturePaths.size(); ++k) {
                const auto& p = wom.texturePaths[k];
                std::printf("  %3zu   %s    %s   %s\n",
                            k,
                            checkBlp(p) ? "y" : "-",
                            sidecarPng(p) ? "y" : "-",
                            p.c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-doodads") == 0 && i + 1 < argc) {
            // List every doodad placement in a WOB (M2 instances inside
            // a building). Companion to --info-textures: where one
            // tracks GPU resources, this tracks scene composition.
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
            if (jsonOut) {
                nlohmann::json j;
                j["wob"] = base + ".wob";
                j["count"] = bld.doodads.size();
                nlohmann::json arr = nlohmann::json::array();
                for (size_t k = 0; k < bld.doodads.size(); ++k) {
                    const auto& d = bld.doodads[k];
                    arr.push_back({
                        {"index", k},
                        {"modelPath", d.modelPath},
                        {"position", {d.position.x, d.position.y, d.position.z}},
                        {"rotation", {d.rotation.x, d.rotation.y, d.rotation.z}},
                        {"scale", d.scale},
                    });
                }
                j["doodads"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("WOB doodads: %s.wob (%zu placements)\n",
                        base.c_str(), bld.doodads.size());
            if (bld.doodads.empty()) {
                std::printf("  *no doodad placements*\n");
                return 0;
            }
            std::printf("  idx  scale  pos (x, y, z)             rot (x, y, z)             model\n");
            for (size_t k = 0; k < bld.doodads.size(); ++k) {
                const auto& d = bld.doodads[k];
                std::printf("  %3zu  %5.2f  (%6.1f, %6.1f, %6.1f)  (%6.1f, %6.1f, %6.1f)  %s\n",
                            k, d.scale,
                            d.position.x, d.position.y, d.position.z,
                            d.rotation.x, d.rotation.y, d.rotation.z,
                            d.modelPath.c_str());
            }
            return 0;
        } else if ((std::strcmp(argv[i], "--info-attachments") == 0 ||
                    std::strcmp(argv[i], "--info-particles") == 0 ||
                    std::strcmp(argv[i], "--info-sequences") == 0) &&
                   i + 1 < argc) {
            // Three M2 inspectors share an entry point — they all need
            // the same M2Loader::load + skin merge dance, then differ
            // only in which sub-array they iterate.
            enum Kind { kAttach, kParticle, kSequence };
            Kind kind;
            const char* cmdName;
            if (std::strcmp(argv[i], "--info-attachments") == 0) {
                kind = kAttach; cmdName = "info-attachments";
            } else if (std::strcmp(argv[i], "--info-particles") == 0) {
                kind = kParticle; cmdName = "info-particles";
            } else {
                kind = kSequence; cmdName = "info-sequences";
            }
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "%s: cannot open %s\n", cmdName, path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            // Auto-merge skin for vertex/index counts to match render.
            std::vector<uint8_t> skinBytes;
            {
                std::string skinPath = path;
                auto dot = skinPath.rfind('.');
                if (dot != std::string::npos)
                    skinPath = skinPath.substr(0, dot) + "00.skin";
                std::ifstream sf(skinPath, std::ios::binary);
                if (sf) {
                    skinBytes.assign((std::istreambuf_iterator<char>(sf)),
                                      std::istreambuf_iterator<char>());
                }
            }
            auto m2 = wowee::pipeline::M2Loader::load(bytes);
            if (!skinBytes.empty()) {
                wowee::pipeline::M2Loader::loadSkin(skinBytes, m2);
            }
            if (kind == kAttach) {
                if (jsonOut) {
                    nlohmann::json j;
                    j["m2"] = path;
                    j["count"] = m2.attachments.size();
                    nlohmann::json arr = nlohmann::json::array();
                    for (size_t k = 0; k < m2.attachments.size(); ++k) {
                        const auto& a = m2.attachments[k];
                        arr.push_back({
                            {"index", k}, {"id", a.id}, {"bone", a.bone},
                            {"position", {a.position.x, a.position.y, a.position.z}}
                        });
                    }
                    j["attachments"] = arr;
                    std::printf("%s\n", j.dump(2).c_str());
                    return 0;
                }
                std::printf("M2 attachments: %s (%zu)\n", path.c_str(),
                            m2.attachments.size());
                if (m2.attachments.empty()) {
                    std::printf("  *no attachments*\n");
                    return 0;
                }
                std::printf("  idx   id  bone  pos (x, y, z)\n");
                for (size_t k = 0; k < m2.attachments.size(); ++k) {
                    const auto& a = m2.attachments[k];
                    std::printf("  %3zu  %3u  %4u  (%6.2f, %6.2f, %6.2f)\n",
                                k, a.id, a.bone,
                                a.position.x, a.position.y, a.position.z);
                }
                return 0;
            }
            if (kind == kParticle) {
                auto blendName = [](uint8_t b) {
                    switch (b) {
                        case 0: return "opaque";
                        case 1: return "alphakey";
                        case 2: return "alpha";
                        case 4: return "add";
                    }
                    return "?";
                };
                if (jsonOut) {
                    nlohmann::json j;
                    j["m2"] = path;
                    j["particleEmitters"] = m2.particleEmitters.size();
                    j["ribbonEmitters"] = m2.ribbonEmitters.size();
                    nlohmann::json parts = nlohmann::json::array();
                    for (size_t k = 0; k < m2.particleEmitters.size(); ++k) {
                        const auto& p = m2.particleEmitters[k];
                        parts.push_back({
                            {"index", k}, {"particleId", p.particleId},
                            {"bone", p.bone}, {"texture", p.texture},
                            {"blendingType", p.blendingType},
                            {"blendName", blendName(p.blendingType)},
                            {"emitterType", p.emitterType},
                            {"position", {p.position.x, p.position.y, p.position.z}}
                        });
                    }
                    j["particles"] = parts;
                    nlohmann::json ribbons = nlohmann::json::array();
                    for (size_t k = 0; k < m2.ribbonEmitters.size(); ++k) {
                        const auto& r = m2.ribbonEmitters[k];
                        ribbons.push_back({
                            {"index", k}, {"ribbonId", r.ribbonId},
                            {"bone", r.bone},
                            {"textureIndex", r.textureIndex},
                            {"materialIndex", r.materialIndex},
                            {"position", {r.position.x, r.position.y, r.position.z}}
                        });
                    }
                    j["ribbons"] = ribbons;
                    std::printf("%s\n", j.dump(2).c_str());
                    return 0;
                }
                std::printf("M2 emitters: %s\n", path.c_str());
                std::printf("  particles: %zu, ribbons: %zu\n",
                            m2.particleEmitters.size(), m2.ribbonEmitters.size());
                if (!m2.particleEmitters.empty()) {
                    std::printf("\n  Particles:\n");
                    std::printf("    idx   id  bone  tex  blend     type  pos (x, y, z)\n");
                    for (size_t k = 0; k < m2.particleEmitters.size(); ++k) {
                        const auto& p = m2.particleEmitters[k];
                        std::printf("    %3zu  %3d  %4u  %3u  %-8s  %4u  (%5.1f, %5.1f, %5.1f)\n",
                                    k, p.particleId, p.bone, p.texture,
                                    blendName(p.blendingType), p.emitterType,
                                    p.position.x, p.position.y, p.position.z);
                    }
                }
                if (!m2.ribbonEmitters.empty()) {
                    std::printf("\n  Ribbons:\n");
                    std::printf("    idx   id  bone  tex  mat  pos (x, y, z)\n");
                    for (size_t k = 0; k < m2.ribbonEmitters.size(); ++k) {
                        const auto& r = m2.ribbonEmitters[k];
                        std::printf("    %3zu  %3d  %4u  %3u  %3u  (%5.1f, %5.1f, %5.1f)\n",
                                    k, r.ribbonId, r.bone, r.textureIndex, r.materialIndex,
                                    r.position.x, r.position.y, r.position.z);
                    }
                }
                return 0;
            }
            // kind == kSequence
            if (jsonOut) {
                nlohmann::json j;
                j["m2"] = path;
                j["count"] = m2.sequences.size();
                nlohmann::json arr = nlohmann::json::array();
                for (size_t k = 0; k < m2.sequences.size(); ++k) {
                    const auto& s = m2.sequences[k];
                    arr.push_back({
                        {"index", k}, {"id", s.id},
                        {"variation", s.variationIndex},
                        {"durationMs", s.duration}, {"flags", s.flags},
                        {"movingSpeed", s.movingSpeed},
                        {"frequency", s.frequency},
                        {"blendTimeMs", s.blendTime}
                    });
                }
                j["sequences"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("M2 sequences: %s (%zu)\n", path.c_str(),
                        m2.sequences.size());
            if (m2.sequences.empty()) {
                std::printf("  *no sequences*\n");
                return 0;
            }
            std::printf("  idx   id  var  duration  flags    speed  blend\n");
            for (size_t k = 0; k < m2.sequences.size(); ++k) {
                const auto& s = m2.sequences[k];
                std::printf("  %3zu  %3u  %3u  %8u  %5u   %5.2f  %5u\n",
                            k, s.id, s.variationIndex,
                            s.duration, s.flags,
                            s.movingSpeed, s.blendTime);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-bones") == 0 && i + 1 < argc) {
            // Inspect M2 bone tree. Shows parent index, key-bone ID
            // (-1 if not a named bone), pivot offset, and a depth
            // indicator computed by walking up parents — useful for
            // debugging skeleton structure when something looks wrong
            // in the renderer ('why is this bone not following its parent?').
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "info-bones: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            auto m2 = wowee::pipeline::M2Loader::load(bytes);
            // Compute depth per bone — guard against cycles by capping
            // walk length at boneCount (a real DAG can't exceed that).
            std::vector<int> depths(m2.bones.size(), -1);
            for (size_t k = 0; k < m2.bones.size(); ++k) {
                int d = 0;
                int idx = static_cast<int>(k);
                while (idx >= 0 && d <= static_cast<int>(m2.bones.size())) {
                    int parent = m2.bones[idx].parentBone;
                    if (parent < 0) break;
                    idx = parent;
                    d++;
                }
                depths[k] = d;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["m2"] = path;
                j["count"] = m2.bones.size();
                nlohmann::json arr = nlohmann::json::array();
                for (size_t k = 0; k < m2.bones.size(); ++k) {
                    const auto& b = m2.bones[k];
                    arr.push_back({
                        {"index", k}, {"keyBoneId", b.keyBoneId},
                        {"parent", b.parentBone}, {"flags", b.flags},
                        {"depth", depths[k]},
                        {"pivot", {b.pivot.x, b.pivot.y, b.pivot.z}}
                    });
                }
                j["bones"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("M2 bones: %s (%zu)\n", path.c_str(), m2.bones.size());
            if (m2.bones.empty()) {
                std::printf("  *no bones (static model)*\n");
                return 0;
            }
            std::printf("  idx  parent  depth  keyBone  flags    pivot (x, y, z)\n");
            for (size_t k = 0; k < m2.bones.size(); ++k) {
                const auto& b = m2.bones[k];
                // Indent the keyBone column by depth so the tree shape
                // is visible at a glance.
                std::printf("  %3zu  %6d  %5d  %7d  %5u    (%6.2f, %6.2f, %6.2f)\n",
                            k, b.parentBone, depths[k], b.keyBoneId, b.flags,
                            b.pivot.x, b.pivot.y, b.pivot.z);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--list-zone-textures") == 0 && i + 1 < argc) {
            // Aggregate texture references across every WOM model in a
            // zone directory. Companion to --list-zone-deps (which lists
            // model paths) — this lists the textures those models pull in.
            // Useful for verifying every BLP/PNG ships with the zone.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "list-zone-textures: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            std::map<std::string, int> texHist;  // path -> count of WOMs that ref it
            int womCount = 0;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file()) continue;
                std::string ext = e.path().extension().string();
                if (ext != ".wom") continue;
                womCount++;
                std::string base = e.path().string();
                if (base.size() >= 4) base = base.substr(0, base.size() - 4);
                auto wom = wowee::pipeline::WoweeModelLoader::load(base);
                std::unordered_set<std::string> seenInThisWom;
                for (const auto& tp : wom.texturePaths) {
                    if (tp.empty()) continue;
                    if (seenInThisWom.insert(tp).second) {
                        texHist[tp]++;
                    }
                }
            }
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["womCount"] = womCount;
                j["uniqueTextures"] = texHist.size();
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& [path, count] : texHist) {
                    arr.push_back({{"path", path}, {"refCount", count}});
                }
                j["textures"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone textures: %s\n", zoneDir.c_str());
            std::printf("  WOMs scanned    : %d\n", womCount);
            std::printf("  unique textures : %zu\n", texHist.size());
            if (texHist.empty()) {
                std::printf("  *no texture references*\n");
                return 0;
            }
            std::printf("\n  refs  path\n");
            for (const auto& [path, count] : texHist) {
                std::printf("  %4d  %s\n", count, path.c_str());
            }
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
        } else if (std::strcmp(argv[i], "--info-png") == 0 && i + 1 < argc) {
            // Inspect a PNG sidecar — width, height, channels, bit depth.
            // Reads only the IHDR chunk (16 bytes after the 8-byte
            // signature) so it works on huge files instantly without
            // decoding pixels. Useful for verifying that the BLP→PNG
            // emitter produced the expected dimensions.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "info-png: cannot open %s\n", path.c_str());
                return 1;
            }
            uint8_t buf[24];
            in.read(reinterpret_cast<char*>(buf), 24);
            if (!in || in.gcount() < 24) {
                std::fprintf(stderr, "info-png: %s too short to be a PNG\n", path.c_str());
                return 1;
            }
            // Validate the 8-byte PNG signature: 89 50 4E 47 0D 0A 1A 0A
            static const uint8_t kSig[8] = {0x89, 0x50, 0x4E, 0x47,
                                             0x0D, 0x0A, 0x1A, 0x0A};
            if (std::memcmp(buf, kSig, 8) != 0) {
                std::fprintf(stderr, "info-png: %s missing PNG signature\n", path.c_str());
                return 1;
            }
            // IHDR chunk follows: 4-byte length, 4-byte type ('IHDR'),
            // then 13-byte payload (width:4, height:4, bitDepth:1,
            // colorType:1, compression:1, filter:1, interlace:1).
            // All multi-byte ints in PNG are big-endian.
            auto be32 = [](const uint8_t* p) {
                return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                       (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
            };
            uint32_t width  = be32(buf + 16);
            uint32_t height = be32(buf + 20);
            // Need bit depth + color type — read the next 5 bytes.
            uint8_t extra[5];
            in.read(reinterpret_cast<char*>(extra), 5);
            uint8_t bitDepth  = extra[0];
            uint8_t colorType = extra[1];
            // Channel count derives from color type (PNG spec table 11.1).
            int channels = 0;
            const char* colorName = "?";
            switch (colorType) {
                case 0: channels = 1; colorName = "grayscale"; break;
                case 2: channels = 3; colorName = "rgb"; break;
                case 3: channels = 1; colorName = "palette"; break;
                case 4: channels = 2; colorName = "grayscale+alpha"; break;
                case 6: channels = 4; colorName = "rgba"; break;
            }
            // File size for a quick sanity check — a 1024x1024 RGBA PNG
            // shouldn't be 12 bytes, that would mean truncation.
            std::error_code ec;
            uint64_t fsz = std::filesystem::file_size(path, ec);
            if (jsonOut) {
                nlohmann::json j;
                j["png"] = path;
                j["width"] = width;
                j["height"] = height;
                j["bitDepth"] = bitDepth;
                j["channels"] = channels;
                j["colorType"] = colorType;
                j["colorTypeName"] = colorName;
                j["fileSize"] = fsz;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("PNG: %s\n", path.c_str());
            std::printf("  size      : %u x %u\n", width, height);
            std::printf("  bit depth : %u\n", bitDepth);
            std::printf("  color     : %s (%d channel%s)\n",
                        colorName, channels, channels == 1 ? "" : "s");
            std::printf("  file bytes: %llu\n", static_cast<unsigned long long>(fsz));
            return 0;
        } else if (std::strcmp(argv[i], "--info-blp") == 0 && i + 1 < argc) {
            // Inspect a BLP texture: format/compression/mips/dimensions.
            // Loads the full image (which decompresses pixels) since we
            // also report channel count and decoded byte size — useful
            // for verifying the source before --convert-blp-png.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "info-blp: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            // Quick magic check before full decode — saves a confusing
            // 'invalid' from the loader when the user feeds a non-BLP.
            if (bytes.size() < 4 ||
                !(bytes[0] == 'B' && bytes[1] == 'L' && bytes[2] == 'P' &&
                  (bytes[3] == '1' || bytes[3] == '2'))) {
                std::fprintf(stderr, "info-blp: %s is not a BLP1/BLP2 file\n",
                             path.c_str());
                return 1;
            }
            std::string magicVer = std::string(bytes.begin(), bytes.begin() + 4);
            auto img = wowee::pipeline::BLPLoader::load(bytes);
            if (!img.isValid()) {
                std::fprintf(stderr, "info-blp: failed to decode %s\n", path.c_str());
                return 1;
            }
            std::error_code ec;
            uint64_t fsz = std::filesystem::file_size(path, ec);
            const char* fmtName = wowee::pipeline::BLPLoader::getFormatName(img.format);
            const char* compName = wowee::pipeline::BLPLoader::getCompressionName(img.compression);
            if (jsonOut) {
                nlohmann::json j;
                j["blp"] = path;
                j["magic"] = magicVer;
                j["width"] = img.width;
                j["height"] = img.height;
                j["channels"] = img.channels;
                j["mipLevels"] = img.mipLevels;
                j["format"] = fmtName;
                j["compression"] = compName;
                j["decodedBytes"] = img.data.size();
                j["fileSize"] = fsz;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("BLP: %s (%s)\n", path.c_str(), magicVer.c_str());
            std::printf("  size       : %d x %d\n", img.width, img.height);
            std::printf("  channels   : %d\n", img.channels);
            std::printf("  format     : %s\n", fmtName);
            std::printf("  compression: %s\n", compName);
            std::printf("  mip levels : %d\n", img.mipLevels);
            std::printf("  file bytes : %llu\n", static_cast<unsigned long long>(fsz));
            std::printf("  decoded RGBA bytes: %zu\n", img.data.size());
            return 0;
        } else if (std::strcmp(argv[i], "--info-m2") == 0 && i + 1 < argc) {
            // Inspect a proprietary M2 model. Pairs with --info to inspect
            // the WOM equivalent, so users can see what was preserved/lost
            // by the M2 -> WOM conversion (e.g. M2 has particles + ribbons,
            // WOM doesn't yet).
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "info-m2: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            // Auto-merge matching <base>00.skin if present (WotLK+ models
            // store geometry there) so vertex/index counts match what
            // gets rendered.
            std::vector<uint8_t> skinBytes;
            {
                std::string skinPath = path;
                auto dot = skinPath.rfind('.');
                if (dot != std::string::npos)
                    skinPath = skinPath.substr(0, dot) + "00.skin";
                std::ifstream sf(skinPath, std::ios::binary);
                if (sf) {
                    skinBytes.assign((std::istreambuf_iterator<char>(sf)),
                                      std::istreambuf_iterator<char>());
                }
            }
            auto m2 = wowee::pipeline::M2Loader::load(bytes);
            if (!skinBytes.empty()) {
                wowee::pipeline::M2Loader::loadSkin(skinBytes, m2);
            }
            if (!m2.isValid()) {
                std::fprintf(stderr, "info-m2: failed to parse %s\n", path.c_str());
                return 1;
            }
            std::error_code ec;
            uint64_t fsz = std::filesystem::file_size(path, ec);
            if (jsonOut) {
                nlohmann::json j;
                j["m2"] = path;
                j["name"] = m2.name;
                j["version"] = m2.version;
                j["fileSize"] = fsz;
                j["skinFound"] = !skinBytes.empty();
                j["vertices"] = m2.vertices.size();
                j["indices"] = m2.indices.size();
                j["triangles"] = m2.indices.size() / 3;
                j["bones"] = m2.bones.size();
                j["sequences"] = m2.sequences.size();
                j["batches"] = m2.batches.size();
                j["textures"] = m2.textures.size();
                j["materials"] = m2.materials.size();
                j["attachments"] = m2.attachments.size();
                j["particles"] = m2.particleEmitters.size();
                j["ribbons"] = m2.ribbonEmitters.size();
                j["collisionTris"] = m2.collisionIndices.size() / 3;
                j["boundRadius"] = m2.boundRadius;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("M2: %s\n", path.c_str());
            std::printf("  name        : %s\n", m2.name.c_str());
            std::printf("  version     : %u\n", m2.version);
            std::printf("  file bytes  : %llu\n", static_cast<unsigned long long>(fsz));
            std::printf("  skin file   : %s\n", skinBytes.empty() ? "not found" : "loaded");
            std::printf("  vertices    : %zu\n", m2.vertices.size());
            std::printf("  triangles   : %zu (%zu indices)\n",
                        m2.indices.size() / 3, m2.indices.size());
            std::printf("  bones       : %zu\n", m2.bones.size());
            std::printf("  sequences   : %zu (animations)\n", m2.sequences.size());
            std::printf("  batches     : %zu\n", m2.batches.size());
            std::printf("  textures    : %zu\n", m2.textures.size());
            std::printf("  materials   : %zu\n", m2.materials.size());
            std::printf("  attachments : %zu\n", m2.attachments.size());
            std::printf("  particles   : %zu\n", m2.particleEmitters.size());
            std::printf("  ribbons     : %zu\n", m2.ribbonEmitters.size());
            std::printf("  collision   : %zu tris\n", m2.collisionIndices.size() / 3);
            std::printf("  boundRadius : %.2f\n", m2.boundRadius);
            return 0;
        } else if (std::strcmp(argv[i], "--info-wmo") == 0 && i + 1 < argc) {
            // Inspect a proprietary WMO building. Like --info-m2 this
            // pairs with --info-wob (the open WOB equivalent inspector)
            // so users can verify the conversion preserves group counts,
            // portal counts, and doodad references.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "info-wmo: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            auto wmo = wowee::pipeline::WMOLoader::load(bytes);
            // Try to locate group files (Foo_NNN.wmo) sitting next to the
            // root file and merge their geometry. Without this the
            // group/vertex counts would all be 0 since the root file only
            // has metadata.
            namespace fs = std::filesystem;
            std::string base = path;
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wmo")
                base = base.substr(0, base.size() - 4);
            // Pre-allocate the groups array — loadGroup writes into
            // model.groups[gi] and bails if the slot doesn't exist.
            if (wmo.groups.size() < wmo.nGroups) wmo.groups.resize(wmo.nGroups);
            int groupsLoaded = 0;
            for (uint32_t gi = 0; gi < wmo.nGroups; ++gi) {
                // "_000.wmo" is 8 chars + NUL = 9 bytes; previous 8-byte
                // buffer was truncating to "_000.wm" and silently failing
                // every lookup.
                char buf[16];
                std::snprintf(buf, sizeof(buf), "_%03u.wmo", gi);
                std::string gp = base + buf;
                std::ifstream gf(gp, std::ios::binary);
                if (!gf) continue;
                std::vector<uint8_t> gd((std::istreambuf_iterator<char>(gf)),
                                         std::istreambuf_iterator<char>());
                if (wowee::pipeline::WMOLoader::loadGroup(gd, wmo, gi)) groupsLoaded++;
            }
            if (!wmo.isValid()) {
                std::fprintf(stderr, "info-wmo: failed to parse %s\n", path.c_str());
                return 1;
            }
            // Total vertex/index counts across loaded groups — this is the
            // useful number for sizing comparisons against WOB.
            size_t totalV = 0, totalI = 0;
            for (const auto& g : wmo.groups) {
                totalV += g.vertices.size();
                totalI += g.indices.size();
            }
            std::error_code ec;
            uint64_t fsz = fs::file_size(path, ec);
            if (jsonOut) {
                nlohmann::json j;
                j["wmo"] = path;
                j["version"] = wmo.version;
                j["fileSize"] = fsz;
                j["groups"] = wmo.nGroups;
                j["groupsLoaded"] = groupsLoaded;
                j["portals"] = wmo.nPortals;
                j["lights"] = wmo.nLights;
                j["doodadDefs"] = wmo.doodads.size();
                j["doodadSets"] = wmo.doodadSets.size();
                j["materials"] = wmo.materials.size();
                j["textures"] = wmo.textures.size();
                j["totalVerts"] = totalV;
                j["totalTris"] = totalI / 3;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("WMO: %s\n", path.c_str());
            std::printf("  version       : %u\n", wmo.version);
            std::printf("  file bytes    : %llu\n", static_cast<unsigned long long>(fsz));
            std::printf("  groups        : %u (%d loaded from group files)\n",
                        wmo.nGroups, groupsLoaded);
            std::printf("  portals       : %u\n", wmo.nPortals);
            std::printf("  lights        : %u\n", wmo.nLights);
            std::printf("  doodad defs   : %zu (%zu sets)\n",
                        wmo.doodads.size(), wmo.doodadSets.size());
            std::printf("  materials     : %zu\n", wmo.materials.size());
            std::printf("  textures      : %zu\n", wmo.textures.size());
            std::printf("  total verts   : %zu\n", totalV);
            std::printf("  total tris    : %zu\n", totalI / 3);
            return 0;
        } else if (std::strcmp(argv[i], "--info-adt") == 0 && i + 1 < argc) {
            // Inspect a proprietary ADT terrain tile. Pairs with
            // --info-wot/--info-whm (open WOT/WHM equivalents) so users
            // can verify the conversion preserves chunk/doodad/wmo counts.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "info-adt: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            auto terrain = wowee::pipeline::ADTLoader::load(bytes);
            if (!terrain.isLoaded()) {
                std::fprintf(stderr, "info-adt: failed to parse %s\n", path.c_str());
                return 1;
            }
            // Walk chunks and tally height range + loaded count + water/holes.
            int loadedChunks = 0, holeChunks = 0, waterChunks = 0;
            float minH = 1e30f, maxH = -1e30f;
            for (size_t c = 0; c < 256; ++c) {
                const auto& chunk = terrain.chunks[c];
                if (!chunk.heightMap.isLoaded()) continue;
                loadedChunks++;
                if (chunk.holes != 0) holeChunks++;
                if (terrain.waterData[c].hasWater()) waterChunks++;
                for (float h : chunk.heightMap.heights) {
                    if (std::isfinite(h)) {
                        if (h < minH) minH = h;
                        if (h > maxH) maxH = h;
                    }
                }
            }
            std::error_code ec;
            uint64_t fsz = std::filesystem::file_size(path, ec);
            if (jsonOut) {
                nlohmann::json j;
                j["adt"] = path;
                j["version"] = terrain.version;
                j["fileSize"] = fsz;
                j["coord"] = {terrain.coord.x, terrain.coord.y};
                j["loadedChunks"] = loadedChunks;
                j["holeChunks"] = holeChunks;
                j["waterChunks"] = waterChunks;
                j["heightMin"] = (loadedChunks > 0) ? minH : 0.0f;
                j["heightMax"] = (loadedChunks > 0) ? maxH : 0.0f;
                j["textures"] = terrain.textures.size();
                j["doodadNames"] = terrain.doodadNames.size();
                j["wmoNames"] = terrain.wmoNames.size();
                j["doodadPlacements"] = terrain.doodadPlacements.size();
                j["wmoPlacements"] = terrain.wmoPlacements.size();
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("ADT: %s\n", path.c_str());
            std::printf("  version          : %u\n", terrain.version);
            std::printf("  file bytes       : %llu\n", static_cast<unsigned long long>(fsz));
            std::printf("  coord            : (%d, %d)\n", terrain.coord.x, terrain.coord.y);
            std::printf("  chunks loaded    : %d/256\n", loadedChunks);
            if (loadedChunks > 0) {
                std::printf("  height range     : [%.2f, %.2f]\n", minH, maxH);
            }
            std::printf("  hole chunks      : %d (with cave/gap masks)\n", holeChunks);
            std::printf("  water chunks     : %d\n", waterChunks);
            std::printf("  textures         : %zu\n", terrain.textures.size());
            std::printf("  doodad names     : %zu (%zu placements)\n",
                        terrain.doodadNames.size(),
                        terrain.doodadPlacements.size());
            std::printf("  wmo names        : %zu (%zu placements)\n",
                        terrain.wmoNames.size(),
                        terrain.wmoPlacements.size());
            return 0;
        } else if (std::strcmp(argv[i], "--info-jsondbc") == 0 && i + 1 < argc) {
            // Inspect a JSON DBC sidecar (the JSON output of asset_extract
            // --emit-json-dbc). Reports recordCount, fieldCount, source
            // filename, and format version — useful for verifying the
            // sidecar tracks the proprietary file's row count.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path);
            if (!in) {
                std::fprintf(stderr, "info-jsondbc: cannot open %s\n", path.c_str());
                return 1;
            }
            nlohmann::json doc;
            try {
                in >> doc;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "info-jsondbc: bad JSON in %s (%s)\n",
                             path.c_str(), e.what());
                return 1;
            }
            // The wowee JSON DBC schema (from open_format_emitter.cpp):
            // {format, source, recordCount, fieldCount, records:[[...], ...]}.
            // Tolerate missing fields rather than crashing — old sidecars
            // may predate a field addition.
            std::string format = doc.value("format", std::string{});
            std::string source = doc.value("source", std::string{});
            uint32_t recordCount = doc.value("recordCount", 0u);
            uint32_t fieldCount  = doc.value("fieldCount",  0u);
            uint32_t actualRecs = 0;
            if (doc.contains("records") && doc["records"].is_array()) {
                actualRecs = static_cast<uint32_t>(doc["records"].size());
            }
            bool countMismatch = (recordCount != actualRecs);
            if (jsonOut) {
                nlohmann::json j;
                j["jsondbc"] = path;
                j["format"] = format;
                j["source"] = source;
                j["recordCount"] = recordCount;
                j["fieldCount"] = fieldCount;
                j["actualRecords"] = actualRecs;
                j["countMismatch"] = countMismatch;
                std::printf("%s\n", j.dump(2).c_str());
                return countMismatch ? 1 : 0;
            }
            std::printf("JSON DBC: %s\n", path.c_str());
            std::printf("  format    : %s\n", format.empty() ? "?" : format.c_str());
            std::printf("  source    : %s\n", source.empty() ? "?" : source.c_str());
            std::printf("  records   : %u (header) / %u (actual)%s\n",
                        recordCount, actualRecs,
                        countMismatch ? " [MISMATCH]" : "");
            std::printf("  fields    : %u\n", fieldCount);
            return countMismatch ? 1 : 0;
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
        } else if (std::strcmp(argv[i], "--list-creatures") == 0 && i + 1 < argc) {
            // Verbose enumeration of every spawn — needed because
            // --remove-creature takes a 0-based index but --info-creatures
            // only shows aggregate counts.
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
            if (jsonOut) {
                nlohmann::json j;
                j["file"] = path;
                j["total"] = spawns.size();
                nlohmann::json arr = nlohmann::json::array();
                for (size_t k = 0; k < spawns.size(); ++k) {
                    const auto& s = spawns[k];
                    arr.push_back({
                        {"index", k},
                        {"name", s.name},
                        {"displayId", s.displayId},
                        {"level", s.level},
                        {"position", {s.position.x, s.position.y, s.position.z}},
                        {"hostile", s.hostile},
                    });
                }
                j["spawns"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("creatures.json: %s (%zu total)\n", path.c_str(), spawns.size());
            std::printf("  idx  name                            lvl  display  pos (x, y, z)\n");
            for (size_t k = 0; k < spawns.size(); ++k) {
                const auto& s = spawns[k];
                std::printf("  %3zu  %-30s %3u  %7u  (%.1f, %.1f, %.1f)%s\n",
                            k, s.name.substr(0, 30).c_str(), s.level, s.displayId,
                            s.position.x, s.position.y, s.position.z,
                            s.hostile ? " [hostile]" : "");
            }
            return 0;
        } else if (std::strcmp(argv[i], "--list-objects") == 0 && i + 1 < argc) {
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
            auto typeStr = [](wowee::editor::PlaceableType t) {
                return t == wowee::editor::PlaceableType::M2 ? "m2" : "wmo";
            };
            if (jsonOut) {
                nlohmann::json j;
                j["file"] = path;
                j["total"] = objs.size();
                nlohmann::json arr = nlohmann::json::array();
                for (size_t k = 0; k < objs.size(); ++k) {
                    const auto& o = objs[k];
                    arr.push_back({
                        {"index", k},
                        {"type", typeStr(o.type)},
                        {"path", o.path},
                        {"position", {o.position.x, o.position.y, o.position.z}},
                        {"scale", o.scale},
                    });
                }
                j["objects"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("objects.json: %s (%zu total)\n", path.c_str(), objs.size());
            std::printf("  idx  type  scale  path                                    pos (x, y, z)\n");
            for (size_t k = 0; k < objs.size(); ++k) {
                const auto& o = objs[k];
                std::printf("  %3zu  %-4s  %5.2f  %-38s  (%.1f, %.1f, %.1f)\n",
                            k, typeStr(o.type), o.scale,
                            o.path.substr(0, 38).c_str(),
                            o.position.x, o.position.y, o.position.z);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--list-quests") == 0 && i + 1 < argc) {
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
            if (jsonOut) {
                nlohmann::json j;
                j["file"] = path;
                j["total"] = quests.size();
                nlohmann::json arr = nlohmann::json::array();
                for (size_t k = 0; k < quests.size(); ++k) {
                    const auto& q = quests[k];
                    arr.push_back({
                        {"index", k},
                        {"title", q.title},
                        {"giver", q.questGiverNpcId},
                        {"turnIn", q.turnInNpcId},
                        {"requiredLevel", q.requiredLevel},
                        {"xp", q.reward.xp},
                        {"nextQuestId", q.nextQuestId},
                    });
                }
                j["quests"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("quests.json: %s (%zu total)\n", path.c_str(), quests.size());
            std::printf("  idx  lvl  giver    turnIn   xp     title\n");
            for (size_t k = 0; k < quests.size(); ++k) {
                const auto& q = quests[k];
                std::printf("  %3zu  %3u  %7u  %7u  %5u  %s%s\n",
                            k, q.requiredLevel, q.questGiverNpcId, q.turnInNpcId,
                            q.reward.xp, q.title.c_str(),
                            q.nextQuestId ? " [chained]" : "");
            }
            return 0;
        } else if (std::strcmp(argv[i], "--list-quest-objectives") == 0 && i + 2 < argc) {
            // Per-quest objective listing — pairs with --remove-quest-objective
            // (which takes objIdx). Tabulates type, target, count, description.
            std::string path = argv[++i];
            std::string idxStr = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            int qIdx;
            try { qIdx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "list-quest-objectives: bad questIdx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "list-quest-objectives: failed to load %s\n", path.c_str());
                return 1;
            }
            if (qIdx < 0 || qIdx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr,
                    "list-quest-objectives: questIdx %d out of range [0, %zu)\n",
                    qIdx, qe.questCount());
                return 1;
            }
            const auto& q = qe.getQuests()[qIdx];
            using OT = wowee::editor::QuestObjectiveType;
            auto typeName = [](OT t) {
                switch (t) {
                    case OT::KillCreature: return "kill";
                    case OT::CollectItem:  return "collect";
                    case OT::TalkToNPC:    return "talk";
                    case OT::ExploreArea:  return "explore";
                    case OT::EscortNPC:    return "escort";
                    case OT::UseObject:    return "use";
                }
                return "?";
            };
            if (jsonOut) {
                nlohmann::json j;
                j["file"] = path;
                j["questIdx"] = qIdx;
                j["title"] = q.title;
                j["count"] = q.objectives.size();
                nlohmann::json arr = nlohmann::json::array();
                for (size_t o = 0; o < q.objectives.size(); ++o) {
                    const auto& ob = q.objectives[o];
                    arr.push_back({
                        {"index", o},
                        {"type", typeName(ob.type)},
                        {"target", ob.targetName},
                        {"count", ob.targetCount},
                        {"description", ob.description},
                    });
                }
                j["objectives"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Quest %d ('%s'): %zu objective(s)\n",
                        qIdx, q.title.c_str(), q.objectives.size());
            std::printf("  idx  type     count  target              description\n");
            for (size_t o = 0; o < q.objectives.size(); ++o) {
                const auto& ob = q.objectives[o];
                std::printf("  %3zu  %-7s  %5u  %-18s  %s\n",
                            o, typeName(ob.type), ob.targetCount,
                            ob.targetName.substr(0, 18).c_str(),
                            ob.description.c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--list-quest-rewards") == 0 && i + 2 < argc) {
            // Per-quest reward listing. Shows XP/coin breakdown plus the
            // full itemRewards list (which --info-quests only counts).
            std::string path = argv[++i];
            std::string idxStr = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            int qIdx;
            try { qIdx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "list-quest-rewards: bad questIdx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "list-quest-rewards: failed to load %s\n", path.c_str());
                return 1;
            }
            if (qIdx < 0 || qIdx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr,
                    "list-quest-rewards: questIdx %d out of range [0, %zu)\n",
                    qIdx, qe.questCount());
                return 1;
            }
            const auto& q = qe.getQuests()[qIdx];
            const auto& r = q.reward;
            if (jsonOut) {
                nlohmann::json j;
                j["file"] = path;
                j["questIdx"] = qIdx;
                j["title"] = q.title;
                j["xp"] = r.xp;
                j["gold"] = r.gold;
                j["silver"] = r.silver;
                j["copper"] = r.copper;
                j["items"] = r.itemRewards;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Quest %d ('%s') rewards:\n", qIdx, q.title.c_str());
            std::printf("  xp     : %u\n", r.xp);
            std::printf("  coin   : %ug %us %uc\n", r.gold, r.silver, r.copper);
            std::printf("  items  : %zu\n", r.itemRewards.size());
            for (size_t k = 0; k < r.itemRewards.size(); ++k) {
                std::printf("    [%zu] %s\n", k, r.itemRewards[k].c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-creature") == 0 && i + 2 < argc) {
            // Single-creature deep dive — every CreatureSpawn field for
            // one entry. Companion to --list-creatures (which is a
            // table view); useful for digging into 'why is this NPC
            // not behaving like I expect?'.
            std::string path = argv[++i];
            std::string idxStr = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "info-creature: bad idx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::NpcSpawner sp;
            if (!sp.loadFromFile(path)) {
                std::fprintf(stderr, "info-creature: failed to load %s\n", path.c_str());
                return 1;
            }
            if (idx < 0 || idx >= static_cast<int>(sp.spawnCount())) {
                std::fprintf(stderr,
                    "info-creature: idx %d out of range [0, %zu)\n",
                    idx, sp.spawnCount());
                return 1;
            }
            const auto& s = sp.getSpawns()[idx];
            using B = wowee::editor::CreatureBehavior;
            const char* behavior =
                s.behavior == B::Patrol ? "patrol" :
                s.behavior == B::Wander ? "wander" : "stationary";
            if (jsonOut) {
                nlohmann::json j;
                j["index"] = idx;
                j["id"] = s.id;
                j["name"] = s.name;
                j["modelPath"] = s.modelPath;
                j["displayId"] = s.displayId;
                j["position"] = {s.position.x, s.position.y, s.position.z};
                j["orientation"] = s.orientation;
                j["level"] = s.level;
                j["health"] = s.health;
                j["mana"] = s.mana;
                j["minDamage"] = s.minDamage;
                j["maxDamage"] = s.maxDamage;
                j["armor"] = s.armor;
                j["faction"] = s.faction;
                j["scale"] = s.scale;
                j["behavior"] = behavior;
                j["wanderRadius"] = s.wanderRadius;
                j["aggroRadius"] = s.aggroRadius;
                j["leashRadius"] = s.leashRadius;
                j["respawnTimeMs"] = s.respawnTimeMs;
                j["patrolPoints"] = s.patrolPath.size();
                j["hostile"] = s.hostile;
                j["questgiver"] = s.questgiver;
                j["vendor"] = s.vendor;
                j["trainer"] = s.trainer;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Creature [%d] '%s'\n", idx, s.name.c_str());
            std::printf("  id            : %u\n", s.id);
            std::printf("  displayId     : %u\n", s.displayId);
            std::printf("  modelPath     : %s\n",
                        s.modelPath.empty() ? "(uses displayId)" : s.modelPath.c_str());
            std::printf("  position      : (%.2f, %.2f, %.2f)\n",
                        s.position.x, s.position.y, s.position.z);
            std::printf("  orientation   : %.2f deg\n", s.orientation);
            std::printf("  scale         : %.2f\n", s.scale);
            std::printf("  level         : %u\n", s.level);
            std::printf("  health/mana   : %u / %u\n", s.health, s.mana);
            std::printf("  damage        : %u-%u\n", s.minDamage, s.maxDamage);
            std::printf("  armor         : %u\n", s.armor);
            std::printf("  faction       : %u\n", s.faction);
            std::printf("  behavior      : %s\n", behavior);
            std::printf("  wander rad    : %.1f\n", s.wanderRadius);
            std::printf("  aggro rad     : %.1f\n", s.aggroRadius);
            std::printf("  leash rad     : %.1f\n", s.leashRadius);
            std::printf("  respawn ms    : %u\n", s.respawnTimeMs);
            std::printf("  patrol points : %zu\n", s.patrolPath.size());
            std::printf("  flags         : %s%s%s%s\n",
                        s.hostile ? "hostile " : "",
                        s.questgiver ? "questgiver " : "",
                        s.vendor ? "vendor " : "",
                        s.trainer ? "trainer " : "");
            return 0;
        } else if (std::strcmp(argv[i], "--info-quest") == 0 && i + 2 < argc) {
            // Single-quest deep dive — combines what --list-quest-objectives
            // and --list-quest-rewards show into one view, plus the chain
            // pointer + descriptions that neither covers.
            std::string path = argv[++i];
            std::string idxStr = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "info-quest: bad idx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "info-quest: failed to load %s\n", path.c_str());
                return 1;
            }
            if (idx < 0 || idx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr,
                    "info-quest: idx %d out of range [0, %zu)\n",
                    idx, qe.questCount());
                return 1;
            }
            const auto& q = qe.getQuests()[idx];
            using OT = wowee::editor::QuestObjectiveType;
            auto typeName = [](OT t) {
                switch (t) {
                    case OT::KillCreature: return "kill";
                    case OT::CollectItem:  return "collect";
                    case OT::TalkToNPC:    return "talk";
                    case OT::ExploreArea:  return "explore";
                    case OT::EscortNPC:    return "escort";
                    case OT::UseObject:    return "use";
                }
                return "?";
            };
            if (jsonOut) {
                nlohmann::json j;
                j["index"] = idx;
                j["id"] = q.id;
                j["title"] = q.title;
                j["description"] = q.description;
                j["completionText"] = q.completionText;
                j["requiredLevel"] = q.requiredLevel;
                j["questGiverNpcId"] = q.questGiverNpcId;
                j["turnInNpcId"] = q.turnInNpcId;
                j["nextQuestId"] = q.nextQuestId;
                j["reward"] = {
                    {"xp", q.reward.xp},
                    {"gold", q.reward.gold},
                    {"silver", q.reward.silver},
                    {"copper", q.reward.copper},
                    {"items", q.reward.itemRewards}
                };
                nlohmann::json objs = nlohmann::json::array();
                for (const auto& obj : q.objectives) {
                    objs.push_back({
                        {"type", typeName(obj.type)},
                        {"target", obj.targetName},
                        {"count", obj.targetCount},
                        {"description", obj.description}
                    });
                }
                j["objectives"] = objs;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Quest [%d] '%s'\n", idx, q.title.c_str());
            std::printf("  id              : %u\n", q.id);
            std::printf("  required level  : %u\n", q.requiredLevel);
            std::printf("  giver NPC id    : %u\n", q.questGiverNpcId);
            std::printf("  turn-in NPC id  : %u\n", q.turnInNpcId);
            std::printf("  next quest id   : %u%s\n", q.nextQuestId,
                        q.nextQuestId == 0 ? " (terminal)" : "");
            if (!q.description.empty()) {
                std::printf("  description     : %s\n", q.description.c_str());
            }
            if (!q.completionText.empty()) {
                std::printf("  completion text : %s\n", q.completionText.c_str());
            }
            std::printf("  reward          : %u XP, %ug %us %uc, %zu item(s)\n",
                        q.reward.xp, q.reward.gold, q.reward.silver,
                        q.reward.copper, q.reward.itemRewards.size());
            for (size_t k = 0; k < q.reward.itemRewards.size(); ++k) {
                std::printf("    item[%zu]      : %s\n", k,
                            q.reward.itemRewards[k].c_str());
            }
            std::printf("  objectives      : %zu\n", q.objectives.size());
            for (size_t k = 0; k < q.objectives.size(); ++k) {
                const auto& o = q.objectives[k];
                std::printf("    [%zu] %-7s ×%u  %s%s%s\n",
                            k, typeName(o.type), o.targetCount,
                            o.targetName.c_str(),
                            o.description.empty() ? "" : "  — ",
                            o.description.c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-object") == 0 && i + 2 < argc) {
            // Single-object deep dive — every PlacedObject field for one
            // entry. Completes the single-entity inspector trio
            // (creature/quest/object).
            std::string path = argv[++i];
            std::string idxStr = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "info-object: bad idx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::ObjectPlacer placer;
            if (!placer.loadFromFile(path)) {
                std::fprintf(stderr, "info-object: failed to load %s\n", path.c_str());
                return 1;
            }
            const auto& objs = placer.getObjects();
            if (idx < 0 || idx >= static_cast<int>(objs.size())) {
                std::fprintf(stderr,
                    "info-object: idx %d out of range [0, %zu)\n",
                    idx, objs.size());
                return 1;
            }
            const auto& o = objs[idx];
            const char* typeStr =
                o.type == wowee::editor::PlaceableType::M2 ? "m2" : "wmo";
            if (jsonOut) {
                nlohmann::json j;
                j["index"] = idx;
                j["type"] = typeStr;
                j["path"] = o.path;
                j["nameId"] = o.nameId;
                j["uniqueId"] = o.uniqueId;
                j["position"] = {o.position.x, o.position.y, o.position.z};
                j["rotation"] = {o.rotation.x, o.rotation.y, o.rotation.z};
                j["scale"] = o.scale;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Object [%d]\n", idx);
            std::printf("  type      : %s\n", typeStr);
            std::printf("  path      : %s\n", o.path.c_str());
            std::printf("  nameId    : %u\n", o.nameId);
            std::printf("  uniqueId  : %u%s\n", o.uniqueId,
                        o.uniqueId == 0 ? " (unassigned)" : "");
            std::printf("  position  : (%.3f, %.3f, %.3f)\n",
                        o.position.x, o.position.y, o.position.z);
            std::printf("  rotation  : (%.2f, %.2f, %.2f) deg\n",
                        o.rotation.x, o.rotation.y, o.rotation.z);
            std::printf("  scale     : %.3f\n", o.scale);
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
        } else if (std::strcmp(argv[i], "--diff-zone") == 0 && i + 2 < argc) {
            // Compare two unpacked zone directories: zone.json fields,
            // creature names, object paths, quest titles. Useful when a
            // designer wants to see what changed between an upstream
            // template (--copy-zone source) and their customized variant,
            // or to verify a refactor only touched what it claimed to.
            std::string aDir = argv[++i];
            std::string bDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            for (const auto& d : {aDir, bDir}) {
                if (!fs::exists(d + "/zone.json")) {
                    std::fprintf(stderr,
                        "diff-zone: %s has no zone.json — not a zone dir\n",
                        d.c_str());
                    return 1;
                }
            }
            wowee::editor::ZoneManifest aZ, bZ;
            aZ.load(aDir + "/zone.json");
            bZ.load(bDir + "/zone.json");
            // Helper: load a sub-file if present, returning empty container
            // when missing — both sides may legitimately omit a content
            // file (e.g. a quest-free zone) without that being a diff per se.
            auto loadCreatures = [](const std::string& dir) {
                std::vector<std::string> names;
                wowee::editor::NpcSpawner sp;
                if (sp.loadFromFile(dir + "/creatures.json")) {
                    for (const auto& s : sp.getSpawns()) names.push_back(s.name);
                }
                std::sort(names.begin(), names.end());
                return names;
            };
            auto loadObjectPaths = [](const std::string& dir) {
                std::vector<std::string> paths;
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile(dir + "/objects.json")) {
                    for (const auto& o : op.getObjects()) paths.push_back(o.path);
                }
                std::sort(paths.begin(), paths.end());
                return paths;
            };
            auto loadQuestTitles = [](const std::string& dir) {
                std::vector<std::string> titles;
                wowee::editor::QuestEditor qe;
                if (qe.loadFromFile(dir + "/quests.json")) {
                    for (const auto& q : qe.getQuests()) titles.push_back(q.title);
                }
                std::sort(titles.begin(), titles.end());
                return titles;
            };
            auto aCreatures = loadCreatures(aDir);
            auto bCreatures = loadCreatures(bDir);
            auto aObjects = loadObjectPaths(aDir);
            auto bObjects = loadObjectPaths(bDir);
            auto aQuests = loadQuestTitles(aDir);
            auto bQuests = loadQuestTitles(bDir);
            // Set diff: returns (onlyA, onlyB) where each is a sorted list.
            auto setDiff = [](const std::vector<std::string>& a,
                              const std::vector<std::string>& b) {
                std::vector<std::string> onlyA, onlyB;
                std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                                    std::back_inserter(onlyA));
                std::set_difference(b.begin(), b.end(), a.begin(), a.end(),
                                    std::back_inserter(onlyB));
                return std::pair{onlyA, onlyB};
            };
            auto [creatOnlyA, creatOnlyB] = setDiff(aCreatures, bCreatures);
            auto [objOnlyA, objOnlyB] = setDiff(aObjects, bObjects);
            auto [questOnlyA, questOnlyB] = setDiff(aQuests, bQuests);
            // Manifest field diffs.
            std::vector<std::string> manifestDiffs;
            auto cmp = [&](const char* field, const std::string& a,
                           const std::string& b) {
                if (a != b) {
                    manifestDiffs.push_back(std::string(field) + ": '" +
                                            a + "' -> '" + b + "'");
                }
            };
            cmp("mapName",      aZ.mapName,      bZ.mapName);
            cmp("displayName",  aZ.displayName,  bZ.displayName);
            cmp("biome",        aZ.biome,        bZ.biome);
            cmp("musicTrack",   aZ.musicTrack,   bZ.musicTrack);
            if (aZ.mapId != bZ.mapId) {
                manifestDiffs.push_back("mapId: " + std::to_string(aZ.mapId) +
                                        " -> " + std::to_string(bZ.mapId));
            }
            if (aZ.tiles.size() != bZ.tiles.size()) {
                manifestDiffs.push_back("tile count: " + std::to_string(aZ.tiles.size()) +
                                        " -> " + std::to_string(bZ.tiles.size()));
            }
            int diffs = manifestDiffs.size() +
                        creatOnlyA.size() + creatOnlyB.size() +
                        objOnlyA.size() + objOnlyB.size() +
                        questOnlyA.size() + questOnlyB.size();
            if (jsonOut) {
                nlohmann::json j;
                j["a"] = aDir;
                j["b"] = bDir;
                j["identical"] = (diffs == 0);
                j["manifestDiffs"] = manifestDiffs;
                j["creatures"] = {{"a", aCreatures.size()},
                                   {"b", bCreatures.size()},
                                   {"onlyA", creatOnlyA},
                                   {"onlyB", creatOnlyB}};
                j["objects"] = {{"a", aObjects.size()},
                                 {"b", bObjects.size()},
                                 {"onlyA", objOnlyA},
                                 {"onlyB", objOnlyB}};
                j["quests"] = {{"a", aQuests.size()},
                                {"b", bQuests.size()},
                                {"onlyA", questOnlyA},
                                {"onlyB", questOnlyB}};
                j["totalDiffs"] = diffs;
                std::printf("%s\n", j.dump(2).c_str());
                return diffs == 0 ? 0 : 1;
            }
            std::printf("Diff: %s vs %s\n", aDir.c_str(), bDir.c_str());
            if (diffs == 0) {
                std::printf("  IDENTICAL\n");
                return 0;
            }
            std::printf("  manifest  : %zu field diff(s)\n", manifestDiffs.size());
            for (const auto& d : manifestDiffs) std::printf("    ~ %s\n", d.c_str());
            std::printf("  creatures : %zu vs %zu\n",
                        aCreatures.size(), bCreatures.size());
            for (const auto& s : creatOnlyA) std::printf("    - %s\n", s.c_str());
            for (const auto& s : creatOnlyB) std::printf("    + %s\n", s.c_str());
            std::printf("  objects   : %zu vs %zu\n",
                        aObjects.size(), bObjects.size());
            for (const auto& s : objOnlyA) std::printf("    - %s\n", s.c_str());
            for (const auto& s : objOnlyB) std::printf("    + %s\n", s.c_str());
            std::printf("  quests    : %zu vs %zu\n",
                        aQuests.size(), bQuests.size());
            for (const auto& s : questOnlyA) std::printf("    - %s\n", s.c_str());
            for (const auto& s : questOnlyB) std::printf("    + %s\n", s.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--diff-glb") == 0 && i + 2 < argc) {
            // Structural compare of two .glb files. Useful for confirming
            // that an alternate export path produces equivalent output
            // (e.g. --bake-zone-glb vs concatenated --export-whm-glbs)
            // or that a re-export of the same source is byte-equivalent.
            // Compares structure (mesh/primitive/accessor counts +
            // chunk sizes), NOT byte-level — JSON key ordering can vary.
            std::string aPath = argv[++i];
            std::string bPath = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            // Reuse the parser from --info-glb. Inline here since it's
            // small and the alternative is a 3-way handler refactor.
            auto loadGlb = [](const std::string& path,
                              uint32_t& outJsonLen, uint32_t& outBinLen,
                              std::string& outJsonStr) -> bool {
                std::ifstream in(path, std::ios::binary);
                if (!in) return false;
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
                if (bytes.size() < 20) return false;
                uint32_t magic, version, totalLen;
                std::memcpy(&magic,    &bytes[0], 4);
                std::memcpy(&version,  &bytes[4], 4);
                std::memcpy(&totalLen, &bytes[8], 4);
                if (magic != 0x46546C67 || version != 2) return false;
                std::memcpy(&outJsonLen, &bytes[12], 4);
                if (20 + outJsonLen > bytes.size()) return false;
                outJsonStr.assign(bytes.begin() + 20,
                                   bytes.begin() + 20 + outJsonLen);
                size_t binOff = 20 + outJsonLen;
                if (binOff + 8 <= bytes.size()) {
                    std::memcpy(&outBinLen, &bytes[binOff], 4);
                } else {
                    outBinLen = 0;
                }
                return true;
            };
            uint32_t aJsonLen = 0, aBinLen = 0;
            uint32_t bJsonLen = 0, bBinLen = 0;
            std::string aJsonStr, bJsonStr;
            if (!loadGlb(aPath, aJsonLen, aBinLen, aJsonStr)) {
                std::fprintf(stderr, "diff-glb: failed to read %s\n", aPath.c_str());
                return 1;
            }
            if (!loadGlb(bPath, bJsonLen, bBinLen, bJsonStr)) {
                std::fprintf(stderr, "diff-glb: failed to read %s\n", bPath.c_str());
                return 1;
            }
            // Pull structural counts from JSON. Skip if parse fails on
            // either side — diff is meaningless then.
            auto countOf = [](const nlohmann::json& j, const char* key) {
                if (j.contains(key) && j[key].is_array()) {
                    return static_cast<int>(j[key].size());
                }
                return 0;
            };
            int aMesh = 0, aPrim = 0, aAcc = 0, aBV = 0, aBuf = 0;
            int bMesh = 0, bPrim = 0, bAcc = 0, bBV = 0, bBuf = 0;
            try {
                auto aj = nlohmann::json::parse(aJsonStr);
                auto bj = nlohmann::json::parse(bJsonStr);
                aMesh = countOf(aj, "meshes");
                bMesh = countOf(bj, "meshes");
                if (aj.contains("meshes") && aj["meshes"].is_array()) {
                    for (const auto& m : aj["meshes"]) {
                        if (m.contains("primitives") && m["primitives"].is_array()) {
                            aPrim += static_cast<int>(m["primitives"].size());
                        }
                    }
                }
                if (bj.contains("meshes") && bj["meshes"].is_array()) {
                    for (const auto& m : bj["meshes"]) {
                        if (m.contains("primitives") && m["primitives"].is_array()) {
                            bPrim += static_cast<int>(m["primitives"].size());
                        }
                    }
                }
                aAcc = countOf(aj, "accessors"); bAcc = countOf(bj, "accessors");
                aBV  = countOf(aj, "bufferViews"); bBV  = countOf(bj, "bufferViews");
                aBuf = countOf(aj, "buffers");   bBuf = countOf(bj, "buffers");
            } catch (const std::exception&) {
                std::fprintf(stderr, "diff-glb: JSON parse failed on one side\n");
                return 1;
            }
            int diffs = (aMesh != bMesh) + (aPrim != bPrim) + (aAcc != bAcc) +
                        (aBV != bBV) + (aBuf != bBuf) +
                        (aBinLen != bBinLen);
            if (jsonOut) {
                nlohmann::json j;
                j["a"] = aPath; j["b"] = bPath;
                j["meshes"]      = {{"a", aMesh},  {"b", bMesh}};
                j["primitives"]  = {{"a", aPrim},  {"b", bPrim}};
                j["accessors"]   = {{"a", aAcc},   {"b", bAcc}};
                j["bufferViews"] = {{"a", aBV},    {"b", bBV}};
                j["buffers"]     = {{"a", aBuf},   {"b", bBuf}};
                j["binBytes"]    = {{"a", aBinLen},{"b", bBinLen}};
                j["jsonBytes"]   = {{"a", aJsonLen},{"b", bJsonLen}};
                j["totalDiffs"]  = diffs;
                j["identical"]   = (diffs == 0);
                std::printf("%s\n", j.dump(2).c_str());
                return diffs == 0 ? 0 : 1;
            }
            auto cmp = [](const char* name, int a, int b) {
                std::printf("  %-12s: %6d %6d  %s\n", name, a, b,
                            a == b ? "" : "DIFF");
            };
            std::printf("Diff: %s vs %s\n", aPath.c_str(), bPath.c_str());
            std::printf("                       a      b\n");
            cmp("meshes",      aMesh, bMesh);
            cmp("primitives",  aPrim, bPrim);
            cmp("accessors",   aAcc,  bAcc);
            cmp("bufferViews", aBV,   bBV);
            cmp("buffers",     aBuf,  bBuf);
            cmp("BIN bytes",   static_cast<int>(aBinLen),
                                static_cast<int>(bBinLen));
            if (diffs == 0) {
                std::printf("  IDENTICAL\n");
                return 0;
            }
            return 1;
        } else if (std::strcmp(argv[i], "--diff-wom") == 0 && i + 2 < argc) {
            // Structural compare of two WOM models. Useful for verifying
            // that a --migrate-wom or round-trip through OBJ/glTF/STL
            // preserved the right counts. Compares sizes only — point-
            // wise vertex compare would be O(n²) and brittle to minor
            // float diffs from format conversions.
            std::string aBase = argv[++i];
            std::string bBase = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            for (auto* base : {&aBase, &bBase}) {
                if (base->size() >= 4 &&
                    base->substr(base->size() - 4) == ".wom") {
                    *base = base->substr(0, base->size() - 4);
                }
            }
            for (const auto& base : {aBase, bBase}) {
                if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                    std::fprintf(stderr,
                        "diff-wom: WOM not found: %s.wom\n", base.c_str());
                    return 1;
                }
            }
            auto a = wowee::pipeline::WoweeModelLoader::load(aBase);
            auto b = wowee::pipeline::WoweeModelLoader::load(bBase);
            // Each row is (label, a-value, b-value) so the table renders
            // straight.
            struct Row {
                const char* label;
                long long av, bv;
            };
            std::vector<Row> rows = {
                {"version",       a.version,                 b.version},
                {"vertices",  (long long)a.vertices.size(),  (long long)b.vertices.size()},
                {"indices",   (long long)a.indices.size(),   (long long)b.indices.size()},
                {"triangles", (long long)(a.indices.size()/3),(long long)(b.indices.size()/3)},
                {"textures",  (long long)a.texturePaths.size(),(long long)b.texturePaths.size()},
                {"bones",     (long long)a.bones.size(),     (long long)b.bones.size()},
                {"animations",(long long)a.animations.size(),(long long)b.animations.size()},
                {"batches",   (long long)a.batches.size(),   (long long)b.batches.size()},
            };
            // Bounds compare with float epsilon since round-trips through
            // text formats can perturb the last bit. 0.01-unit slop is
            // generous (positions are typically in yards, ~1m).
            auto closeBounds = [](const glm::vec3& x, const glm::vec3& y) {
                return std::abs(x.x - y.x) < 0.01f &&
                       std::abs(x.y - y.y) < 0.01f &&
                       std::abs(x.z - y.z) < 0.01f;
            };
            bool boundsMatch = closeBounds(a.boundMin, b.boundMin) &&
                                closeBounds(a.boundMax, b.boundMax) &&
                                std::abs(a.boundRadius - b.boundRadius) < 0.01f;
            int diffs = 0;
            for (const auto& r : rows) if (r.av != r.bv) diffs++;
            if (!boundsMatch) diffs++;
            bool nameMatch = (a.name == b.name);
            if (!nameMatch) diffs++;
            if (jsonOut) {
                nlohmann::json j;
                j["a"] = aBase + ".wom";
                j["b"] = bBase + ".wom";
                for (const auto& r : rows) {
                    j[r.label] = {{"a", r.av}, {"b", r.bv}};
                }
                j["name"] = {{"a", a.name}, {"b", b.name}};
                j["boundsMatch"] = boundsMatch;
                j["totalDiffs"] = diffs;
                j["identical"] = (diffs == 0);
                std::printf("%s\n", j.dump(2).c_str());
                return diffs == 0 ? 0 : 1;
            }
            std::printf("Diff: %s.wom vs %s.wom\n", aBase.c_str(), bBase.c_str());
            std::printf("                       a              b\n");
            for (const auto& r : rows) {
                std::printf("  %-12s: %12lld %12lld  %s\n",
                            r.label, r.av, r.bv,
                            r.av == r.bv ? "" : "DIFF");
            }
            std::printf("  %-12s: %-13s %-13s  %s\n",
                        "name",
                        a.name.substr(0, 13).c_str(),
                        b.name.substr(0, 13).c_str(),
                        nameMatch ? "" : "DIFF");
            std::printf("  %-12s: %s\n", "bounds",
                        boundsMatch ? "match" : "DIFF");
            if (diffs == 0) {
                std::printf("  IDENTICAL\n");
                return 0;
            }
            return 1;
        } else if (std::strcmp(argv[i], "--diff-wob") == 0 && i + 2 < argc) {
            // Companion to --diff-wom for buildings. Same shape: count-
            // based compare so round-trips through OBJ/glTF can be
            // validated without false positives from float perturbation.
            std::string aBase = argv[++i];
            std::string bBase = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            for (auto* base : {&aBase, &bBase}) {
                if (base->size() >= 4 &&
                    base->substr(base->size() - 4) == ".wob") {
                    *base = base->substr(0, base->size() - 4);
                }
            }
            for (const auto& base : {aBase, bBase}) {
                if (!wowee::pipeline::WoweeBuildingLoader::exists(base)) {
                    std::fprintf(stderr,
                        "diff-wob: WOB not found: %s.wob\n", base.c_str());
                    return 1;
                }
            }
            auto a = wowee::pipeline::WoweeBuildingLoader::load(aBase);
            auto b = wowee::pipeline::WoweeBuildingLoader::load(bBase);
            // Aggregate vertex+index counts across all groups for the
            // headline 'totalVerts/totalTris' metric (matches what
            // --info-wob reports).
            auto sumGroupVerts = [](const auto& bld) {
                size_t s = 0;
                for (const auto& g : bld.groups) s += g.vertices.size();
                return s;
            };
            auto sumGroupIdx = [](const auto& bld) {
                size_t s = 0;
                for (const auto& g : bld.groups) s += g.indices.size();
                return s;
            };
            struct Row {
                const char* label;
                long long av, bv;
            };
            // WoweeBuilding doesn't have a top-level textures vector or
            // doodadSets — materials and textures are per-group, doodad
            // sets are flattened. Aggregate the per-group counts.
            long long aMats = 0, bMats = 0;
            long long aGroupTex = 0, bGroupTex = 0;
            for (const auto& g : a.groups) {
                aMats += static_cast<long long>(g.materials.size());
                aGroupTex += static_cast<long long>(g.texturePaths.size());
            }
            for (const auto& g : b.groups) {
                bMats += static_cast<long long>(g.materials.size());
                bGroupTex += static_cast<long long>(g.texturePaths.size());
            }
            std::vector<Row> rows = {
                {"groups",      (long long)a.groups.size(),   (long long)b.groups.size()},
                {"portals",     (long long)a.portals.size(),  (long long)b.portals.size()},
                {"doodads",     (long long)a.doodads.size(),  (long long)b.doodads.size()},
                {"materials",   aMats, bMats},
                {"groupTex",    aGroupTex, bGroupTex},
                {"totalVerts",  (long long)sumGroupVerts(a),  (long long)sumGroupVerts(b)},
                {"totalIdx",    (long long)sumGroupIdx(a),    (long long)sumGroupIdx(b)},
            };
            int diffs = 0;
            for (const auto& r : rows) if (r.av != r.bv) diffs++;
            bool nameMatch = (a.name == b.name);
            if (!nameMatch) diffs++;
            bool radMatch = (std::abs(a.boundRadius - b.boundRadius) < 0.01f);
            if (!radMatch) diffs++;
            if (jsonOut) {
                nlohmann::json j;
                j["a"] = aBase + ".wob";
                j["b"] = bBase + ".wob";
                for (const auto& r : rows) {
                    j[r.label] = {{"a", r.av}, {"b", r.bv}};
                }
                j["name"] = {{"a", a.name}, {"b", b.name}};
                j["boundRadiusMatch"] = radMatch;
                j["totalDiffs"] = diffs;
                j["identical"] = (diffs == 0);
                std::printf("%s\n", j.dump(2).c_str());
                return diffs == 0 ? 0 : 1;
            }
            std::printf("Diff: %s.wob vs %s.wob\n", aBase.c_str(), bBase.c_str());
            std::printf("                       a              b\n");
            for (const auto& r : rows) {
                std::printf("  %-12s: %12lld %12lld  %s\n",
                            r.label, r.av, r.bv,
                            r.av == r.bv ? "" : "DIFF");
            }
            std::printf("  %-12s: %-13s %-13s  %s\n",
                        "name", a.name.substr(0, 13).c_str(),
                        b.name.substr(0, 13).c_str(),
                        nameMatch ? "" : "DIFF");
            std::printf("  %-12s: %s\n", "boundRadius",
                        radMatch ? "match" : "DIFF");
            if (diffs == 0) {
                std::printf("  IDENTICAL\n");
                return 0;
            }
            return 1;
        } else if (std::strcmp(argv[i], "--diff-whm") == 0 && i + 2 < argc) {
            // Terrain diff. Catches the common "did my edit actually
            // change anything?" question for heightmap tweaks. Compares
            // chunk presence + height range + placement counts; not
            // pointwise height compare since float perturbation from
            // round-trips would false-flag.
            std::string aBase = argv[++i];
            std::string bBase = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            for (auto* base : {&aBase, &bBase}) {
                for (const char* ext : {".wot", ".whm"}) {
                    if (base->size() >= 4 && base->substr(base->size() - 4) == ext) {
                        *base = base->substr(0, base->size() - 4);
                        break;
                    }
                }
            }
            for (const auto& base : {aBase, bBase}) {
                if (!wowee::pipeline::WoweeTerrainLoader::exists(base)) {
                    std::fprintf(stderr,
                        "diff-whm: WHM/WOT not found: %s.{whm,wot}\n", base.c_str());
                    return 1;
                }
            }
            wowee::pipeline::ADTTerrain a, b;
            wowee::pipeline::WoweeTerrainLoader::load(aBase, a);
            wowee::pipeline::WoweeTerrainLoader::load(bBase, b);
            // Per-side height range walk — same as --info-whm.
            auto stats = [](const wowee::pipeline::ADTTerrain& t) {
                struct S { int loaded; float minH, maxH; } s{0, 1e30f, -1e30f};
                for (const auto& c : t.chunks) {
                    if (!c.heightMap.isLoaded()) continue;
                    s.loaded++;
                    for (float h : c.heightMap.heights) {
                        if (std::isfinite(h)) {
                            s.minH = std::min(s.minH, h);
                            s.maxH = std::max(s.maxH, h);
                        }
                    }
                }
                if (s.loaded == 0) { s.minH = 0; s.maxH = 0; }
                return s;
            };
            auto sa = stats(a);
            auto sb = stats(b);
            struct Row { const char* label; long long av, bv; };
            std::vector<Row> rows = {
                {"loadedChunks",  sa.loaded,                          sb.loaded},
                {"doodadPlace",   (long long)a.doodadPlacements.size(),(long long)b.doodadPlacements.size()},
                {"wmoPlace",      (long long)a.wmoPlacements.size(),  (long long)b.wmoPlacements.size()},
                {"textures",      (long long)a.textures.size(),       (long long)b.textures.size()},
                {"doodadNames",   (long long)a.doodadNames.size(),    (long long)b.doodadNames.size()},
                {"wmoNames",      (long long)a.wmoNames.size(),       (long long)b.wmoNames.size()},
            };
            int diffs = 0;
            for (const auto& r : rows) if (r.av != r.bv) diffs++;
            // Tile coords + height range comparison (epsilon for floats).
            bool tileMatch = (a.coord.x == b.coord.x && a.coord.y == b.coord.y);
            if (!tileMatch) diffs++;
            bool heightMatch = (std::abs(sa.minH - sb.minH) < 0.01f &&
                                 std::abs(sa.maxH - sb.maxH) < 0.01f);
            if (!heightMatch) diffs++;
            if (jsonOut) {
                nlohmann::json j;
                j["a"] = aBase; j["b"] = bBase;
                for (const auto& r : rows) {
                    j[r.label] = {{"a", r.av}, {"b", r.bv}};
                }
                j["tile"] = {{"a", {a.coord.x, a.coord.y}},
                              {"b", {b.coord.x, b.coord.y}}};
                j["heightRange"] = {{"a", {sa.minH, sa.maxH}},
                                     {"b", {sb.minH, sb.maxH}}};
                j["totalDiffs"] = diffs;
                j["identical"] = (diffs == 0);
                std::printf("%s\n", j.dump(2).c_str());
                return diffs == 0 ? 0 : 1;
            }
            std::printf("Diff: %s vs %s\n", aBase.c_str(), bBase.c_str());
            std::printf("                       a              b\n");
            std::printf("  %-13s: (%4d,%4d)    (%4d,%4d)  %s\n",
                        "tile", a.coord.x, a.coord.y, b.coord.x, b.coord.y,
                        tileMatch ? "" : "DIFF");
            for (const auto& r : rows) {
                std::printf("  %-13s: %12lld %12lld  %s\n",
                            r.label, r.av, r.bv,
                            r.av == r.bv ? "" : "DIFF");
            }
            std::printf("  %-13s: [%.2f,%.2f]  [%.2f,%.2f]  %s\n",
                        "heightRange", sa.minH, sa.maxH, sb.minH, sb.maxH,
                        heightMatch ? "" : "DIFF");
            if (diffs == 0) {
                std::printf("  IDENTICAL\n");
                return 0;
            }
            return 1;
        } else if (std::strcmp(argv[i], "--diff-woc") == 0 && i + 2 < argc) {
            // Collision-mesh diff. Confirms a --regen-collision pass
            // actually changed something (or didn't, when the heightmap
            // tweak was below the slope threshold).
            std::string aPath = argv[++i];
            std::string bPath = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            for (const auto& p : {aPath, bPath}) {
                if (!std::filesystem::exists(p)) {
                    std::fprintf(stderr, "diff-woc: WOC not found: %s\n", p.c_str());
                    return 1;
                }
            }
            auto a = wowee::pipeline::WoweeCollisionBuilder::load(aPath);
            auto b = wowee::pipeline::WoweeCollisionBuilder::load(bPath);
            struct Row { const char* label; long long av, bv; };
            std::vector<Row> rows = {
                {"triangles", (long long)a.triangles.size(), (long long)b.triangles.size()},
                {"walkable",  (long long)a.walkableCount(),   (long long)b.walkableCount()},
                {"steep",     (long long)a.steepCount(),      (long long)b.steepCount()},
            };
            int diffs = 0;
            for (const auto& r : rows) if (r.av != r.bv) diffs++;
            bool tileMatch = (a.tileX == b.tileX && a.tileY == b.tileY);
            if (!tileMatch) diffs++;
            if (jsonOut) {
                nlohmann::json j;
                j["a"] = aPath; j["b"] = bPath;
                for (const auto& r : rows) {
                    j[r.label] = {{"a", r.av}, {"b", r.bv}};
                }
                j["tile"] = {{"a", {a.tileX, a.tileY}},
                              {"b", {b.tileX, b.tileY}}};
                j["totalDiffs"] = diffs;
                j["identical"] = (diffs == 0);
                std::printf("%s\n", j.dump(2).c_str());
                return diffs == 0 ? 0 : 1;
            }
            std::printf("Diff: %s vs %s\n", aPath.c_str(), bPath.c_str());
            std::printf("                       a              b\n");
            std::printf("  %-12s: (%4u,%4u)    (%4u,%4u)  %s\n",
                        "tile", a.tileX, a.tileY, b.tileX, b.tileY,
                        tileMatch ? "" : "DIFF");
            for (const auto& r : rows) {
                std::printf("  %-12s: %12lld %12lld  %s\n",
                            r.label, r.av, r.bv,
                            r.av == r.bv ? "" : "DIFF");
            }
            if (diffs == 0) {
                std::printf("  IDENTICAL\n");
                return 0;
            }
            return 1;
        } else if (std::strcmp(argv[i], "--diff-jsondbc") == 0 && i + 2 < argc) {
            // JSON DBC structural diff. Catches schema regressions when
            // a sidecar is regenerated by a different tool version (or
            // a different DBC layout produces a different field count).
            std::string aPath = argv[++i];
            std::string bPath = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            auto loadDoc = [](const std::string& p, nlohmann::json& doc) {
                std::ifstream in(p);
                if (!in) return false;
                try { in >> doc; } catch (...) { return false; }
                return true;
            };
            nlohmann::json a, b;
            if (!loadDoc(aPath, a)) {
                std::fprintf(stderr, "diff-jsondbc: failed to read %s\n", aPath.c_str());
                return 1;
            }
            if (!loadDoc(bPath, b)) {
                std::fprintf(stderr, "diff-jsondbc: failed to read %s\n", bPath.c_str());
                return 1;
            }
            // Pull comparable fields with safe defaults.
            std::string aFmt = a.value("format", std::string{});
            std::string bFmt = b.value("format", std::string{});
            std::string aSrc = a.value("source", std::string{});
            std::string bSrc = b.value("source", std::string{});
            uint32_t aRC = a.value("recordCount", 0u);
            uint32_t bRC = b.value("recordCount", 0u);
            uint32_t aFC = a.value("fieldCount", 0u);
            uint32_t bFC = b.value("fieldCount", 0u);
            uint32_t aActual = (a.contains("records") && a["records"].is_array())
                ? static_cast<uint32_t>(a["records"].size()) : 0u;
            uint32_t bActual = (b.contains("records") && b["records"].is_array())
                ? static_cast<uint32_t>(b["records"].size()) : 0u;
            int diffs = 0;
            if (aFmt != bFmt) diffs++;
            if (aSrc != bSrc) diffs++;
            if (aRC != bRC) diffs++;
            if (aFC != bFC) diffs++;
            if (aActual != bActual) diffs++;
            if (jsonOut) {
                nlohmann::json j;
                j["a"] = aPath; j["b"] = bPath;
                j["format"]      = {{"a", aFmt},     {"b", bFmt}};
                j["source"]      = {{"a", aSrc},     {"b", bSrc}};
                j["recordCount"] = {{"a", aRC},      {"b", bRC}};
                j["fieldCount"]  = {{"a", aFC},      {"b", bFC}};
                j["actualRecs"]  = {{"a", aActual},  {"b", bActual}};
                j["totalDiffs"] = diffs;
                j["identical"]  = (diffs == 0);
                std::printf("%s\n", j.dump(2).c_str());
                return diffs == 0 ? 0 : 1;
            }
            auto cmpStr = [](const char* lbl, const std::string& a, const std::string& b) {
                std::printf("  %-12s: %-20s %-20s  %s\n", lbl,
                            a.empty() ? "(unset)" : a.c_str(),
                            b.empty() ? "(unset)" : b.c_str(),
                            a == b ? "" : "DIFF");
            };
            auto cmpNum = [](const char* lbl, uint32_t a, uint32_t b) {
                std::printf("  %-12s: %20u %20u  %s\n", lbl, a, b,
                            a == b ? "" : "DIFF");
            };
            std::printf("Diff: %s vs %s\n", aPath.c_str(), bPath.c_str());
            cmpStr("format",      aFmt, bFmt);
            cmpStr("source",      aSrc, bSrc);
            cmpNum("recordCount", aRC, bRC);
            cmpNum("fieldCount",  aFC, bFC);
            cmpNum("actualRecs",  aActual, bActual);
            if (diffs == 0) {
                std::printf("  IDENTICAL\n");
                return 0;
            }
            return 1;
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
        } else if (std::strcmp(argv[i], "--info-zone-tree") == 0 && i + 1 < argc) {
            // Pretty `tree`-style hierarchical view of a zone's contents.
            // Designed for at-a-glance comprehension — what creatures,
            // what objects, what quests, what tiles, what files. No
            // --json flag because the structured equivalent is just
            // running --info-* per category and concatenating.
            std::string zoneDir = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "info-zone-tree: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "info-zone-tree: parse failed\n");
                return 1;
            }
            wowee::editor::NpcSpawner sp;
            sp.loadFromFile(zoneDir + "/creatures.json");
            wowee::editor::ObjectPlacer op;
            op.loadFromFile(zoneDir + "/objects.json");
            wowee::editor::QuestEditor qe;
            qe.loadFromFile(zoneDir + "/quests.json");
            // Walk on-disk files for the 'Files' branch.
            std::vector<std::string> diskFiles;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(zoneDir, ec)) {
                if (e.is_regular_file()) {
                    diskFiles.push_back(e.path().filename().string());
                }
            }
            std::sort(diskFiles.begin(), diskFiles.end());
            // Tree-drawing helpers — Unix box characters since most
            // terminals support UTF-8 by default. Pre-compute prefix
            // strings so leaf vs branch alignment looks right.
            auto branch = [](bool last) { return last ? "└─ " : "├─ "; };
            auto cont   = [](bool last) { return last ? "   " : "│  "; };
            std::printf("%s/\n",
                        zm.displayName.empty() ? zm.mapName.c_str()
                                                : zm.displayName.c_str());
            // Manifest section
            std::printf("├─ Manifest\n");
            std::printf("│  ├─ mapName     : %s\n", zm.mapName.c_str());
            std::printf("│  ├─ mapId       : %u\n", zm.mapId);
            std::printf("│  ├─ baseHeight  : %.1f\n", zm.baseHeight);
            std::printf("│  ├─ biome       : %s\n",
                        zm.biome.empty() ? "(unset)" : zm.biome.c_str());
            std::printf("│  └─ flags       : %s%s%s%s\n",
                        zm.allowFlying ? "fly " : "",
                        zm.pvpEnabled  ? "pvp " : "",
                        zm.isIndoor    ? "indoor " : "",
                        zm.isSanctuary ? "sanctuary " : "");
            // Tiles
            std::printf("├─ Tiles (%zu)\n", zm.tiles.size());
            for (size_t k = 0; k < zm.tiles.size(); ++k) {
                bool last = (k == zm.tiles.size() - 1);
                std::printf("│  %s(%d, %d)\n", branch(last),
                            zm.tiles[k].first, zm.tiles[k].second);
            }
            // Creatures
            std::printf("├─ Creatures (%zu)\n", sp.spawnCount());
            for (size_t k = 0; k < sp.spawnCount(); ++k) {
                bool last = (k == sp.spawnCount() - 1);
                const auto& s = sp.getSpawns()[k];
                std::printf("│  %slvl %u  %s%s\n",
                            branch(last), s.level, s.name.c_str(),
                            s.hostile ? " [hostile]" : "");
            }
            // Objects
            std::printf("├─ Objects (%zu)\n", op.getObjects().size());
            for (size_t k = 0; k < op.getObjects().size(); ++k) {
                bool last = (k == op.getObjects().size() - 1);
                const auto& o = op.getObjects()[k];
                std::printf("│  %s%s  %s\n", branch(last),
                            o.type == wowee::editor::PlaceableType::M2 ? "m2 " : "wmo",
                            o.path.c_str());
            }
            // Quests with sub-tree of objectives
            std::printf("├─ Quests (%zu)\n", qe.questCount());
            using OT = wowee::editor::QuestObjectiveType;
            auto typeName = [](OT t) {
                switch (t) {
                    case OT::KillCreature: return "kill";
                    case OT::CollectItem:  return "collect";
                    case OT::TalkToNPC:    return "talk";
                    case OT::ExploreArea:  return "explore";
                    case OT::EscortNPC:    return "escort";
                    case OT::UseObject:    return "use";
                }
                return "?";
            };
            for (size_t k = 0; k < qe.questCount(); ++k) {
                bool lastQ = (k == qe.questCount() - 1);
                const auto& q = qe.getQuests()[k];
                std::printf("│  %s[%u] %s (lvl %u, %u XP)\n",
                            branch(lastQ), q.id, q.title.c_str(),
                            q.requiredLevel, q.reward.xp);
                // Objectives indented under the quest. Use 'cont' for
                // the prior column so vertical bars align.
                for (size_t o = 0; o < q.objectives.size(); ++o) {
                    bool lastO = (o == q.objectives.size() - 1 &&
                                  q.reward.itemRewards.empty());
                    const auto& obj = q.objectives[o];
                    std::printf("│  %s%s%s ×%u %s\n",
                                cont(lastQ), branch(lastO),
                                typeName(obj.type), obj.targetCount,
                                obj.targetName.c_str());
                }
                for (size_t r = 0; r < q.reward.itemRewards.size(); ++r) {
                    bool lastR = (r == q.reward.itemRewards.size() - 1);
                    std::printf("│  %s%sreward: %s\n",
                                cont(lastQ), branch(lastR),
                                q.reward.itemRewards[r].c_str());
                }
            }
            // Files (last top-level branch — uses └─)
            std::printf("└─ Files (%zu)\n", diskFiles.size());
            for (size_t k = 0; k < diskFiles.size(); ++k) {
                bool last = (k == diskFiles.size() - 1);
                std::printf("   %s%s\n", branch(last), diskFiles[k].c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--export-zone-summary-md") == 0 && i + 1 < argc) {
            // Render a Markdown documentation page for a zone. Useful for
            // designers tracking changes between versions, generating
            // GitHub Pages docs, or reviewing zones in PRs without
            // round-tripping through the GUI.
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "export-zone-summary-md: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr,
                    "export-zone-summary-md: failed to parse %s\n", manifestPath.c_str());
                return 1;
            }
            // Default output: ZONE.md sitting next to zone.json.
            if (outPath.empty()) outPath = zoneDir + "/ZONE.md";
            // Load content sub-files; missing ones contribute 0 entries.
            wowee::editor::NpcSpawner sp;
            sp.loadFromFile(zoneDir + "/creatures.json");
            wowee::editor::ObjectPlacer op;
            op.loadFromFile(zoneDir + "/objects.json");
            wowee::editor::QuestEditor qe;
            qe.loadFromFile(zoneDir + "/quests.json");
            std::ofstream md(outPath);
            if (!md) {
                std::fprintf(stderr,
                    "export-zone-summary-md: cannot write %s\n", outPath.c_str());
                return 1;
            }
            md << "# " << (zm.displayName.empty() ? zm.mapName : zm.displayName) << "\n\n";
            md << "*Auto-generated by `wowee_editor --export-zone-summary-md`. "
                  "Do not edit by hand.*\n\n";
            md << "## Manifest\n\n";
            md << "| Field | Value |\n";
            md << "|---|---|\n";
            md << "| Map name | `" << zm.mapName << "` |\n";
            md << "| Display name | " << zm.displayName << " |\n";
            md << "| Map ID | " << zm.mapId << " |\n";
            if (!zm.biome.empty())     md << "| Biome | " << zm.biome << " |\n";
            md << "| Base height | " << zm.baseHeight << " |\n";
            md << "| Tile count | " << zm.tiles.size() << " |\n";
            md << "| Allow flying | " << (zm.allowFlying ? "yes" : "no") << " |\n";
            md << "| PvP enabled | " << (zm.pvpEnabled ? "yes" : "no") << " |\n";
            md << "| Indoor | " << (zm.isIndoor ? "yes" : "no") << " |\n";
            md << "| Sanctuary | " << (zm.isSanctuary ? "yes" : "no") << " |\n";
            if (!zm.musicTrack.empty())   md << "| Music | `" << zm.musicTrack << "` |\n";
            if (!zm.ambienceDay.empty())  md << "| Ambient (day) | `" << zm.ambienceDay << "` |\n";
            if (!zm.ambienceNight.empty())md << "| Ambient (night) | `" << zm.ambienceNight << "` |\n";
            if (!zm.description.empty()) {
                md << "\n### Description\n\n" << zm.description << "\n";
            }
            md << "\n## Tiles\n\n";
            md << "| tx | ty |\n|---|---|\n";
            for (const auto& [tx, ty] : zm.tiles) {
                md << "| " << tx << " | " << ty << " |\n";
            }
            md << "\n## Creatures (" << sp.spawnCount() << ")\n\n";
            if (sp.spawnCount() == 0) {
                md << "*No creature spawns.*\n";
            } else {
                md << "| # | Name | Lvl | DisplayId | Pos (x, y, z) | Flags |\n";
                md << "|---|---|---|---|---|---|\n";
                for (size_t k = 0; k < sp.spawnCount(); ++k) {
                    const auto& s = sp.getSpawns()[k];
                    md << "| " << k << " | " << s.name << " | " << s.level << " | "
                       << s.displayId << " | ("
                       << s.position.x << ", " << s.position.y << ", " << s.position.z
                       << ") |";
                    if (s.hostile)    md << " hostile";
                    if (s.questgiver) md << " quest";
                    if (s.vendor)     md << " vendor";
                    if (s.trainer)    md << " trainer";
                    md << " |\n";
                }
            }
            md << "\n## Objects (" << op.getObjects().size() << ")\n\n";
            if (op.getObjects().empty()) {
                md << "*No object placements.*\n";
            } else {
                md << "| # | Type | Path | Pos | Scale |\n";
                md << "|---|---|---|---|---|\n";
                for (size_t k = 0; k < op.getObjects().size(); ++k) {
                    const auto& o = op.getObjects()[k];
                    md << "| " << k << " | "
                       << (o.type == wowee::editor::PlaceableType::M2 ? "m2" : "wmo")
                       << " | `" << o.path << "` | ("
                       << o.position.x << ", " << o.position.y << ", " << o.position.z
                       << ") | " << o.scale << " |\n";
                }
            }
            md << "\n## Quests (" << qe.questCount() << ")\n\n";
            if (qe.questCount() == 0) {
                md << "*No quests.*\n";
            } else {
                using OT = wowee::editor::QuestObjectiveType;
                auto typeName = [](OT t) {
                    switch (t) {
                        case OT::KillCreature: return "kill";
                        case OT::CollectItem:  return "collect";
                        case OT::TalkToNPC:    return "talk";
                        case OT::ExploreArea:  return "explore";
                        case OT::EscortNPC:    return "escort";
                        case OT::UseObject:    return "use";
                    }
                    return "?";
                };
                for (size_t k = 0; k < qe.questCount(); ++k) {
                    const auto& q = qe.getQuests()[k];
                    md << "### " << k << ". " << q.title << "\n\n";
                    md << "- Required level: " << q.requiredLevel << "\n";
                    md << "- Quest giver NPC ID: " << q.questGiverNpcId << "\n";
                    md << "- Turn-in NPC ID: " << q.turnInNpcId << "\n";
                    md << "- XP: " << q.reward.xp << "\n";
                    if (q.reward.gold || q.reward.silver || q.reward.copper) {
                        md << "- Coin: " << q.reward.gold << "g "
                           << q.reward.silver << "s " << q.reward.copper << "c\n";
                    }
                    if (!q.objectives.empty()) {
                        md << "- Objectives:\n";
                        for (const auto& obj : q.objectives) {
                            md << "  - **" << typeName(obj.type) << "** "
                               << obj.targetName << " ×" << obj.targetCount;
                            if (!obj.description.empty()) {
                                md << " — *" << obj.description << "*";
                            }
                            md << "\n";
                        }
                    }
                    if (!q.reward.itemRewards.empty()) {
                        md << "- Item rewards:\n";
                        for (const auto& it : q.reward.itemRewards) {
                            md << "  - `" << it << "`\n";
                        }
                    }
                    md << "\n";
                }
            }
            md.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  zone=%s, %zu tiles, %zu creatures, %zu objects, %zu quests\n",
                        zm.mapName.c_str(), zm.tiles.size(), sp.spawnCount(),
                        op.getObjects().size(), qe.questCount());
            return 0;
        } else if (std::strcmp(argv[i], "--export-zone-csv") == 0 && i + 1 < argc) {
            // Emit creatures.csv / objects.csv / quests.csv for designers
            // who prefer spreadsheets over JSON. Round-trip back into the
            // editor isn't supported yet, but for read-only analysis (sort
            // by XP, group by faction, pivot tables in LibreOffice) CSV is
            // the lingua franca of design data.
            std::string zoneDir = argv[++i];
            std::string outDir;
            if (i + 1 < argc && argv[i + 1][0] != '-') outDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "export-zone-csv: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            if (outDir.empty()) outDir = zoneDir;
            // CSV-escape: wrap any field containing comma/quote/newline in
            // double quotes; double up internal quotes per RFC 4180.
            auto csvEsc = [](const std::string& s) {
                bool needs = s.find(',') != std::string::npos ||
                             s.find('"') != std::string::npos ||
                             s.find('\n') != std::string::npos;
                if (!needs) return s;
                std::string out = "\"";
                for (char c : s) {
                    if (c == '"') out += "\"\"";
                    else out += c;
                }
                out += "\"";
                return out;
            };
            int filesWritten = 0;
            // Creatures
            wowee::editor::NpcSpawner sp;
            if (sp.loadFromFile(zoneDir + "/creatures.json")) {
                std::string out = outDir + "/creatures.csv";
                std::ofstream f(out);
                if (!f) {
                    std::fprintf(stderr, "cannot write %s\n", out.c_str());
                    return 1;
                }
                f << "index,id,name,displayId,level,health,mana,faction,"
                     "x,y,z,orientation,scale,hostile,questgiver,vendor,trainer\n";
                for (size_t k = 0; k < sp.spawnCount(); ++k) {
                    const auto& s = sp.getSpawns()[k];
                    f << k << "," << s.id << "," << csvEsc(s.name) << ","
                      << s.displayId << "," << s.level << ","
                      << s.health << "," << s.mana << "," << s.faction << ","
                      << s.position.x << "," << s.position.y << ","
                      << s.position.z << "," << s.orientation << ","
                      << s.scale << ","
                      << (s.hostile ? 1 : 0) << ","
                      << (s.questgiver ? 1 : 0) << ","
                      << (s.vendor ? 1 : 0) << ","
                      << (s.trainer ? 1 : 0) << "\n";
                }
                std::printf("  wrote %s (%zu rows)\n", out.c_str(), sp.spawnCount());
                filesWritten++;
            }
            // Objects
            wowee::editor::ObjectPlacer op;
            if (op.loadFromFile(zoneDir + "/objects.json")) {
                std::string out = outDir + "/objects.csv";
                std::ofstream f(out);
                if (!f) return 1;
                f << "index,type,path,x,y,z,rotX,rotY,rotZ,scale\n";
                for (size_t k = 0; k < op.getObjects().size(); ++k) {
                    const auto& o = op.getObjects()[k];
                    f << k << ","
                      << (o.type == wowee::editor::PlaceableType::M2 ? "m2" : "wmo") << ","
                      << csvEsc(o.path) << ","
                      << o.position.x << "," << o.position.y << "," << o.position.z << ","
                      << o.rotation.x << "," << o.rotation.y << "," << o.rotation.z << ","
                      << o.scale << "\n";
                }
                std::printf("  wrote %s (%zu rows)\n", out.c_str(),
                            op.getObjects().size());
                filesWritten++;
            }
            // Quests — flatten to one row per quest. Objectives + items
            // are joined into a single semicolon-separated cell so the
            // CSV stays one-row-per-quest (designer-friendly for sorting).
            wowee::editor::QuestEditor qe;
            if (qe.loadFromFile(zoneDir + "/quests.json")) {
                std::string out = outDir + "/quests.csv";
                std::ofstream f(out);
                if (!f) return 1;
                f << "index,id,title,requiredLevel,giverNpcId,turnInNpcId,"
                     "xp,gold,silver,copper,nextQuestId,objectiveCount,"
                     "objectives,itemRewards\n";
                using OT = wowee::editor::QuestObjectiveType;
                auto typeName = [](OT t) {
                    switch (t) {
                        case OT::KillCreature: return "kill";
                        case OT::CollectItem:  return "collect";
                        case OT::TalkToNPC:    return "talk";
                        case OT::ExploreArea:  return "explore";
                        case OT::EscortNPC:    return "escort";
                        case OT::UseObject:    return "use";
                    }
                    return "?";
                };
                for (size_t k = 0; k < qe.questCount(); ++k) {
                    const auto& q = qe.getQuests()[k];
                    std::string objs;
                    for (size_t o = 0; o < q.objectives.size(); ++o) {
                        if (o) objs += "; ";
                        objs += std::string(typeName(q.objectives[o].type)) + ":" +
                                q.objectives[o].targetName + "x" +
                                std::to_string(q.objectives[o].targetCount);
                    }
                    std::string items;
                    for (size_t r = 0; r < q.reward.itemRewards.size(); ++r) {
                        if (r) items += "; ";
                        items += q.reward.itemRewards[r];
                    }
                    f << k << "," << q.id << "," << csvEsc(q.title) << ","
                      << q.requiredLevel << ","
                      << q.questGiverNpcId << "," << q.turnInNpcId << ","
                      << q.reward.xp << "," << q.reward.gold << ","
                      << q.reward.silver << "," << q.reward.copper << ","
                      << q.nextQuestId << ","
                      << q.objectives.size() << ","
                      << csvEsc(objs) << "," << csvEsc(items) << "\n";
                }
                std::printf("  wrote %s (%zu rows)\n", out.c_str(), qe.questCount());
                filesWritten++;
            }
            if (filesWritten == 0) {
                std::fprintf(stderr,
                    "export-zone-csv: zone has no creatures/objects/quests to emit\n");
                return 1;
            }
            std::printf("Exported %d CSV file(s) to %s\n", filesWritten, outDir.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--export-zone-html") == 0 && i + 1 < argc) {
            // Generate a single-file HTML viewer next to the zone .glb.
            // Anyone with a modern browser can open it — no installs, no
            // CDN-mining the user's network. Uses model-viewer (Google's
            // web component) bundled from the unpkg CDN since it's
            // standards-based and doesn't require a build step.
            //
            // Usage flow:
            //   wowee_editor --bake-zone-glb custom_zones/MyZone
            //   wowee_editor --export-zone-html custom_zones/MyZone
            //   open custom_zones/MyZone/MyZone.html  # opens in browser
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "export-zone-html: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "export-zone-html: parse failed\n");
                return 1;
            }
            std::string glbName = zm.mapName + ".glb";
            std::string glbPath = zoneDir + "/" + glbName;
            if (!fs::exists(glbPath)) {
                std::fprintf(stderr,
                    "export-zone-html: %s does not exist — run --bake-zone-glb first\n",
                    glbPath.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/" + zm.mapName + ".html";
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "export-zone-html: cannot write %s\n", outPath.c_str());
                return 1;
            }
            // Compute relative path from html file's parent dir to the
            // .glb so the viewer loads it. Default same-dir → just basename.
            std::string glbHref = glbName;
            // If outPath is in a different dir than the .glb, the user is
            // responsible for moving things; leaving glbHref as the
            // basename is a sensible default that fails loudly in the
            // browser console rather than producing a wrong-but-silent
            // page.
            std::string title = zm.displayName.empty()
                ? zm.mapName : zm.displayName;
            // Single-file template with model-viewer. The version pin
            // (^4.0.0) keeps the page from breaking when the unpkg
            // 'latest' silently bumps a major version.
            out << "<!doctype html>\n"
                   "<html lang=\"en\">\n"
                   "<head>\n"
                   "  <meta charset=\"utf-8\">\n"
                   "  <title>" << title << " — Wowee Zone Viewer</title>\n"
                   "  <script type=\"module\" "
                       "src=\"https://unpkg.com/@google/model-viewer@^4.0.0/dist/model-viewer.min.js\">"
                   "</script>\n"
                   "  <style>\n"
                   "    body { margin:0; font-family: sans-serif; background:#1a1a1a; color:#eee; }\n"
                   "    header { padding:12px 20px; background:#2a2a2a; border-bottom:1px solid #444; }\n"
                   "    h1 { margin:0; font-size:18px; font-weight:500; }\n"
                   "    .meta { color:#aaa; font-size:13px; margin-top:4px; }\n"
                   "    model-viewer { width:100vw; height:calc(100vh - 60px); background:#1a1a1a; }\n"
                   "    .footer { position:fixed; bottom:8px; right:12px; color:#666; font-size:11px; }\n"
                   "  </style>\n"
                   "</head>\n"
                   "<body>\n"
                   "  <header>\n"
                   "    <h1>" << title << "</h1>\n"
                   "    <div class=\"meta\">Map: <code>" << zm.mapName
                << "</code> · Tiles: " << zm.tiles.size()
                << " · MapId: " << zm.mapId << "</div>\n"
                   "  </header>\n"
                   "  <model-viewer\n"
                   "    src=\"" << glbHref << "\"\n"
                   "    alt=\"" << title << " terrain\"\n"
                   "    camera-controls\n"
                   "    auto-rotate\n"
                   "    rotation-per-second=\"15deg\"\n"
                   "    shadow-intensity=\"1\"\n"
                   "    exposure=\"1.2\"\n"
                   "    environment-image=\"neutral\">\n"
                   "  </model-viewer>\n"
                   "  <div class=\"footer\">Generated by wowee_editor --export-zone-html</div>\n"
                   "</body>\n"
                   "</html>\n";
            out.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  references %s (must sit next to .html)\n", glbHref.c_str());
            std::printf("  open in any modern browser — no install required\n");
            return 0;
        } else if (std::strcmp(argv[i], "--export-quest-graph") == 0 && i + 1 < argc) {
            // Render quest chains as a Graphviz DOT graph. Visualizing
            // quest dependencies in plain text rapidly becomes unreadable
            // past ~10 quests; piping this through 'dot -Tpng -o q.png'
            // makes complex chains immediately legible.
            //
            //   wowee_editor --export-quest-graph custom_zones/MyZone
            //   dot -Tpng custom_zones/MyZone/quests.dot -o quests.png
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr,
                    "export-quest-graph: %s not found\n", path.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/quests.dot";
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr,
                    "export-quest-graph: failed to parse %s\n", path.c_str());
                return 1;
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "export-quest-graph: cannot write %s\n", outPath.c_str());
                return 1;
            }
            // DOT-escape strings (just quotes and backslashes) — quest
            // titles can include arbitrary punctuation that breaks DOT
            // parsing if not escaped.
            auto dotEsc = [](const std::string& s) {
                std::string out;
                for (char c : s) {
                    if (c == '"' || c == '\\') out += '\\';
                    out += c;
                }
                return out;
            };
            const auto& quests = qe.getQuests();
            // Build an index of valid quest IDs so dangling chain
            // pointers can be styled differently (red, dashed).
            std::unordered_set<uint32_t> validIds;
            for (const auto& q : quests) validIds.insert(q.id);
            out << "digraph QuestChains {\n";
            out << "  // Generated by wowee_editor --export-quest-graph\n";
            out << "  rankdir=LR;\n";
            out << "  node [shape=box, style=filled, fontname=\"sans-serif\"];\n";
            // Nodes: one per quest, colored by completion-readiness:
            //   green = has objectives + reward + valid NPCs
            //   yellow = missing some non-fatal field (description, etc.)
            //   gray = no objectives (won't actually complete in-game)
            for (const auto& q : quests) {
                bool hasObjs = !q.objectives.empty();
                bool hasReward = (q.reward.xp > 0 || !q.reward.itemRewards.empty());
                std::string color = hasObjs ? (hasReward ? "lightgreen" : "lightyellow")
                                             : "lightgray";
                std::string label = "[" + std::to_string(q.id) + "] " + dotEsc(q.title);
                if (q.requiredLevel > 1) {
                    label += "\\nlvl " + std::to_string(q.requiredLevel);
                }
                if (q.reward.xp > 0) {
                    label += "  " + std::to_string(q.reward.xp) + " XP";
                }
                out << "  q" << q.id << " [label=\"" << label
                    << "\", fillcolor=" << color << "];\n";
            }
            // Edges: quest -> nextQuestId. Style chain-pointers to
            // missing quests differently so they stand out visually.
            int chainEdges = 0, brokenEdges = 0;
            for (const auto& q : quests) {
                if (q.nextQuestId == 0) continue;
                if (validIds.count(q.nextQuestId) == 0) {
                    out << "  q" << q.id << " -> q" << q.nextQuestId
                        << " [color=red, style=dashed, label=\"missing\"];\n";
                    out << "  q" << q.nextQuestId
                        << " [label=\"<missing> [" << q.nextQuestId
                        << "]\", fillcolor=mistyrose, style=\"filled,dashed\"];\n";
                    brokenEdges++;
                } else {
                    out << "  q" << q.id << " -> q" << q.nextQuestId << ";\n";
                    chainEdges++;
                }
            }
            out << "}\n";
            out.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  %zu quests, %d chain edges, %d broken (red/dashed)\n",
                        quests.size(), chainEdges, brokenEdges);
            std::printf("  next: dot -Tpng %s -o quests.png\n", outPath.c_str());
            return 0;
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
        } else if ((std::strcmp(argv[i], "--validate-glb") == 0 ||
                    std::strcmp(argv[i], "--info-glb") == 0) && i + 1 < argc) {
            // Shared handler: --validate-glb errors out on broken structure;
            // --info-glb prints the same metadata but exits 0 unless the
            // file is unreadable. Same parser, different verdict policy.
            bool isValidate = (std::strcmp(argv[i], "--validate-glb") == 0);
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr,
                    "%s: cannot open %s\n",
                    isValidate ? "validate-glb" : "info-glb", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            std::vector<std::string> errors;
            // 12-byte header: 'glTF' magic, version=2, total length.
            uint32_t magic = 0, version = 0, totalLen = 0;
            if (bytes.size() < 12) {
                errors.push_back("file too short for glTF header (need 12 bytes)");
            } else {
                std::memcpy(&magic,    &bytes[0], 4);
                std::memcpy(&version,  &bytes[4], 4);
                std::memcpy(&totalLen, &bytes[8], 4);
                if (magic != 0x46546C67) {
                    errors.push_back("magic is not 'glTF' (0x46546C67)");
                }
                if (version != 2) {
                    errors.push_back("version " + std::to_string(version) +
                                     " not supported (only glTF 2.0)");
                }
                if (totalLen != bytes.size()) {
                    errors.push_back("totalLength=" + std::to_string(totalLen) +
                                     " != file size " + std::to_string(bytes.size()));
                }
            }
            // JSON chunk follows: 4-byte length, 4-byte type ('JSON'),
            // then payload. Then BIN chunk same shape.
            uint32_t jsonLen = 0, jsonType = 0;
            uint32_t binLen = 0, binType = 0;
            std::string jsonStr;
            std::vector<uint8_t> binData;
            if (errors.empty()) {
                if (bytes.size() < 20) {
                    errors.push_back("missing JSON chunk header");
                } else {
                    std::memcpy(&jsonLen, &bytes[12], 4);
                    std::memcpy(&jsonType, &bytes[16], 4);
                    if (jsonType != 0x4E4F534A) {
                        errors.push_back("first chunk type is not 'JSON' (0x4E4F534A)");
                    }
                    if (20 + jsonLen > bytes.size()) {
                        errors.push_back("JSON chunk extends past file end");
                    } else {
                        jsonStr.assign(bytes.begin() + 20,
                                        bytes.begin() + 20 + jsonLen);
                    }
                }
                size_t binOff = 20 + jsonLen;
                if (binOff + 8 <= bytes.size()) {
                    std::memcpy(&binLen, &bytes[binOff], 4);
                    std::memcpy(&binType, &bytes[binOff + 4], 4);
                    if (binType != 0x004E4942) {
                        errors.push_back("second chunk type is not 'BIN\\0' (0x004E4942)");
                    }
                    if (binOff + 8 + binLen > bytes.size()) {
                        errors.push_back("BIN chunk extends past file end");
                    } else {
                        binData.assign(bytes.begin() + binOff + 8,
                                        bytes.begin() + binOff + 8 + binLen);
                    }
                }
                // BIN chunk is optional in spec; only flag missing if
                // accessors below reference a buffer.
            }
            // Parse JSON and validate structure.
            nlohmann::json gj;
            int meshCount = 0, primitiveCount = 0, accessorCount = 0,
                bufferViewCount = 0, bufferCount = 0;
            std::string assetVersion;
            if (errors.empty() && !jsonStr.empty()) {
                try {
                    gj = nlohmann::json::parse(jsonStr);
                    assetVersion = gj.value("/asset/version"_json_pointer, std::string{});
                    if (assetVersion != "2.0") {
                        errors.push_back("asset.version is '" + assetVersion +
                                         "', not '2.0'");
                    }
                    if (gj.contains("meshes") && gj["meshes"].is_array()) {
                        meshCount = static_cast<int>(gj["meshes"].size());
                        for (const auto& m : gj["meshes"]) {
                            if (m.contains("primitives") && m["primitives"].is_array()) {
                                primitiveCount += static_cast<int>(m["primitives"].size());
                            }
                        }
                    }
                    if (gj.contains("accessors") && gj["accessors"].is_array()) {
                        accessorCount = static_cast<int>(gj["accessors"].size());
                        // Verify each accessor's bufferView exists.
                        for (size_t a = 0; a < gj["accessors"].size(); ++a) {
                            const auto& acc = gj["accessors"][a];
                            if (acc.contains("bufferView")) {
                                int bv = acc["bufferView"];
                                if (!gj.contains("bufferViews") ||
                                    bv >= static_cast<int>(gj["bufferViews"].size())) {
                                    errors.push_back("accessor " + std::to_string(a) +
                                                     " bufferView=" + std::to_string(bv) +
                                                     " out of range");
                                }
                            }
                        }
                    }
                    if (gj.contains("bufferViews") && gj["bufferViews"].is_array()) {
                        bufferViewCount = static_cast<int>(gj["bufferViews"].size());
                        for (size_t b = 0; b < gj["bufferViews"].size(); ++b) {
                            const auto& bv = gj["bufferViews"][b];
                            uint32_t bo = bv.value("byteOffset", 0u);
                            uint32_t bl = bv.value("byteLength", 0u);
                            uint64_t end = uint64_t(bo) + bl;
                            if (end > binLen) {
                                errors.push_back("bufferView " + std::to_string(b) +
                                                 " range [" + std::to_string(bo) +
                                                 ", " + std::to_string(end) +
                                                 ") past BIN chunk length " +
                                                 std::to_string(binLen));
                            }
                        }
                    }
                    if (gj.contains("buffers") && gj["buffers"].is_array()) {
                        bufferCount = static_cast<int>(gj["buffers"].size());
                    }
                } catch (const std::exception& e) {
                    errors.push_back(std::string("JSON parse error: ") + e.what());
                }
            }
            int errorCount = static_cast<int>(errors.size());
            if (jsonOut) {
                nlohmann::json j;
                j["glb"] = path;
                j["fileSize"] = bytes.size();
                j["version"] = version;
                j["assetVersion"] = assetVersion;
                j["totalLength"] = totalLen;
                j["jsonLength"] = jsonLen;
                j["binLength"] = binLen;
                j["meshes"] = meshCount;
                j["primitives"] = primitiveCount;
                j["accessors"] = accessorCount;
                j["bufferViews"] = bufferViewCount;
                j["buffers"] = bufferCount;
                j["errorCount"] = errorCount;
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return (isValidate && errorCount > 0) ? 1 : 0;
            }
            std::printf("GLB: %s\n", path.c_str());
            std::printf("  file bytes  : %zu\n", bytes.size());
            std::printf("  glTF version: %u (asset.version=%s)\n",
                        version, assetVersion.empty() ? "?" : assetVersion.c_str());
            std::printf("  totalLength : %u\n", totalLen);
            std::printf("  JSON chunk  : %u bytes\n", jsonLen);
            std::printf("  BIN chunk   : %u bytes\n", binLen);
            std::printf("  meshes      : %d (%d primitives)\n",
                        meshCount, primitiveCount);
            std::printf("  accessors   : %d  bufferViews: %d  buffers: %d\n",
                        accessorCount, bufferViewCount, bufferCount);
            if (errors.empty()) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %d error(s):\n", errorCount);
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return isValidate ? 1 : 0;
        } else if (std::strcmp(argv[i], "--info-glb-tree") == 0 && i + 1 < argc) {
            // Pretty `tree`-style view of glTF structure. --info-glb gives
            // counts; this shows the actual scene→node→mesh→primitive
            // hierarchy with names. Useful when debugging 'why is this
            // imported model showing up empty in three.js?' (often
            // because the scene's nodes[] array references the wrong node).
            std::string path = argv[++i];
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr,
                    "info-glb-tree: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            if (bytes.size() < 28) {
                std::fprintf(stderr, "info-glb-tree: file too short\n");
                return 1;
            }
            uint32_t magic, version;
            std::memcpy(&magic, &bytes[0], 4);
            std::memcpy(&version, &bytes[4], 4);
            if (magic != 0x46546C67 || version != 2) {
                std::fprintf(stderr, "info-glb-tree: not glTF 2.0\n");
                return 1;
            }
            uint32_t jsonLen;
            std::memcpy(&jsonLen, &bytes[12], 4);
            std::string jsonStr(bytes.begin() + 20, bytes.begin() + 20 + jsonLen);
            nlohmann::json gj;
            try { gj = nlohmann::json::parse(jsonStr); }
            catch (const std::exception& e) {
                std::fprintf(stderr, "info-glb-tree: JSON parse failed: %s\n", e.what());
                return 1;
            }
            // Tree drawing
            auto branch = [](bool last) { return last ? "└─ " : "├─ "; };
            auto cont = [](bool last) { return last ? "   " : "│  "; };
            std::printf("%s\n", path.c_str());
            // Asset section
            std::string genName = gj.value("/asset/version"_json_pointer, std::string{});
            std::string gen = gj.value("/asset/generator"_json_pointer, std::string{});
            std::printf("├─ asset (v%s, %s)\n",
                        genName.c_str(),
                        gen.empty() ? "no generator" : gen.c_str());
            // Buffers
            int nBuf = (gj.contains("buffers") && gj["buffers"].is_array())
                        ? static_cast<int>(gj["buffers"].size()) : 0;
            std::printf("├─ buffers (%d)\n", nBuf);
            if (nBuf > 0) {
                for (int b = 0; b < nBuf; ++b) {
                    bool last = (b == nBuf - 1);
                    uint64_t bl = gj["buffers"][b].value("byteLength", 0u);
                    std::printf("│  %s[%d] %llu bytes\n", branch(last), b,
                                static_cast<unsigned long long>(bl));
                }
            }
            // BufferViews
            int nBV = (gj.contains("bufferViews") && gj["bufferViews"].is_array())
                       ? static_cast<int>(gj["bufferViews"].size()) : 0;
            std::printf("├─ bufferViews (%d)\n", nBV);
            for (int v = 0; v < nBV; ++v) {
                bool last = (v == nBV - 1);
                const auto& bv = gj["bufferViews"][v];
                uint32_t bo = bv.value("byteOffset", 0u);
                uint32_t bl = bv.value("byteLength", 0u);
                int target = bv.value("target", 0);
                std::printf("│  %s[%d] off=%u len=%u%s\n",
                            branch(last), v, bo, bl,
                            target == 34962 ? " (vertex)"
                          : target == 34963 ? " (index)"
                          : "");
            }
            // Accessors
            int nAcc = (gj.contains("accessors") && gj["accessors"].is_array())
                        ? static_cast<int>(gj["accessors"].size()) : 0;
            std::printf("├─ accessors (%d)\n", nAcc);
            for (int a = 0; a < nAcc; ++a) {
                bool last = (a == nAcc - 1);
                const auto& acc = gj["accessors"][a];
                int ct = acc.value("componentType", 0);
                std::string type = acc.value("type", std::string{});
                uint32_t count = acc.value("count", 0u);
                int bv = acc.value("bufferView", -1);
                const char* ctName =
                    ct == 5120 ? "i8" :
                    ct == 5121 ? "u8" :
                    ct == 5122 ? "i16" :
                    ct == 5123 ? "u16" :
                    ct == 5125 ? "u32" :
                    ct == 5126 ? "f32" : "?";
                std::printf("│  %s[%d] %s %s ×%u (bv=%d)\n",
                            branch(last), a, ctName, type.c_str(), count, bv);
            }
            // Meshes (with primitives nested)
            int nMesh = (gj.contains("meshes") && gj["meshes"].is_array())
                         ? static_cast<int>(gj["meshes"].size()) : 0;
            std::printf("├─ meshes (%d)\n", nMesh);
            for (int m = 0; m < nMesh; ++m) {
                bool lastM = (m == nMesh - 1);
                const auto& mesh = gj["meshes"][m];
                std::string name = mesh.value("name", std::string{});
                int nPrim = (mesh.contains("primitives") && mesh["primitives"].is_array())
                             ? static_cast<int>(mesh["primitives"].size()) : 0;
                std::printf("│  %s[%d]%s%s (%d primitives)\n",
                            branch(lastM), m,
                            name.empty() ? "" : " ",
                            name.c_str(), nPrim);
                for (int p = 0; p < nPrim; ++p) {
                    bool lastP = (p == nPrim - 1);
                    const auto& prim = mesh["primitives"][p];
                    int idxAcc = prim.value("indices", -1);
                    int mode = prim.value("mode", 4);
                    const char* modeName =
                        mode == 0 ? "POINTS" :
                        mode == 1 ? "LINES" :
                        mode == 4 ? "TRIANGLES" : "?";
                    std::printf("│  %s%s[%d] %s indices=acc#%d\n",
                                cont(lastM), branch(lastP), p, modeName, idxAcc);
                }
            }
            // Nodes (flat list — could be tree but glTF nodes are a graph)
            int nNode = (gj.contains("nodes") && gj["nodes"].is_array())
                         ? static_cast<int>(gj["nodes"].size()) : 0;
            std::printf("├─ nodes (%d)\n", nNode);
            for (int n = 0; n < nNode; ++n) {
                bool last = (n == nNode - 1);
                const auto& node = gj["nodes"][n];
                std::string name = node.value("name", std::string{});
                int meshIdx = node.value("mesh", -1);
                std::printf("│  %s[%d]%s%s%s\n",
                            branch(last), n,
                            name.empty() ? "" : " ",
                            name.c_str(),
                            meshIdx >= 0 ? (" -> mesh#" + std::to_string(meshIdx)).c_str() : "");
            }
            // Scenes (last branch)
            int nScene = (gj.contains("scenes") && gj["scenes"].is_array())
                          ? static_cast<int>(gj["scenes"].size()) : 0;
            std::printf("└─ scenes (%d, default=%d)\n",
                        nScene, gj.value("scene", 0));
            for (int s = 0; s < nScene; ++s) {
                bool lastS = (s == nScene - 1);
                const auto& scene = gj["scenes"][s];
                int nodeRefs = (scene.contains("nodes") && scene["nodes"].is_array())
                                ? static_cast<int>(scene["nodes"].size()) : 0;
                std::printf("   %s[%d] nodes=[", branch(lastS), s);
                if (scene.contains("nodes") && scene["nodes"].is_array()) {
                    for (size_t k = 0; k < scene["nodes"].size(); ++k) {
                        std::printf("%s%d", k ? "," : "", scene["nodes"][k].get<int>());
                    }
                }
                std::printf("] (%d nodes)\n", nodeRefs);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--check-glb-bounds") == 0 && i + 1 < argc) {
            // Cross-checks every position accessor's claimed min/max
            // against the actual data in the BIN chunk. glTF viewers use
            // these for camera framing and frustum culling — stale
            // values (e.g. from a tool that edited geometry without
            // recomputing) cause models to vanish at certain angles or
            // get framed wrong on load.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr,
                    "check-glb-bounds: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            // Parse glb structure (re-implements --validate-glb's parser
            // since we need access to the BIN chunk bytes here).
            if (bytes.size() < 28) {
                std::fprintf(stderr,
                    "check-glb-bounds: file too short to be a .glb\n");
                return 1;
            }
            uint32_t magic, version;
            std::memcpy(&magic, &bytes[0], 4);
            std::memcpy(&version, &bytes[4], 4);
            if (magic != 0x46546C67 || version != 2) {
                std::fprintf(stderr,
                    "check-glb-bounds: not a valid glTF 2.0 binary\n");
                return 1;
            }
            uint32_t jsonLen, jsonType;
            std::memcpy(&jsonLen, &bytes[12], 4);
            std::memcpy(&jsonType, &bytes[16], 4);
            std::string jsonStr(bytes.begin() + 20, bytes.begin() + 20 + jsonLen);
            size_t binOff = 20 + jsonLen;
            std::memcpy(&magic, &bytes[binOff + 4], 4);  // chunkType
            const uint8_t* binData = &bytes[binOff + 8];
            uint32_t binLen;
            std::memcpy(&binLen, &bytes[binOff], 4);
            (void)binLen;  // not range-checked here; --validate-glb does that
            nlohmann::json gj;
            try { gj = nlohmann::json::parse(jsonStr); }
            catch (const std::exception& e) {
                std::fprintf(stderr,
                    "check-glb-bounds: JSON parse failed: %s\n", e.what());
                return 1;
            }
            std::vector<std::string> errors;
            int posAccessors = 0, mismatched = 0;
            // Walk all primitives, collect their POSITION accessor index,
            // dedupe (multiple primitives can share an accessor — only
            // recompute once per unique).
            std::set<int> posAccIndices;
            if (gj.contains("meshes") && gj["meshes"].is_array()) {
                for (const auto& m : gj["meshes"]) {
                    if (!m.contains("primitives") || !m["primitives"].is_array()) continue;
                    for (const auto& p : m["primitives"]) {
                        if (p.contains("attributes") &&
                            p["attributes"].contains("POSITION")) {
                            posAccIndices.insert(p["attributes"]["POSITION"].get<int>());
                        }
                    }
                }
            }
            const auto& accessors = gj["accessors"];
            const auto& bufferViews = gj["bufferViews"];
            for (int ai : posAccIndices) {
                if (ai < 0 || ai >= static_cast<int>(accessors.size())) {
                    errors.push_back("position accessor " + std::to_string(ai) +
                                     " out of range");
                    continue;
                }
                const auto& acc = accessors[ai];
                if (acc.value("type", std::string{}) != "VEC3" ||
                    acc.value("componentType", 0) != 5126) {
                    errors.push_back("accessor " + std::to_string(ai) +
                                     " is not VEC3 FLOAT");
                    continue;
                }
                posAccessors++;
                int bvIdx = acc.value("bufferView", -1);
                if (bvIdx < 0 || bvIdx >= static_cast<int>(bufferViews.size())) {
                    errors.push_back("accessor " + std::to_string(ai) +
                                     " bufferView " + std::to_string(bvIdx) +
                                     " out of range");
                    continue;
                }
                const auto& bv = bufferViews[bvIdx];
                uint32_t bvOff = bv.value("byteOffset", 0u);
                uint32_t accOff = acc.value("byteOffset", 0u);
                uint32_t count = acc.value("count", 0u);
                const uint8_t* p = binData + bvOff + accOff;
                glm::vec3 actualMin{1e30f}, actualMax{-1e30f};
                for (uint32_t v = 0; v < count; ++v) {
                    glm::vec3 pos;
                    std::memcpy(&pos.x, p + v * 12 + 0, 4);
                    std::memcpy(&pos.y, p + v * 12 + 4, 4);
                    std::memcpy(&pos.z, p + v * 12 + 8, 4);
                    actualMin = glm::min(actualMin, pos);
                    actualMax = glm::max(actualMax, pos);
                }
                // Compare against claimed min/max (within float epsilon).
                glm::vec3 claimedMin{0}, claimedMax{0};
                bool hasClaimed = (acc.contains("min") && acc.contains("max"));
                if (hasClaimed) {
                    claimedMin.x = acc["min"][0]; claimedMin.y = acc["min"][1]; claimedMin.z = acc["min"][2];
                    claimedMax.x = acc["max"][0]; claimedMax.y = acc["max"][1]; claimedMax.z = acc["max"][2];
                    auto close = [](float a, float b) {
                        return std::abs(a - b) < 1e-3f;
                    };
                    bool ok = close(claimedMin.x, actualMin.x) &&
                              close(claimedMin.y, actualMin.y) &&
                              close(claimedMin.z, actualMin.z) &&
                              close(claimedMax.x, actualMax.x) &&
                              close(claimedMax.y, actualMax.y) &&
                              close(claimedMax.z, actualMax.z);
                    if (!ok) {
                        mismatched++;
                        char buf[256];
                        std::snprintf(buf, sizeof(buf),
                            "accessor %d bounds mismatch: claimed [%g,%g,%g]-[%g,%g,%g] vs actual [%g,%g,%g]-[%g,%g,%g]",
                            ai,
                            claimedMin.x, claimedMin.y, claimedMin.z,
                            claimedMax.x, claimedMax.y, claimedMax.z,
                            actualMin.x, actualMin.y, actualMin.z,
                            actualMax.x, actualMax.y, actualMax.z);
                        errors.push_back(buf);
                    }
                } else {
                    // glTF spec requires position accessors to declare min/max.
                    errors.push_back("accessor " + std::to_string(ai) +
                                     " missing required min/max for POSITION attribute");
                    mismatched++;
                }
            }
            if (jsonOut) {
                nlohmann::json j;
                j["glb"] = path;
                j["positionAccessors"] = posAccessors;
                j["mismatched"] = mismatched;
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return errors.empty() ? 0 : 1;
            }
            std::printf("GLB bounds: %s\n", path.c_str());
            std::printf("  position accessors checked : %d\n", posAccessors);
            std::printf("  mismatched                 : %d\n", mismatched);
            if (errors.empty()) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %zu error(s):\n", errors.size());
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--validate-stl") == 0 && i + 1 < argc) {
            // Structural validator for ASCII STL — pairs with --export-stl
            // and --import-stl (and --bake-zone-stl). Catches truncation,
            // missing solid framing, mismatched facet/vertex counts, and
            // non-finite vertex coords that would crash a slicer's mesh
            // analyzer.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path);
            if (!in) {
                std::fprintf(stderr,
                    "validate-stl: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<std::string> errors;
            std::string solidName;
            int facetCount = 0, vertCount = 0, nonFinite = 0;
            int facetsOpen = 0;  // facet-without-endfacet leak detector
            bool sawSolid = false, sawEndsolid = false;
            int currentFacetVerts = 0;
            std::string line;
            int lineNum = 0;
            while (std::getline(in, line)) {
                lineNum++;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.empty()) continue;
                std::istringstream ss(line);
                std::string tok;
                ss >> tok;
                if (tok == "solid") {
                    if (sawSolid) {
                        errors.push_back("line " + std::to_string(lineNum) +
                                         ": multiple 'solid' headers");
                    }
                    sawSolid = true;
                    ss >> solidName;
                } else if (tok == "facet") {
                    facetCount++;
                    facetsOpen++;
                    currentFacetVerts = 0;
                    std::string nrmTok;
                    ss >> nrmTok;
                    if (nrmTok != "normal") {
                        errors.push_back("line " + std::to_string(lineNum) +
                                         ": 'facet' missing 'normal' subtoken");
                    } else {
                        float nx, ny, nz;
                        if (!(ss >> nx >> ny >> nz)) {
                            errors.push_back("line " + std::to_string(lineNum) +
                                             ": 'facet normal' missing 3 floats");
                        } else if (!std::isfinite(nx) || !std::isfinite(ny) ||
                                    !std::isfinite(nz)) {
                            errors.push_back("line " + std::to_string(lineNum) +
                                             ": non-finite facet normal");
                            nonFinite++;
                        }
                    }
                } else if (tok == "vertex") {
                    vertCount++;
                    currentFacetVerts++;
                    float x, y, z;
                    if (!(ss >> x >> y >> z)) {
                        errors.push_back("line " + std::to_string(lineNum) +
                                         ": 'vertex' missing 3 floats");
                    } else if (!std::isfinite(x) || !std::isfinite(y) ||
                                !std::isfinite(z)) {
                        nonFinite++;
                        if (errors.size() < 30) {
                            errors.push_back("line " + std::to_string(lineNum) +
                                             ": non-finite vertex coord");
                        }
                    }
                } else if (tok == "endfacet") {
                    facetsOpen--;
                    if (currentFacetVerts != 3) {
                        errors.push_back("line " + std::to_string(lineNum) +
                                         ": facet has " +
                                         std::to_string(currentFacetVerts) +
                                         " vertices, expected exactly 3");
                    }
                } else if (tok == "endsolid") {
                    sawEndsolid = true;
                }
                // outer loop / endloop are required by spec but ignored
                // here; their absence doesn't break parsing as long as
                // the vertex count per facet is correct.
            }
            if (!sawSolid) errors.push_back("missing 'solid' header");
            if (!sawEndsolid) errors.push_back("missing 'endsolid' footer");
            if (facetsOpen != 0) {
                errors.push_back(std::to_string(facetsOpen) +
                                 " unclosed 'facet' (missing 'endfacet')");
            }
            if (vertCount != facetCount * 3) {
                errors.push_back("vertex count " + std::to_string(vertCount) +
                                 " != 3 * facet count " +
                                 std::to_string(facetCount));
            }
            if (jsonOut) {
                nlohmann::json j;
                j["stl"] = path;
                j["solidName"] = solidName;
                j["facetCount"] = facetCount;
                j["vertexCount"] = vertCount;
                j["nonFiniteCount"] = nonFinite;
                j["errorCount"] = errors.size();
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return errors.empty() ? 0 : 1;
            }
            std::printf("STL: %s\n", path.c_str());
            std::printf("  solid name : %s\n",
                        solidName.empty() ? "(unset)" : solidName.c_str());
            std::printf("  facets     : %d\n", facetCount);
            std::printf("  vertices   : %d\n", vertCount);
            if (nonFinite > 0) {
                std::printf("  non-finite : %d\n", nonFinite);
            }
            if (errors.empty()) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %zu error(s):\n", errors.size());
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--validate-png") == 0 && i + 1 < argc) {
            // Full PNG structural validator — beyond --info-png's
            // header-only sniff. Walks every chunk, verifies CRC,
            // ensures IHDR/IDAT/IEND are present and ordered correctly.
            // Catches the kind of corruption (truncation mid-IDAT,
            // bit-flip in CRC) that browsers/decoders silently skip.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr,
                    "validate-png: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            std::vector<std::string> errors;
            // PNG signature: 89 50 4E 47 0D 0A 1A 0A
            static const uint8_t kSig[8] = {0x89, 0x50, 0x4E, 0x47,
                                             0x0D, 0x0A, 0x1A, 0x0A};
            if (bytes.size() < 8 || std::memcmp(bytes.data(), kSig, 8) != 0) {
                errors.push_back("missing PNG signature");
            }
            // CRC32 table per PNG spec (matches the standard polynomial
            // 0xEDB88320; building once via constexpr-eligible logic).
            uint32_t crcTable[256];
            for (uint32_t n = 0; n < 256; ++n) {
                uint32_t c = n;
                for (int k = 0; k < 8; ++k) {
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                crcTable[n] = c;
            }
            auto crc32 = [&](const uint8_t* data, size_t len) {
                uint32_t c = 0xFFFFFFFFu;
                for (size_t k = 0; k < len; ++k) {
                    c = crcTable[(c ^ data[k]) & 0xFF] ^ (c >> 8);
                }
                return c ^ 0xFFFFFFFFu;
            };
            auto be32 = [](const uint8_t* p) {
                return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                       (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
            };
            int chunkCount = 0;
            int badCrcs = 0;
            bool sawIHDR = false, sawIDAT = false, sawIEND = false;
            bool ihdrFirst = false;
            std::string firstChunkType;
            uint32_t width = 0, height = 0;
            uint8_t bitDepth = 0, colorType = 0;
            // Walk chunks: each is length(4) + type(4) + data(length) + crc(4).
            size_t off = 8;
            while (errors.empty() && off + 12 <= bytes.size()) {
                uint32_t len = be32(&bytes[off]);
                if (off + 8 + len + 4 > bytes.size()) {
                    errors.push_back("chunk at offset " + std::to_string(off) +
                                     " extends past file end");
                    break;
                }
                std::string type(reinterpret_cast<const char*>(&bytes[off + 4]), 4);
                if (chunkCount == 0) {
                    firstChunkType = type;
                    ihdrFirst = (type == "IHDR");
                }
                chunkCount++;
                if (type == "IHDR") {
                    sawIHDR = true;
                    if (len >= 13) {
                        width = be32(&bytes[off + 8]);
                        height = be32(&bytes[off + 12]);
                        bitDepth = bytes[off + 16];
                        colorType = bytes[off + 17];
                    }
                } else if (type == "IDAT") {
                    sawIDAT = true;
                } else if (type == "IEND") {
                    sawIEND = true;
                }
                // Verify CRC (computed over type + data, not length).
                uint32_t storedCrc = be32(&bytes[off + 8 + len]);
                uint32_t actualCrc = crc32(&bytes[off + 4], 4 + len);
                if (storedCrc != actualCrc) {
                    badCrcs++;
                    if (errors.size() < 10) {
                        char buf[128];
                        std::snprintf(buf, sizeof(buf),
                            "chunk '%s' at offset %zu: CRC mismatch (stored=0x%08X actual=0x%08X)",
                            type.c_str(), off, storedCrc, actualCrc);
                        errors.push_back(buf);
                    }
                }
                off += 8 + len + 4;
            }
            if (!ihdrFirst) {
                errors.push_back("first chunk is '" + firstChunkType +
                                  "', expected 'IHDR'");
            }
            if (!sawIHDR) errors.push_back("missing required IHDR chunk");
            if (!sawIDAT) errors.push_back("missing required IDAT chunk");
            if (!sawIEND) errors.push_back("missing required IEND chunk");
            if (off < bytes.size()) {
                errors.push_back(std::to_string(bytes.size() - off) +
                                  " trailing bytes after IEND chunk");
            }
            if (jsonOut) {
                nlohmann::json j;
                j["png"] = path;
                j["width"] = width;
                j["height"] = height;
                j["bitDepth"] = bitDepth;
                j["colorType"] = colorType;
                j["chunkCount"] = chunkCount;
                j["badCrcs"] = badCrcs;
                j["fileSize"] = bytes.size();
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return errors.empty() ? 0 : 1;
            }
            std::printf("PNG: %s\n", path.c_str());
            std::printf("  size       : %u x %u\n", width, height);
            std::printf("  bit depth  : %u (color type %u)\n", bitDepth, colorType);
            std::printf("  chunks     : %d (%d CRC mismatches)\n",
                        chunkCount, badCrcs);
            std::printf("  file bytes : %zu\n", bytes.size());
            if (errors.empty()) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %zu error(s):\n", errors.size());
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--validate-jsondbc") == 0 && i + 1 < argc) {
            // Strict schema validator for JSON DBC sidecars. --info-jsondbc
            // checks that header recordCount matches the actual records[]
            // length; this goes deeper:
            //   - format tag is the wowee 1.0 string
            //   - source field present (so re-import knows which DBC slot)
            //   - recordCount + fieldCount are non-negative integers
            //   - records is an array
            //   - each record is an array exactly fieldCount long
            //   - each cell is string|number|bool|null (no objects/arrays)
            // Catches the kind of corruption that load() might silently
            // tolerate (missing fields default to 0/empty), letting the
            // editor's runtime DBC loader downstream-fail in confusing
            // ways.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path);
            if (!in) {
                std::fprintf(stderr,
                    "validate-jsondbc: cannot open %s\n", path.c_str());
                return 1;
            }
            nlohmann::json doc;
            std::vector<std::string> errors;
            try {
                in >> doc;
            } catch (const std::exception& e) {
                errors.push_back(std::string("JSON parse error: ") + e.what());
            }
            std::string format, source;
            uint32_t recordCount = 0, fieldCount = 0;
            uint32_t actualRecs = 0;
            int badRowWidths = 0, badCellTypes = 0;
            if (errors.empty()) {
                if (!doc.is_object()) {
                    errors.push_back("top-level value is not a JSON object");
                } else {
                    if (!doc.contains("format")) {
                        errors.push_back("missing 'format' field");
                    } else if (!doc["format"].is_string()) {
                        errors.push_back("'format' field is not a string");
                    } else {
                        format = doc["format"].get<std::string>();
                        if (format != "wowee-dbc-json-1.0") {
                            errors.push_back("'format' is '" + format +
                                             "', expected 'wowee-dbc-json-1.0'");
                        }
                    }
                    if (!doc.contains("source")) {
                        errors.push_back("missing 'source' field (re-import needs it)");
                    } else {
                        source = doc.value("source", std::string{});
                    }
                    if (!doc.contains("recordCount") ||
                        !doc["recordCount"].is_number_integer()) {
                        errors.push_back("'recordCount' missing or not an integer");
                    } else {
                        recordCount = doc["recordCount"].get<uint32_t>();
                    }
                    if (!doc.contains("fieldCount") ||
                        !doc["fieldCount"].is_number_integer()) {
                        errors.push_back("'fieldCount' missing or not an integer");
                    } else {
                        fieldCount = doc["fieldCount"].get<uint32_t>();
                    }
                    if (!doc.contains("records") || !doc["records"].is_array()) {
                        errors.push_back("'records' missing or not an array");
                    } else {
                        const auto& records = doc["records"];
                        actualRecs = static_cast<uint32_t>(records.size());
                        if (actualRecs != recordCount) {
                            errors.push_back("recordCount " + std::to_string(recordCount) +
                                             " != actual records " +
                                             std::to_string(actualRecs));
                        }
                        for (size_t r = 0; r < records.size(); ++r) {
                            const auto& row = records[r];
                            if (!row.is_array()) {
                                errors.push_back("record[" + std::to_string(r) +
                                                 "] is not an array");
                                continue;
                            }
                            if (row.size() != fieldCount) {
                                badRowWidths++;
                                if (badRowWidths <= 3) {
                                    errors.push_back("record[" + std::to_string(r) +
                                                     "] has " + std::to_string(row.size()) +
                                                     " cells, expected " +
                                                     std::to_string(fieldCount));
                                }
                            }
                            for (size_t c = 0; c < row.size(); ++c) {
                                const auto& cell = row[c];
                                bool ok = cell.is_string() || cell.is_number() ||
                                          cell.is_boolean() || cell.is_null();
                                if (!ok) {
                                    badCellTypes++;
                                    if (badCellTypes <= 3) {
                                        errors.push_back("record[" + std::to_string(r) +
                                                         "][" + std::to_string(c) +
                                                         "] has invalid type (objects/arrays not allowed)");
                                    }
                                }
                            }
                        }
                        if (badRowWidths > 3) {
                            errors.push_back("... and " + std::to_string(badRowWidths - 3) +
                                             " more rows with wrong cell count");
                        }
                        if (badCellTypes > 3) {
                            errors.push_back("... and " + std::to_string(badCellTypes - 3) +
                                             " more cells with invalid types");
                        }
                    }
                }
            }
            int errorCount = static_cast<int>(errors.size());
            if (jsonOut) {
                nlohmann::json j;
                j["jsondbc"] = path;
                j["format"] = format;
                j["source"] = source;
                j["recordCount"] = recordCount;
                j["fieldCount"] = fieldCount;
                j["actualRecords"] = actualRecs;
                j["errorCount"] = errorCount;
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return errors.empty() ? 0 : 1;
            }
            std::printf("JSON DBC: %s\n", path.c_str());
            std::printf("  format    : %s\n", format.empty() ? "?" : format.c_str());
            std::printf("  source    : %s\n", source.empty() ? "?" : source.c_str());
            std::printf("  records   : %u (header) / %u (actual)\n",
                        recordCount, actualRecs);
            std::printf("  fields    : %u\n", fieldCount);
            if (errors.empty()) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %d error(s):\n", errorCount);
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--export-obj") == 0 && i + 1 < argc) {
            // Convert WOM (our open M2 replacement) to Wavefront OBJ — a
            // universally supported text format that opens directly in
            // Blender, MeshLab, ZBrush, Maya, and basically every other 3D
            // tool ever made. Makes the open-format ecosystem actually
            // useful for content authors who don't want to write a custom
            // WOM importer for their DCC of choice.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr, "WOM not found: %s.wom\n", base.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = base + ".obj";
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            if (!wom.isValid()) {
                std::fprintf(stderr, "WOM has no geometry to export: %s.wom\n", base.c_str());
                return 1;
            }
            std::ofstream obj(outPath);
            if (!obj) {
                std::fprintf(stderr, "Failed to open output file: %s\n", outPath.c_str());
                return 1;
            }
            // Header — preserves provenance so a designer reopening the OBJ
            // weeks later knows where it came from. The MTL line is a
            // courtesy: we don't currently emit a .mtl, but downstream
            // tools won't error without one either.
            obj << "# Wavefront OBJ generated by wowee_editor --export-obj\n";
            obj << "# Source: " << base << ".wom (v" << wom.version << ")\n";
            obj << "# Verts: " << wom.vertices.size()
                << " Tris: " << wom.indices.size() / 3
                << " Textures: " << wom.texturePaths.size() << "\n\n";
            obj << "o " << (wom.name.empty() ? "WoweeModel" : wom.name) << "\n";
            // Positions (v), texcoords (vt), normals (vn) — OBJ flips V so
            // that the same UVs that look right in our Vulkan renderer
            // also look right in Blender's bottom-left UV convention.
            for (const auto& v : wom.vertices) {
                obj << "v " << v.position.x << " " << v.position.y
                    << " " << v.position.z << "\n";
            }
            for (const auto& v : wom.vertices) {
                obj << "vt " << v.texCoord.x << " " << (1.0f - v.texCoord.y) << "\n";
            }
            for (const auto& v : wom.vertices) {
                obj << "vn " << v.normal.x << " " << v.normal.y
                    << " " << v.normal.z << "\n";
            }
            // Faces — split per-batch so each material/texture range becomes
            // its own group. Falls back to a single group when the WOM
            // wasn't authored with batches (WOM1/WOM2). OBJ indices are
            // 1-based, hence the +1.
            auto emitFaces = [&](const char* groupName,
                                  uint32_t start, uint32_t count) {
                obj << "g " << groupName << "\n";
                for (uint32_t k = 0; k < count; k += 3) {
                    uint32_t i0 = wom.indices[start + k] + 1;
                    uint32_t i1 = wom.indices[start + k + 1] + 1;
                    uint32_t i2 = wom.indices[start + k + 2] + 1;
                    obj << "f "
                        << i0 << "/" << i0 << "/" << i0 << " "
                        << i1 << "/" << i1 << "/" << i1 << " "
                        << i2 << "/" << i2 << "/" << i2 << "\n";
                }
            };
            if (wom.batches.empty()) {
                emitFaces("mesh", 0,
                          static_cast<uint32_t>(wom.indices.size()));
            } else {
                for (size_t b = 0; b < wom.batches.size(); ++b) {
                    const auto& batch = wom.batches[b];
                    std::string groupName = "batch_" + std::to_string(b);
                    if (batch.textureIndex < wom.texturePaths.size()) {
                        // Strip directory + extension for a readable group
                        // name; full path is preserved in the file header
                        // comment so nothing is lost.
                        std::string tex = wom.texturePaths[batch.textureIndex];
                        auto slash = tex.find_last_of("/\\");
                        if (slash != std::string::npos) tex = tex.substr(slash + 1);
                        auto dot = tex.find_last_of('.');
                        if (dot != std::string::npos) tex = tex.substr(0, dot);
                        if (!tex.empty()) groupName += "_" + tex;
                    }
                    emitFaces(groupName.c_str(), batch.indexStart, batch.indexCount);
                }
            }
            obj.close();
            std::printf("Exported %s.wom -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  %zu verts, %zu tris, %zu groups\n",
                        wom.vertices.size(), wom.indices.size() / 3,
                        wom.batches.empty() ? size_t(1) : wom.batches.size());
            return 0;
        } else if (std::strcmp(argv[i], "--export-glb") == 0 && i + 1 < argc) {
            // glTF 2.0 binary (.glb) export — modern industry standard
            // that, unlike OBJ, supports skinning + animations + PBR
            // materials natively. v1 here writes positions/normals/UVs/
            // indices as a single mesh (or one primitive per WOM3 batch);
            // bones/anims are deliberately not yet emitted because glTF's
            // joint matrix layout differs from WOM's bone tree and needs
            // a careful re-mapping pass.
            //
            // Why this matters: glTF is what Sketchfab, Three.js, Babylon.js,
            // and Unity/Unreal-via-import all consume. Shipping WOM through
            // .glb makes our open binary format viewable in any modern
            // browser-based 3D viewer with zero conversion friction.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr, "WOM not found: %s.wom\n", base.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = base + ".glb";
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            if (!wom.isValid()) {
                std::fprintf(stderr, "WOM has no geometry: %s.wom\n", base.c_str());
                return 1;
            }
            // BIN chunk layout — sections ordered so each accessor's
            // byteOffset is naturally aligned for its component type:
            //   positions (vec3 float)  : 12 bytes/vert, offset 0
            //   normals   (vec3 float)  : 12 bytes/vert
            //   uvs       (vec2 float)  :  8 bytes/vert
            //   indices   (uint32)      :  4 bytes each
            // After 32 bytes per vertex, indices start at a 4-byte aligned
            // offset for free.
            const uint32_t vCount = static_cast<uint32_t>(wom.vertices.size());
            const uint32_t iCount = static_cast<uint32_t>(wom.indices.size());
            const uint32_t posOff = 0;
            const uint32_t nrmOff = posOff + vCount * 12;
            const uint32_t uvOff  = nrmOff + vCount * 12;
            const uint32_t idxOff = uvOff  + vCount * 8;
            const uint32_t binSize = idxOff + iCount * 4;
            std::vector<uint8_t> bin(binSize);
            // Pack positions
            for (uint32_t v = 0; v < vCount; ++v) {
                const auto& vert = wom.vertices[v];
                std::memcpy(&bin[posOff + v * 12 + 0], &vert.position.x, 4);
                std::memcpy(&bin[posOff + v * 12 + 4], &vert.position.y, 4);
                std::memcpy(&bin[posOff + v * 12 + 8], &vert.position.z, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 0], &vert.normal.x, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 4], &vert.normal.y, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 8], &vert.normal.z, 4);
                std::memcpy(&bin[uvOff  + v * 8  + 0], &vert.texCoord.x, 4);
                std::memcpy(&bin[uvOff  + v * 8  + 4], &vert.texCoord.y, 4);
            }
            std::memcpy(&bin[idxOff], wom.indices.data(), iCount * 4);
            // Compute bounds for the position accessor's min/max — glTF
            // viewers rely on these for camera framing and culling.
            glm::vec3 bMin{1e30f}, bMax{-1e30f};
            for (const auto& v : wom.vertices) {
                bMin = glm::min(bMin, v.position);
                bMax = glm::max(bMax, v.position);
            }
            // Build the JSON structure. nlohmann::json keeps insertion
            // order in dump(), but glTF readers are key-based so order
            // doesn't matter functionally.
            nlohmann::json gj;
            gj["asset"] = {{"version", "2.0"},
                            {"generator", "wowee_editor --export-glb"}};
            gj["scene"] = 0;
            gj["scenes"] = nlohmann::json::array({nlohmann::json{{"nodes", {0}}}});
            gj["nodes"] = nlohmann::json::array({nlohmann::json{
                {"name", wom.name.empty() ? "WoweeModel" : wom.name},
                {"mesh", 0}
            }});
            gj["buffers"] = nlohmann::json::array({nlohmann::json{
                {"byteLength", binSize}
            }});
            // BufferViews: one per attribute + one per index range.
            // Per WOM3 batch we slice the index bufferView with separate
            // accessors so each batch becomes its own primitive.
            nlohmann::json bufferViews = nlohmann::json::array();
            // 0: positions, 1: normals, 2: uvs, 3: indices (whole range)
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", posOff},
                                    {"byteLength", vCount * 12},
                                    {"target", 34962}}); // ARRAY_BUFFER
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", nrmOff},
                                    {"byteLength", vCount * 12},
                                    {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", uvOff},
                                    {"byteLength", vCount * 8},
                                    {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", idxOff},
                                    {"byteLength", iCount * 4},
                                    {"target", 34963}}); // ELEMENT_ARRAY_BUFFER
            gj["bufferViews"] = bufferViews;
            // Accessors: 0=position, 1=normal, 2=uv, 3..N=indices (one
            // per primitive, sliced from bufferView 3).
            nlohmann::json accessors = nlohmann::json::array();
            accessors.push_back({
                {"bufferView", 0}, {"componentType", 5126}, // FLOAT
                {"count", vCount}, {"type", "VEC3"},
                {"min", {bMin.x, bMin.y, bMin.z}},
                {"max", {bMax.x, bMax.y, bMax.z}}
            });
            accessors.push_back({
                {"bufferView", 1}, {"componentType", 5126},
                {"count", vCount}, {"type", "VEC3"}
            });
            accessors.push_back({
                {"bufferView", 2}, {"componentType", 5126},
                {"count", vCount}, {"type", "VEC2"}
            });
            // Build primitives — one per WOM3 batch, or one over the
            // whole index range if no batches.
            nlohmann::json primitives = nlohmann::json::array();
            auto addPrimitive = [&](uint32_t idxStart, uint32_t idxCount) {
                uint32_t accessorIdx = static_cast<uint32_t>(accessors.size());
                accessors.push_back({
                    {"bufferView", 3},
                    {"byteOffset", idxStart * 4},
                    {"componentType", 5125}, // UNSIGNED_INT
                    {"count", idxCount},
                    {"type", "SCALAR"}
                });
                primitives.push_back({
                    {"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                    {"indices", accessorIdx},
                    {"mode", 4} // TRIANGLES
                });
            };
            if (wom.batches.empty()) {
                addPrimitive(0, iCount);
            } else {
                for (const auto& b : wom.batches) {
                    addPrimitive(b.indexStart, b.indexCount);
                }
            }
            gj["accessors"] = accessors;
            gj["meshes"] = nlohmann::json::array({nlohmann::json{
                {"primitives", primitives}
            }});
            // Serialize JSON to bytes; pad to 4-byte boundary with spaces
            // (glTF spec requires JSON chunk padded with 0x20).
            std::string jsonStr = gj.dump();
            while (jsonStr.size() % 4 != 0) jsonStr += ' ';
            // BIN chunk pads to 4-byte boundary with zeros (already
            // satisfied since binSize = idxOff + iCount*4 and idxOff is
            // 4-byte aligned).
            uint32_t jsonLen = static_cast<uint32_t>(jsonStr.size());
            uint32_t binLen = binSize;
            uint32_t totalLen = 12 + 8 + jsonLen + 8 + binLen;
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "Failed to open output: %s\n", outPath.c_str());
                return 1;
            }
            // Header: magic, version, total length (all little-endian uint32)
            uint32_t magic = 0x46546C67;  // 'glTF'
            uint32_t version = 2;
            out.write(reinterpret_cast<const char*>(&magic), 4);
            out.write(reinterpret_cast<const char*>(&version), 4);
            out.write(reinterpret_cast<const char*>(&totalLen), 4);
            // JSON chunk header + payload
            uint32_t jsonChunkType = 0x4E4F534A;  // 'JSON'
            out.write(reinterpret_cast<const char*>(&jsonLen), 4);
            out.write(reinterpret_cast<const char*>(&jsonChunkType), 4);
            out.write(jsonStr.data(), jsonLen);
            // BIN chunk header + payload
            uint32_t binChunkType = 0x004E4942;  // 'BIN\0'
            out.write(reinterpret_cast<const char*>(&binLen), 4);
            out.write(reinterpret_cast<const char*>(&binChunkType), 4);
            out.write(reinterpret_cast<const char*>(bin.data()), binLen);
            out.close();
            std::printf("Exported %s.wom -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  %u verts, %u tris, %zu primitive(s), %u-byte binary chunk\n",
                        vCount, iCount / 3, primitives.size(), binLen);
            return 0;
        } else if (std::strcmp(argv[i], "--export-stl") == 0 && i + 1 < argc) {
            // ASCII STL export — single most universal 3D-printer format.
            // Cura, PrusaSlicer, Bambu Studio, Slic3r, OctoPrint, MakerBot
            // — every slicer made in the last 25 years opens STL natively.
            // Lets WOM models drive physical prints with no conversion
            // friction beyond this one command.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr, "WOM not found: %s.wom\n", base.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = base + ".stl";
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            if (!wom.isValid()) {
                std::fprintf(stderr, "WOM has no geometry: %s.wom\n", base.c_str());
                return 1;
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr, "Failed to open output: %s\n", outPath.c_str());
                return 1;
            }
            // STL solid name must be alphanumeric + underscores per loose
            // convention; sanitize whatever the WOM name contains. Empty
            // -> 'wowee_model'.
            std::string solidName = wom.name.empty() ? "wowee_model" : wom.name;
            for (auto& c : solidName) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_')) c = '_';
            }
            out << "solid " << solidName << "\n";
            // Per-triangle facet — STL has no shared vertex pool, every
            // triangle stands alone. Compute face normal from cross product
            // (STL spec requires unit-length face normal; viewers fall
            // back to per-vertex if zero, but most slicers want the real
            // value for orientation hints).
            uint32_t triCount = 0;
            for (size_t k = 0; k + 2 < wom.indices.size(); k += 3) {
                uint32_t i0 = wom.indices[k];
                uint32_t i1 = wom.indices[k + 1];
                uint32_t i2 = wom.indices[k + 2];
                if (i0 >= wom.vertices.size() || i1 >= wom.vertices.size() ||
                    i2 >= wom.vertices.size()) continue;
                const auto& v0 = wom.vertices[i0].position;
                const auto& v1 = wom.vertices[i1].position;
                const auto& v2 = wom.vertices[i2].position;
                glm::vec3 e1 = v1 - v0;
                glm::vec3 e2 = v2 - v0;
                glm::vec3 n = glm::cross(e1, e2);
                float len = glm::length(n);
                if (len > 1e-12f) n /= len;
                else n = {0, 0, 1};  // degenerate — STL spec allows any unit normal
                out << "  facet normal " << n.x << " " << n.y << " " << n.z << "\n"
                    << "    outer loop\n"
                    << "      vertex " << v0.x << " " << v0.y << " " << v0.z << "\n"
                    << "      vertex " << v1.x << " " << v1.y << " " << v1.z << "\n"
                    << "      vertex " << v2.x << " " << v2.y << " " << v2.z << "\n"
                    << "    endloop\n"
                    << "  endfacet\n";
                triCount++;
            }
            out << "endsolid " << solidName << "\n";
            out.close();
            std::printf("Exported %s.wom -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  solid '%s', %u facets\n",
                        solidName.c_str(), triCount);
            return 0;
        } else if (std::strcmp(argv[i], "--import-stl") == 0 && i + 1 < argc) {
            // ASCII STL -> WOM. Closes the STL round trip so designers can
            // edit prints in TinkerCAD/Meshmixer/SolidWorks and bring them
            // back to the engine. Dedupes vertices on (pos, normal) so the
            // resulting WOM vertex buffer stays compact.
            std::string stlPath = argv[++i];
            std::string womBase;
            if (i + 1 < argc && argv[i + 1][0] != '-') womBase = argv[++i];
            if (!std::filesystem::exists(stlPath)) {
                std::fprintf(stderr, "STL not found: %s\n", stlPath.c_str());
                return 1;
            }
            if (womBase.empty()) {
                womBase = stlPath;
                if (womBase.size() >= 4 &&
                    womBase.substr(womBase.size() - 4) == ".stl") {
                    womBase = womBase.substr(0, womBase.size() - 4);
                }
            }
            std::ifstream in(stlPath);
            if (!in) {
                std::fprintf(stderr, "Failed to open STL: %s\n", stlPath.c_str());
                return 1;
            }
            wowee::pipeline::WoweeModel wom;
            wom.version = 1;
            // Dedupe key: 6 floats (pos + normal) packed as a string. Loose
            // matching, but exact for round-trips since we write the same
            // floats back. Real-world STLs from CAD tools rarely benefit
            // from looser tolerance — they already share verts at the
            // exporter level.
            std::unordered_map<std::string, uint32_t> dedupe;
            auto interVert = [&](const glm::vec3& pos, const glm::vec3& nrm) {
                char key[128];
                std::snprintf(key, sizeof(key), "%.6f|%.6f|%.6f|%.6f|%.6f|%.6f",
                              pos.x, pos.y, pos.z, nrm.x, nrm.y, nrm.z);
                auto it = dedupe.find(key);
                if (it != dedupe.end()) return it->second;
                wowee::pipeline::WoweeModel::Vertex v;
                v.position = pos;
                v.normal = nrm;
                v.texCoord = {0, 0};
                uint32_t idx = static_cast<uint32_t>(wom.vertices.size());
                wom.vertices.push_back(v);
                dedupe[key] = idx;
                return idx;
            };
            std::string line;
            std::string solidName;
            // Per-facet state: parsed normal + accumulating vertex queue.
            glm::vec3 currentNormal{0, 0, 1};
            std::vector<glm::vec3> facetVerts;
            int facetCount = 0;
            while (std::getline(in, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                std::istringstream ss(line);
                std::string tok;
                ss >> tok;
                if (tok == "solid" && solidName.empty()) {
                    ss >> solidName;
                } else if (tok == "facet") {
                    std::string normalKw;
                    ss >> normalKw;
                    if (normalKw == "normal") {
                        ss >> currentNormal.x >> currentNormal.y >> currentNormal.z;
                    }
                    facetVerts.clear();
                } else if (tok == "vertex") {
                    glm::vec3 v;
                    ss >> v.x >> v.y >> v.z;
                    facetVerts.push_back(v);
                } else if (tok == "endfacet") {
                    if (facetVerts.size() == 3) {
                        // Use the facet normal for all 3 verts since STL
                        // doesn't carry per-vertex normals. Glue-points to
                        // adjacent facets will get distinct verts (which is
                        // correct for faceted-shading STL geometry).
                        for (const auto& v : facetVerts) {
                            wom.indices.push_back(interVert(v, currentNormal));
                        }
                        facetCount++;
                    }
                    facetVerts.clear();
                }
                // 'outer loop', 'endloop', 'endsolid' ignored — we infer
                // from the vertex count per facet.
            }
            if (wom.vertices.empty() || wom.indices.empty()) {
                std::fprintf(stderr,
                    "import-stl: no geometry parsed from %s\n", stlPath.c_str());
                return 1;
            }
            wom.name = solidName.empty()
                ? std::filesystem::path(stlPath).stem().string()
                : solidName;
            // Compute bounds — renderer culls by these so wrong values
            // make models disappear at distance.
            wom.boundMin = wom.vertices[0].position;
            wom.boundMax = wom.boundMin;
            for (const auto& v : wom.vertices) {
                wom.boundMin = glm::min(wom.boundMin, v.position);
                wom.boundMax = glm::max(wom.boundMax, v.position);
            }
            glm::vec3 center = (wom.boundMin + wom.boundMax) * 0.5f;
            float r2 = 0;
            for (const auto& v : wom.vertices) {
                glm::vec3 d = v.position - center;
                r2 = std::max(r2, glm::dot(d, d));
            }
            wom.boundRadius = std::sqrt(r2);
            if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
                std::fprintf(stderr, "import-stl: failed to write %s.wom\n",
                             womBase.c_str());
                return 1;
            }
            std::printf("Imported %s -> %s.wom\n", stlPath.c_str(), womBase.c_str());
            std::printf("  %d facets, %zu verts (deduped), bounds [%.2f, %.2f, %.2f] - [%.2f, %.2f, %.2f]\n",
                        facetCount, wom.vertices.size(),
                        wom.boundMin.x, wom.boundMin.y, wom.boundMin.z,
                        wom.boundMax.x, wom.boundMax.y, wom.boundMax.z);
            return 0;
        } else if (std::strcmp(argv[i], "--export-wob-glb") == 0 && i + 1 < argc) {
            // glTF 2.0 binary export for WOB. Same purpose as --export-glb
            // for WOM but adapted for buildings: each WOB group becomes
            // one primitive in a single mesh, sharing one big vertex
            // pool concatenated from per-group vertex arrays.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wob")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeBuildingLoader::exists(base)) {
                std::fprintf(stderr, "WOB not found: %s.wob\n", base.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = base + ".glb";
            auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
            if (!bld.isValid()) {
                std::fprintf(stderr, "WOB has no groups: %s.wob\n", base.c_str());
                return 1;
            }
            // Total counts + per-group offsets needed before allocating
            // the BIN buffer. Index buffer is uint32 so groups can each
            // index into the global pool by offset.
            uint32_t totalV = 0, totalI = 0;
            std::vector<uint32_t> groupVertOff(bld.groups.size(), 0);
            std::vector<uint32_t> groupIdxOff(bld.groups.size(), 0);
            for (size_t g = 0; g < bld.groups.size(); ++g) {
                groupVertOff[g] = totalV;
                groupIdxOff[g] = totalI;
                totalV += static_cast<uint32_t>(bld.groups[g].vertices.size());
                totalI += static_cast<uint32_t>(bld.groups[g].indices.size());
            }
            if (totalV == 0 || totalI == 0) {
                std::fprintf(stderr, "WOB has no vertex data\n");
                return 1;
            }
            const uint32_t posOff = 0;
            const uint32_t nrmOff = posOff + totalV * 12;
            const uint32_t uvOff  = nrmOff + totalV * 12;
            const uint32_t idxOff = uvOff  + totalV * 8;
            const uint32_t binSize = idxOff + totalI * 4;
            std::vector<uint8_t> bin(binSize);
            // Pack per-group geometry into the global pool. Indices get
            // offset by the group's starting vertex index so they
            // continue to reference the right vertices in the merged pool.
            uint32_t vCursor = 0, iCursor = 0;
            glm::vec3 bMin{1e30f}, bMax{-1e30f};
            for (size_t g = 0; g < bld.groups.size(); ++g) {
                const auto& grp = bld.groups[g];
                for (const auto& v : grp.vertices) {
                    std::memcpy(&bin[posOff + vCursor * 12 + 0], &v.position.x, 4);
                    std::memcpy(&bin[posOff + vCursor * 12 + 4], &v.position.y, 4);
                    std::memcpy(&bin[posOff + vCursor * 12 + 8], &v.position.z, 4);
                    std::memcpy(&bin[nrmOff + vCursor * 12 + 0], &v.normal.x, 4);
                    std::memcpy(&bin[nrmOff + vCursor * 12 + 4], &v.normal.y, 4);
                    std::memcpy(&bin[nrmOff + vCursor * 12 + 8], &v.normal.z, 4);
                    std::memcpy(&bin[uvOff  + vCursor * 8  + 0], &v.texCoord.x, 4);
                    std::memcpy(&bin[uvOff  + vCursor * 8  + 4], &v.texCoord.y, 4);
                    bMin = glm::min(bMin, v.position);
                    bMax = glm::max(bMax, v.position);
                    vCursor++;
                }
                // Offset indices by group's vertex base so merged pool
                // indexing still works. uint32 indices, written LE.
                for (uint32_t idx : grp.indices) {
                    uint32_t off = idx + groupVertOff[g];
                    std::memcpy(&bin[idxOff + iCursor * 4], &off, 4);
                    iCursor++;
                }
            }
            // Build glTF JSON.
            nlohmann::json gj;
            gj["asset"] = {{"version", "2.0"},
                            {"generator", "wowee_editor --export-wob-glb"}};
            gj["scene"] = 0;
            gj["scenes"] = nlohmann::json::array({nlohmann::json{{"nodes", {0}}}});
            gj["nodes"] = nlohmann::json::array({nlohmann::json{
                {"name", bld.name.empty() ? "WoweeBuilding" : bld.name},
                {"mesh", 0}
            }});
            gj["buffers"] = nlohmann::json::array({nlohmann::json{
                {"byteLength", binSize}
            }});
            nlohmann::json bufferViews = nlohmann::json::array();
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", posOff},
                                    {"byteLength", totalV * 12}, {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", nrmOff},
                                    {"byteLength", totalV * 12}, {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", uvOff},
                                    {"byteLength", totalV * 8},  {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", idxOff},
                                    {"byteLength", totalI * 4},  {"target", 34963}});
            gj["bufferViews"] = bufferViews;
            nlohmann::json accessors = nlohmann::json::array();
            accessors.push_back({
                {"bufferView", 0}, {"componentType", 5126},
                {"count", totalV}, {"type", "VEC3"},
                {"min", {bMin.x, bMin.y, bMin.z}},
                {"max", {bMax.x, bMax.y, bMax.z}}
            });
            accessors.push_back({{"bufferView", 1}, {"componentType", 5126},
                                  {"count", totalV}, {"type", "VEC3"}});
            accessors.push_back({{"bufferView", 2}, {"componentType", 5126},
                                  {"count", totalV}, {"type", "VEC2"}});
            // Per-group primitives — each gets its own indices accessor
            // sliced from the shared index bufferView via byteOffset.
            nlohmann::json primitives = nlohmann::json::array();
            for (size_t g = 0; g < bld.groups.size(); ++g) {
                uint32_t accIdx = static_cast<uint32_t>(accessors.size());
                accessors.push_back({
                    {"bufferView", 3},
                    {"byteOffset", groupIdxOff[g] * 4},
                    {"componentType", 5125},
                    {"count", bld.groups[g].indices.size()},
                    {"type", "SCALAR"}
                });
                primitives.push_back({
                    {"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                    {"indices", accIdx},
                    {"mode", 4}
                });
            }
            gj["accessors"] = accessors;
            gj["meshes"] = nlohmann::json::array({nlohmann::json{
                {"primitives", primitives}
            }});
            std::string jsonStr = gj.dump();
            while (jsonStr.size() % 4 != 0) jsonStr += ' ';
            uint32_t jsonLen = static_cast<uint32_t>(jsonStr.size());
            uint32_t binLen = binSize;
            uint32_t totalLen = 12 + 8 + jsonLen + 8 + binLen;
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "Failed to open output: %s\n", outPath.c_str());
                return 1;
            }
            uint32_t magic = 0x46546C67;
            uint32_t version = 2;
            out.write(reinterpret_cast<const char*>(&magic), 4);
            out.write(reinterpret_cast<const char*>(&version), 4);
            out.write(reinterpret_cast<const char*>(&totalLen), 4);
            uint32_t jsonChunkType = 0x4E4F534A;
            out.write(reinterpret_cast<const char*>(&jsonLen), 4);
            out.write(reinterpret_cast<const char*>(&jsonChunkType), 4);
            out.write(jsonStr.data(), jsonLen);
            uint32_t binChunkType = 0x004E4942;
            out.write(reinterpret_cast<const char*>(&binLen), 4);
            out.write(reinterpret_cast<const char*>(&binChunkType), 4);
            out.write(reinterpret_cast<const char*>(bin.data()), binLen);
            out.close();
            std::printf("Exported %s.wob -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  %zu groups -> %zu primitives, %u verts, %u tris, %u-byte BIN\n",
                        bld.groups.size(), primitives.size(),
                        totalV, totalI / 3, binLen);
            return 0;
        } else if (std::strcmp(argv[i], "--export-whm-glb") == 0 && i + 1 < argc) {
            // glTF 2.0 binary export for WHM/WOT terrain. Mirrors
            // --export-whm-obj's mesh layout (9x9 outer grid per chunk
            // → 8x8 quads → 2 tris each), but ships as a single .glb
            // viewable in any modern web 3D tool. Per-chunk primitives
            // so designers can hide individual chunks in three.js.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
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
            if (outPath.empty()) outPath = base + ".glb";
            wowee::pipeline::ADTTerrain terrain;
            wowee::pipeline::WoweeTerrainLoader::load(base, terrain);
            // Same coord constants as --export-whm-obj so the .glb and
            // .obj of the same source align spatially.
            constexpr float kTileSize = 533.33333f;
            constexpr float kChunkSize = kTileSize / 16.0f;
            constexpr float kVertSpacing = kChunkSize / 8.0f;
            // Walk the 16x16 chunk grid, build per-chunk vertex + index
            // arrays. Hole bits respected (cave-entrance quads dropped).
            struct ChunkMesh { uint32_t vertOff, vertCount, idxOff, idxCount; };
            std::vector<ChunkMesh> chunkMeshes;
            std::vector<glm::vec3> positions;  // packed sequentially
            std::vector<uint32_t>  indices;
            int loadedChunks = 0;
            glm::vec3 bMin{1e30f}, bMax{-1e30f};
            for (int cx = 0; cx < 16; ++cx) {
                for (int cy = 0; cy < 16; ++cy) {
                    const auto& chunk = terrain.getChunk(cx, cy);
                    if (!chunk.heightMap.isLoaded()) continue;
                    loadedChunks++;
                    ChunkMesh cm{};
                    cm.vertOff = static_cast<uint32_t>(positions.size());
                    cm.idxOff  = static_cast<uint32_t>(indices.size());
                    float chunkBaseX = (32.0f - terrain.coord.y) * kTileSize - cy * kChunkSize;
                    float chunkBaseY = (32.0f - terrain.coord.x) * kTileSize - cx * kChunkSize;
                    // 9x9 outer verts (skip 8x8 inner fan-center verts).
                    for (int row = 0; row < 9; ++row) {
                        for (int col = 0; col < 9; ++col) {
                            glm::vec3 p{
                                chunkBaseX - row * kVertSpacing,
                                chunkBaseY - col * kVertSpacing,
                                chunk.position[2] + chunk.heightMap.heights[row * 17 + col]
                            };
                            positions.push_back(p);
                            bMin = glm::min(bMin, p);
                            bMax = glm::max(bMax, p);
                        }
                    }
                    cm.vertCount = 81;
                    bool isHoleChunk = (chunk.holes != 0);
                    auto idx = [&](int r, int c) { return cm.vertOff + r * 9 + c; };
                    for (int row = 0; row < 8; ++row) {
                        for (int col = 0; col < 8; ++col) {
                            if (isHoleChunk) {
                                int hx = col / 2, hy = row / 2;
                                if (chunk.holes & (1 << (hy * 4 + hx))) continue;
                            }
                            indices.push_back(idx(row, col));
                            indices.push_back(idx(row, col + 1));
                            indices.push_back(idx(row + 1, col + 1));
                            indices.push_back(idx(row, col));
                            indices.push_back(idx(row + 1, col + 1));
                            indices.push_back(idx(row + 1, col));
                        }
                    }
                    cm.idxCount = static_cast<uint32_t>(indices.size()) - cm.idxOff;
                    chunkMeshes.push_back(cm);
                }
            }
            if (loadedChunks == 0) {
                std::fprintf(stderr, "WHM has no loaded chunks\n");
                return 1;
            }
            // Synthesize normals as +Z (terrain is Z-up). Real per-vertex
            // normals would need a smoothing pass across chunk boundaries
            // — skip for v1, viewers can compute their own from positions.
            const uint32_t totalV = static_cast<uint32_t>(positions.size());
            const uint32_t totalI = static_cast<uint32_t>(indices.size());
            const uint32_t posOff = 0;
            const uint32_t nrmOff = posOff + totalV * 12;
            const uint32_t idxOff = nrmOff + totalV * 12;
            const uint32_t binSize = idxOff + totalI * 4;
            std::vector<uint8_t> bin(binSize);
            for (uint32_t v = 0; v < totalV; ++v) {
                std::memcpy(&bin[posOff + v * 12 + 0], &positions[v].x, 4);
                std::memcpy(&bin[posOff + v * 12 + 4], &positions[v].y, 4);
                std::memcpy(&bin[posOff + v * 12 + 8], &positions[v].z, 4);
                float nx = 0, ny = 0, nz = 1;
                std::memcpy(&bin[nrmOff + v * 12 + 0], &nx, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 4], &ny, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 8], &nz, 4);
            }
            std::memcpy(&bin[idxOff], indices.data(), totalI * 4);
            // Build glTF JSON.
            nlohmann::json gj;
            gj["asset"] = {{"version", "2.0"},
                            {"generator", "wowee_editor --export-whm-glb"}};
            gj["scene"] = 0;
            gj["scenes"] = nlohmann::json::array({nlohmann::json{{"nodes", {0}}}});
            std::string nodeName = "WoweeTerrain_" + std::to_string(terrain.coord.x) +
                                    "_" + std::to_string(terrain.coord.y);
            gj["nodes"] = nlohmann::json::array({nlohmann::json{
                {"name", nodeName}, {"mesh", 0}
            }});
            gj["buffers"] = nlohmann::json::array({nlohmann::json{
                {"byteLength", binSize}
            }});
            nlohmann::json bufferViews = nlohmann::json::array();
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", posOff},
                                    {"byteLength", totalV * 12}, {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", nrmOff},
                                    {"byteLength", totalV * 12}, {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", idxOff},
                                    {"byteLength", totalI * 4},  {"target", 34963}});
            gj["bufferViews"] = bufferViews;
            nlohmann::json accessors = nlohmann::json::array();
            accessors.push_back({
                {"bufferView", 0}, {"componentType", 5126},
                {"count", totalV}, {"type", "VEC3"},
                {"min", {bMin.x, bMin.y, bMin.z}},
                {"max", {bMax.x, bMax.y, bMax.z}}
            });
            accessors.push_back({{"bufferView", 1}, {"componentType", 5126},
                                  {"count", totalV}, {"type", "VEC3"}});
            // Per-chunk primitive — sliced from shared index bufferView.
            nlohmann::json primitives = nlohmann::json::array();
            for (const auto& cm : chunkMeshes) {
                if (cm.idxCount == 0) continue;  // all-hole chunk
                uint32_t accIdx = static_cast<uint32_t>(accessors.size());
                accessors.push_back({
                    {"bufferView", 2},
                    {"byteOffset", cm.idxOff * 4},
                    {"componentType", 5125},
                    {"count", cm.idxCount},
                    {"type", "SCALAR"}
                });
                primitives.push_back({
                    {"attributes", {{"POSITION", 0}, {"NORMAL", 1}}},
                    {"indices", accIdx},
                    {"mode", 4}
                });
            }
            gj["accessors"] = accessors;
            gj["meshes"] = nlohmann::json::array({nlohmann::json{
                {"primitives", primitives}
            }});
            std::string jsonStr = gj.dump();
            while (jsonStr.size() % 4 != 0) jsonStr += ' ';
            uint32_t jsonLen = static_cast<uint32_t>(jsonStr.size());
            uint32_t binLen = binSize;
            uint32_t totalLen = 12 + 8 + jsonLen + 8 + binLen;
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "Failed to open output: %s\n", outPath.c_str());
                return 1;
            }
            uint32_t magic = 0x46546C67, version = 2;
            out.write(reinterpret_cast<const char*>(&magic), 4);
            out.write(reinterpret_cast<const char*>(&version), 4);
            out.write(reinterpret_cast<const char*>(&totalLen), 4);
            uint32_t jsonChunkType = 0x4E4F534A;
            out.write(reinterpret_cast<const char*>(&jsonLen), 4);
            out.write(reinterpret_cast<const char*>(&jsonChunkType), 4);
            out.write(jsonStr.data(), jsonLen);
            uint32_t binChunkType = 0x004E4942;
            out.write(reinterpret_cast<const char*>(&binLen), 4);
            out.write(reinterpret_cast<const char*>(&binChunkType), 4);
            out.write(reinterpret_cast<const char*>(bin.data()), binLen);
            out.close();
            std::printf("Exported %s.whm -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  %d chunks loaded, %u verts, %u tris, %zu primitives, %u-byte BIN\n",
                        loadedChunks, totalV, totalI / 3, primitives.size(), binLen);
            return 0;
        } else if (std::strcmp(argv[i], "--bake-zone-glb") == 0 && i + 1 < argc) {
            // Bake every WHM tile in a zone into ONE .glb so the whole
            // multi-tile zone opens in three.js / model-viewer with one
            // file. Each tile becomes its own mesh+node so they can be
            // toggled independently. v1: terrain only — object/WOB
            // instances are a follow-up that needs careful per-mesh
            // bufferView slicing.
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "bake-zone-glb: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr,
                    "bake-zone-glb: failed to parse zone.json\n");
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/" + zm.mapName + ".glb";
            if (zm.tiles.empty()) {
                std::fprintf(stderr, "bake-zone-glb: zone has no tiles\n");
                return 1;
            }
            constexpr float kTileSize = 533.33333f;
            constexpr float kChunkSize = kTileSize / 16.0f;
            constexpr float kVertSpacing = kChunkSize / 8.0f;
            // Per-tile mesh metadata so we can create one node per tile
            // and slice its index range from the shared bufferView.
            struct TileMesh {
                int tx, ty;
                uint32_t vertOff, vertCount;
                uint32_t idxOff, idxCount;
            };
            std::vector<TileMesh> tileMeshes;
            std::vector<glm::vec3> positions;
            std::vector<uint32_t>  indices;
            int loadedTiles = 0;
            glm::vec3 bMin{1e30f}, bMax{-1e30f};
            for (const auto& [tx, ty] : zm.tiles) {
                std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                       std::to_string(tx) + "_" + std::to_string(ty);
                if (!wowee::pipeline::WoweeTerrainLoader::exists(tileBase)) {
                    std::fprintf(stderr,
                        "bake-zone-glb: tile (%d,%d) WHM/WOT missing — skipping\n",
                        tx, ty);
                    continue;
                }
                wowee::pipeline::ADTTerrain terrain;
                wowee::pipeline::WoweeTerrainLoader::load(tileBase, terrain);
                TileMesh tm{tx, ty, 0, 0, 0, 0};
                tm.vertOff = static_cast<uint32_t>(positions.size());
                tm.idxOff  = static_cast<uint32_t>(indices.size());
                // Same per-chunk outer-grid layout as --export-whm-glb,
                // but accumulated across all tiles so they share one
                // global vertex+index pool.
                for (int cx = 0; cx < 16; ++cx) {
                    for (int cy = 0; cy < 16; ++cy) {
                        const auto& chunk = terrain.getChunk(cx, cy);
                        if (!chunk.heightMap.isLoaded()) continue;
                        float chunkBaseX = (32.0f - terrain.coord.y) * kTileSize - cy * kChunkSize;
                        float chunkBaseY = (32.0f - terrain.coord.x) * kTileSize - cx * kChunkSize;
                        uint32_t chunkVertOff =
                            static_cast<uint32_t>(positions.size());
                        for (int row = 0; row < 9; ++row) {
                            for (int col = 0; col < 9; ++col) {
                                glm::vec3 p{
                                    chunkBaseX - row * kVertSpacing,
                                    chunkBaseY - col * kVertSpacing,
                                    chunk.position[2] +
                                        chunk.heightMap.heights[row * 17 + col]
                                };
                                positions.push_back(p);
                                bMin = glm::min(bMin, p);
                                bMax = glm::max(bMax, p);
                            }
                        }
                        bool isHoleChunk = (chunk.holes != 0);
                        for (int row = 0; row < 8; ++row) {
                            for (int col = 0; col < 8; ++col) {
                                if (isHoleChunk) {
                                    int hx = col / 2, hy = row / 2;
                                    if (chunk.holes & (1 << (hy * 4 + hx))) continue;
                                }
                                auto idx = [&](int r, int c) {
                                    return chunkVertOff + r * 9 + c;
                                };
                                indices.push_back(idx(row, col));
                                indices.push_back(idx(row, col + 1));
                                indices.push_back(idx(row + 1, col + 1));
                                indices.push_back(idx(row, col));
                                indices.push_back(idx(row + 1, col + 1));
                                indices.push_back(idx(row + 1, col));
                            }
                        }
                    }
                }
                tm.vertCount = static_cast<uint32_t>(positions.size()) - tm.vertOff;
                tm.idxCount  = static_cast<uint32_t>(indices.size()) - tm.idxOff;
                if (tm.vertCount > 0 && tm.idxCount > 0) {
                    tileMeshes.push_back(tm);
                    loadedTiles++;
                }
            }
            if (loadedTiles == 0) {
                std::fprintf(stderr, "bake-zone-glb: no tiles loaded\n");
                return 1;
            }
            // Pack BIN chunk same way as --export-whm-glb (positions +
            // synthetic +Z normals + indices). Per-tile accessors slice
            // their index region via byteOffset.
            const uint32_t totalV = static_cast<uint32_t>(positions.size());
            const uint32_t totalI = static_cast<uint32_t>(indices.size());
            const uint32_t posOff = 0;
            const uint32_t nrmOff = posOff + totalV * 12;
            const uint32_t idxOff = nrmOff + totalV * 12;
            const uint32_t binSize = idxOff + totalI * 4;
            std::vector<uint8_t> bin(binSize);
            for (uint32_t v = 0; v < totalV; ++v) {
                std::memcpy(&bin[posOff + v * 12 + 0], &positions[v].x, 4);
                std::memcpy(&bin[posOff + v * 12 + 4], &positions[v].y, 4);
                std::memcpy(&bin[posOff + v * 12 + 8], &positions[v].z, 4);
                float nx = 0, ny = 0, nz = 1;
                std::memcpy(&bin[nrmOff + v * 12 + 0], &nx, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 4], &ny, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 8], &nz, 4);
            }
            std::memcpy(&bin[idxOff], indices.data(), totalI * 4);
            // Build glTF JSON. One mesh + one node per tile so they can
            // be toggled in viewers.
            nlohmann::json gj;
            gj["asset"] = {{"version", "2.0"},
                            {"generator", "wowee_editor --bake-zone-glb"}};
            gj["scene"] = 0;
            gj["buffers"] = nlohmann::json::array({nlohmann::json{
                {"byteLength", binSize}
            }});
            // Three shared bufferViews — pos, nrm, idx — sliced into
            // per-tile primitives via byteOffset on the index accessor.
            nlohmann::json bufferViews = nlohmann::json::array();
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", posOff},
                                    {"byteLength", totalV * 12}, {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", nrmOff},
                                    {"byteLength", totalV * 12}, {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", idxOff},
                                    {"byteLength", totalI * 4},  {"target", 34963}});
            gj["bufferViews"] = bufferViews;
            // Shared position+normal accessors (covering the full pool;
            // primitives reference them, the index accessor does the
            // per-tile slicing).
            nlohmann::json accessors = nlohmann::json::array();
            accessors.push_back({
                {"bufferView", 0}, {"componentType", 5126},
                {"count", totalV}, {"type", "VEC3"},
                {"min", {bMin.x, bMin.y, bMin.z}},
                {"max", {bMax.x, bMax.y, bMax.z}}
            });
            accessors.push_back({{"bufferView", 1}, {"componentType", 5126},
                                  {"count", totalV}, {"type", "VEC3"}});
            // Per-tile mesh + node + indices accessor.
            nlohmann::json meshes = nlohmann::json::array();
            nlohmann::json nodes = nlohmann::json::array();
            nlohmann::json sceneNodes = nlohmann::json::array();
            for (const auto& tm : tileMeshes) {
                uint32_t accIdx = static_cast<uint32_t>(accessors.size());
                accessors.push_back({
                    {"bufferView", 2},
                    {"byteOffset", tm.idxOff * 4},
                    {"componentType", 5125},
                    {"count", tm.idxCount},
                    {"type", "SCALAR"}
                });
                uint32_t meshIdx = static_cast<uint32_t>(meshes.size());
                meshes.push_back({
                    {"primitives", nlohmann::json::array({nlohmann::json{
                        {"attributes", {{"POSITION", 0}, {"NORMAL", 1}}},
                        {"indices", accIdx}, {"mode", 4}
                    }})}
                });
                std::string nodeName = "tile_" + std::to_string(tm.tx) +
                                       "_" + std::to_string(tm.ty);
                uint32_t nodeIdx = static_cast<uint32_t>(nodes.size());
                nodes.push_back({{"name", nodeName}, {"mesh", meshIdx}});
                sceneNodes.push_back(nodeIdx);
            }
            gj["accessors"] = accessors;
            gj["meshes"] = meshes;
            gj["nodes"] = nodes;
            gj["scenes"] = nlohmann::json::array({nlohmann::json{
                {"nodes", sceneNodes}
            }});
            std::string jsonStr = gj.dump();
            while (jsonStr.size() % 4 != 0) jsonStr += ' ';
            uint32_t jsonLen = static_cast<uint32_t>(jsonStr.size());
            uint32_t binLen = binSize;
            uint32_t totalLen = 12 + 8 + jsonLen + 8 + binLen;
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "Failed to open output: %s\n", outPath.c_str());
                return 1;
            }
            uint32_t magic = 0x46546C67, version = 2;
            out.write(reinterpret_cast<const char*>(&magic), 4);
            out.write(reinterpret_cast<const char*>(&version), 4);
            out.write(reinterpret_cast<const char*>(&totalLen), 4);
            uint32_t jsonChunkType = 0x4E4F534A;
            out.write(reinterpret_cast<const char*>(&jsonLen), 4);
            out.write(reinterpret_cast<const char*>(&jsonChunkType), 4);
            out.write(jsonStr.data(), jsonLen);
            uint32_t binChunkType = 0x004E4942;
            out.write(reinterpret_cast<const char*>(&binLen), 4);
            out.write(reinterpret_cast<const char*>(&binChunkType), 4);
            out.write(reinterpret_cast<const char*>(bin.data()), binLen);
            out.close();
            std::printf("Baked %s -> %s\n", zoneDir.c_str(), outPath.c_str());
            std::printf("  %d tile(s), %u verts, %u tris, %zu meshes, %u-byte BIN\n",
                        loadedTiles, totalV, totalI / 3,
                        meshes.size(), binLen);
            return 0;
        } else if (std::strcmp(argv[i], "--bake-zone-stl") == 0 && i + 1 < argc) {
            // STL counterpart to --bake-zone-glb. Designers can 3D-print a
            // miniature of an entire multi-tile zone in one slicer load —
            // useful for tabletop RPG props or a physical reference of a
            // playtest area.
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "bake-zone-stl: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr,
                    "bake-zone-stl: failed to parse zone.json\n");
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/" + zm.mapName + ".stl";
            if (zm.tiles.empty()) {
                std::fprintf(stderr, "bake-zone-stl: zone has no tiles\n");
                return 1;
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr, "bake-zone-stl: cannot write %s\n", outPath.c_str());
                return 1;
            }
            constexpr float kTileSize = 533.33333f;
            constexpr float kChunkSize = kTileSize / 16.0f;
            constexpr float kVertSpacing = kChunkSize / 8.0f;
            // Solid name sanitized to alphanum + underscore.
            std::string solidName = zm.mapName;
            for (auto& c : solidName) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_')) c = '_';
            }
            if (solidName.empty()) solidName = "wowee_zone";
            out << "solid " << solidName << "\n";
            int loadedTiles = 0, holesSkipped = 0;
            uint64_t triCount = 0;
            // For each tile, generate the same 9x9 outer-grid mesh and
            // emit per-triangle facets directly (STL has no shared
            // vertex pool — each triangle stands alone). Compute face
            // normal from cross product (slicers use it for orientation).
            for (const auto& [tx, ty] : zm.tiles) {
                std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                       std::to_string(tx) + "_" + std::to_string(ty);
                if (!wowee::pipeline::WoweeTerrainLoader::exists(tileBase)) {
                    std::fprintf(stderr,
                        "bake-zone-stl: tile (%d, %d) WHM/WOT missing — skipping\n",
                        tx, ty);
                    continue;
                }
                wowee::pipeline::ADTTerrain terrain;
                wowee::pipeline::WoweeTerrainLoader::load(tileBase, terrain);
                loadedTiles++;
                for (int cx = 0; cx < 16; ++cx) {
                    for (int cy = 0; cy < 16; ++cy) {
                        const auto& chunk = terrain.getChunk(cx, cy);
                        if (!chunk.heightMap.isLoaded()) continue;
                        float chunkBaseX = (32.0f - terrain.coord.y) * kTileSize - cy * kChunkSize;
                        float chunkBaseY = (32.0f - terrain.coord.x) * kTileSize - cx * kChunkSize;
                        // Pre-compute the 9x9 vertex grid for this chunk.
                        glm::vec3 V[9][9];
                        for (int row = 0; row < 9; ++row) {
                            for (int col = 0; col < 9; ++col) {
                                V[row][col] = {
                                    chunkBaseX - row * kVertSpacing,
                                    chunkBaseY - col * kVertSpacing,
                                    chunk.position[2] +
                                        chunk.heightMap.heights[row * 17 + col]
                                };
                            }
                        }
                        bool isHoleChunk = (chunk.holes != 0);
                        auto emitTri = [&](const glm::vec3& a,
                                           const glm::vec3& b,
                                           const glm::vec3& c) {
                            glm::vec3 e1 = b - a, e2 = c - a;
                            glm::vec3 n = glm::cross(e1, e2);
                            float len = glm::length(n);
                            if (len > 1e-12f) n /= len;
                            else n = {0, 0, 1};
                            out << "  facet normal " << n.x << " " << n.y << " " << n.z << "\n"
                                << "    outer loop\n"
                                << "      vertex " << a.x << " " << a.y << " " << a.z << "\n"
                                << "      vertex " << b.x << " " << b.y << " " << b.z << "\n"
                                << "      vertex " << c.x << " " << c.y << " " << c.z << "\n"
                                << "    endloop\n"
                                << "  endfacet\n";
                            triCount++;
                        };
                        for (int row = 0; row < 8; ++row) {
                            for (int col = 0; col < 8; ++col) {
                                if (isHoleChunk) {
                                    int hx = col / 2, hy = row / 2;
                                    if (chunk.holes & (1 << (hy * 4 + hx))) {
                                        holesSkipped++;
                                        continue;
                                    }
                                }
                                emitTri(V[row][col], V[row][col + 1], V[row + 1][col + 1]);
                                emitTri(V[row][col], V[row + 1][col + 1], V[row + 1][col]);
                            }
                        }
                    }
                }
            }
            out << "endsolid " << solidName << "\n";
            out.close();
            if (loadedTiles == 0) {
                std::fprintf(stderr, "bake-zone-stl: no tiles loaded\n");
                std::filesystem::remove(outPath);
                return 1;
            }
            std::printf("Baked %s -> %s\n", zoneDir.c_str(), outPath.c_str());
            std::printf("  %d tile(s), %llu facets, %d hole quads skipped\n",
                        loadedTiles, static_cast<unsigned long long>(triCount),
                        holesSkipped);
            return 0;
        } else if (std::strcmp(argv[i], "--bake-zone-obj") == 0 && i + 1 < argc) {
            // OBJ companion to --bake-zone-glb / --bake-zone-stl. Same
            // multi-tile WHM aggregation, but as Wavefront OBJ — opens
            // directly in Blender / MeshLab / 3DS Max for hand-editing.
            // Each tile becomes its own 'g' block so designers can hide
            // tiles independently.
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "bake-zone-obj: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "bake-zone-obj: parse failed\n");
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/" + zm.mapName + ".obj";
            if (zm.tiles.empty()) {
                std::fprintf(stderr, "bake-zone-obj: zone has no tiles\n");
                return 1;
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr, "bake-zone-obj: cannot write %s\n", outPath.c_str());
                return 1;
            }
            constexpr float kTileSize = 533.33333f;
            constexpr float kChunkSize = kTileSize / 16.0f;
            constexpr float kVertSpacing = kChunkSize / 8.0f;
            out << "# Wavefront OBJ generated by wowee_editor --bake-zone-obj\n";
            out << "# Zone: " << zm.mapName << " (" << zm.tiles.size()
                << " tiles)\n";
            out << "o " << zm.mapName << "\n";
            // OBJ uses a single global vertex pool with per-tile g-blocks
            // and per-tile face index offsetting. We accumulate per-tile
            // vertex blocks first (so face indices know their offsets),
            // then per-tile face blocks at the end.
            // Layout: emit ALL verts first (organized by tile, in order),
            // then emit ALL face blocks. OBJ requires verts before faces
            // that reference them.
            int loadedTiles = 0;
            int totalVerts = 0;
            // Per-tile bookkeeping: vertex base index (1-based for OBJ)
            // and which faces reference it.
            struct TileMeta {
                int tx, ty;
                uint32_t vertBase;  // 1-based OBJ index of first vert
                uint32_t vertCount;
                std::vector<uint32_t> faceI0, faceI1, faceI2;  // local indices
            };
            std::vector<TileMeta> tiles;
            for (const auto& [tx, ty] : zm.tiles) {
                std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                       std::to_string(tx) + "_" + std::to_string(ty);
                if (!wowee::pipeline::WoweeTerrainLoader::exists(tileBase)) {
                    std::fprintf(stderr,
                        "bake-zone-obj: tile (%d, %d) WHM/WOT missing — skipping\n",
                        tx, ty);
                    continue;
                }
                wowee::pipeline::ADTTerrain terrain;
                wowee::pipeline::WoweeTerrainLoader::load(tileBase, terrain);
                TileMeta tm{tx, ty, static_cast<uint32_t>(totalVerts + 1), 0, {}, {}, {}};
                // Walk chunks; emit verts to file as we go (so we don't
                // hold a giant vector in memory). Track local indices for
                // face emission afterwards.
                uint32_t tileLocalIdx = 0;
                for (int cx = 0; cx < 16; ++cx) {
                    for (int cy = 0; cy < 16; ++cy) {
                        const auto& chunk = terrain.getChunk(cx, cy);
                        if (!chunk.heightMap.isLoaded()) continue;
                        float chunkBaseX = (32.0f - terrain.coord.y) * kTileSize - cy * kChunkSize;
                        float chunkBaseY = (32.0f - terrain.coord.x) * kTileSize - cx * kChunkSize;
                        uint32_t chunkBaseLocal = tileLocalIdx;
                        for (int row = 0; row < 9; ++row) {
                            for (int col = 0; col < 9; ++col) {
                                float x = chunkBaseX - row * kVertSpacing;
                                float y = chunkBaseY - col * kVertSpacing;
                                float z = chunk.position[2] +
                                          chunk.heightMap.heights[row * 17 + col];
                                out << "v " << x << " " << y << " " << z << "\n";
                                tileLocalIdx++;
                            }
                        }
                        bool isHoleChunk = (chunk.holes != 0);
                        for (int row = 0; row < 8; ++row) {
                            for (int col = 0; col < 8; ++col) {
                                if (isHoleChunk) {
                                    int hx = col / 2, hy = row / 2;
                                    if (chunk.holes & (1 << (hy * 4 + hx))) continue;
                                }
                                auto idx = [&](int r, int c) {
                                    return chunkBaseLocal + r * 9 + c;
                                };
                                tm.faceI0.push_back(idx(row, col));
                                tm.faceI1.push_back(idx(row, col + 1));
                                tm.faceI2.push_back(idx(row + 1, col + 1));
                                tm.faceI0.push_back(idx(row, col));
                                tm.faceI1.push_back(idx(row + 1, col + 1));
                                tm.faceI2.push_back(idx(row + 1, col));
                            }
                        }
                    }
                }
                tm.vertCount = tileLocalIdx;
                totalVerts += tm.vertCount;
                if (tm.vertCount > 0) {
                    tiles.push_back(std::move(tm));
                    loadedTiles++;
                }
            }
            // Now emit per-tile face groups (after all verts are written).
            uint64_t totalFaces = 0;
            for (const auto& tm : tiles) {
                out << "g tile_" << tm.tx << "_" << tm.ty << "\n";
                for (size_t k = 0; k < tm.faceI0.size(); ++k) {
                    uint32_t a = tm.faceI0[k] + tm.vertBase;
                    uint32_t b = tm.faceI1[k] + tm.vertBase;
                    uint32_t c = tm.faceI2[k] + tm.vertBase;
                    out << "f " << a << " " << b << " " << c << "\n";
                    totalFaces++;
                }
            }
            out.close();
            if (loadedTiles == 0) {
                std::fprintf(stderr, "bake-zone-obj: no tiles loaded\n");
                std::filesystem::remove(outPath);
                return 1;
            }
            std::printf("Baked %s -> %s\n", zoneDir.c_str(), outPath.c_str());
            std::printf("  %d tile(s), %d verts, %llu tris\n",
                        loadedTiles, totalVerts,
                        static_cast<unsigned long long>(totalFaces));
            return 0;
        } else if (std::strcmp(argv[i], "--export-wob-obj") == 0 && i + 1 < argc) {
            // WOB is the WMO replacement; like --export-obj for WOM, this
            // bridges WOB into the universal-3D-tool ecosystem. Each WOB
            // group becomes one OBJ 'g' block, preserving the room/floor
            // structure for downstream selection in Blender/MeshLab.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wob")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeBuildingLoader::exists(base)) {
                std::fprintf(stderr, "WOB not found: %s.wob\n", base.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = base + ".obj";
            auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
            if (!bld.isValid()) {
                std::fprintf(stderr, "WOB has no groups to export: %s.wob\n", base.c_str());
                return 1;
            }
            std::ofstream obj(outPath);
            if (!obj) {
                std::fprintf(stderr, "Failed to open output file: %s\n", outPath.c_str());
                return 1;
            }
            // Total verts/tris across all groups for the header.
            size_t totalV = 0, totalI = 0;
            for (const auto& g : bld.groups) {
                totalV += g.vertices.size();
                totalI += g.indices.size();
            }
            obj << "# Wavefront OBJ generated by wowee_editor --export-wob-obj\n";
            obj << "# Source: " << base << ".wob\n";
            obj << "# Groups: " << bld.groups.size()
                << " Verts: " << totalV
                << " Tris: " << totalI / 3
                << " Portals: " << bld.portals.size()
                << " Doodads: " << bld.doodads.size() << "\n\n";
            obj << "o " << (bld.name.empty() ? "WoweeBuilding" : bld.name) << "\n";
            // OBJ uses a single global vertex pool, so we offset each group's
            // local indices by the running total of verts written so far.
            uint32_t vertOffset = 0;
            for (size_t g = 0; g < bld.groups.size(); ++g) {
                const auto& grp = bld.groups[g];
                if (grp.vertices.empty()) continue;
                for (const auto& v : grp.vertices) {
                    obj << "v "  << v.position.x << " "
                                  << v.position.y << " "
                                  << v.position.z << "\n";
                }
                for (const auto& v : grp.vertices) {
                    obj << "vt " << v.texCoord.x << " "
                                  << (1.0f - v.texCoord.y) << "\n";
                }
                for (const auto& v : grp.vertices) {
                    obj << "vn " << v.normal.x << " "
                                  << v.normal.y << " "
                                  << v.normal.z << "\n";
                }
                std::string groupName = grp.name.empty()
                    ? "group_" + std::to_string(g)
                    : grp.name;
                if (grp.isOutdoor) groupName += "_outdoor";
                obj << "g " << groupName << "\n";
                for (size_t k = 0; k + 2 < grp.indices.size(); k += 3) {
                    uint32_t i0 = grp.indices[k]     + 1 + vertOffset;
                    uint32_t i1 = grp.indices[k + 1] + 1 + vertOffset;
                    uint32_t i2 = grp.indices[k + 2] + 1 + vertOffset;
                    obj << "f "
                        << i0 << "/" << i0 << "/" << i0 << " "
                        << i1 << "/" << i1 << "/" << i1 << " "
                        << i2 << "/" << i2 << "/" << i2 << "\n";
                }
                vertOffset += static_cast<uint32_t>(grp.vertices.size());
            }
            // Doodad placements as a separate informational block — emit
            // each as a comment line so OBJ stays valid but the data is
            // recoverable for tools that want to re-create the placements.
            if (!bld.doodads.empty()) {
                obj << "\n# Doodad placements (model, position, rotation, scale):\n";
                for (const auto& d : bld.doodads) {
                    obj << "# doodad " << d.modelPath
                        << " pos " << d.position.x << "," << d.position.y << "," << d.position.z
                        << " rot " << d.rotation.x << "," << d.rotation.y << "," << d.rotation.z
                        << " scale " << d.scale << "\n";
                }
            }
            obj.close();
            std::printf("Exported %s.wob -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  %zu groups, %zu verts, %zu tris, %zu doodad placements\n",
                        bld.groups.size(), totalV, totalI / 3,
                        bld.doodads.size());
            return 0;
        } else if (std::strcmp(argv[i], "--import-wob-obj") == 0 && i + 1 < argc) {
            // Round-trip companion to --export-wob-obj. Each OBJ 'g' block
            // becomes one WoweeBuilding::Group; geometry under that group
            // is deduped into the group's local vertex array. Faces
            // before any 'g' directive land in a default 'imported' group.
            // Doodad placements written as # comment lines by --export-wob-obj
            // ARE recognized and re-instanced into bld.doodads.
            std::string objPath = argv[++i];
            std::string wobBase;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                wobBase = argv[++i];
            }
            if (!std::filesystem::exists(objPath)) {
                std::fprintf(stderr, "OBJ not found: %s\n", objPath.c_str());
                return 1;
            }
            if (wobBase.empty()) {
                wobBase = objPath;
                if (wobBase.size() >= 4 &&
                    wobBase.substr(wobBase.size() - 4) == ".obj") {
                    wobBase = wobBase.substr(0, wobBase.size() - 4);
                }
            }
            std::ifstream in(objPath);
            if (!in) {
                std::fprintf(stderr, "Failed to open OBJ: %s\n", objPath.c_str());
                return 1;
            }
            // Global pools (OBJ vertex/uv/normal indices reference these
            // across all groups).
            std::vector<glm::vec3> positions;
            std::vector<glm::vec2> texcoords;
            std::vector<glm::vec3> normals;
            wowee::pipeline::WoweeBuilding bld;
            // Active group bookkeeping: dedupe table is per-group since
            // each WOB group has its own local vertex buffer.
            std::string activeGroup = "imported";
            std::unordered_map<std::string, uint32_t> groupDedupe;
            int activeGroupIdx = -1;
            int badFaces = 0;
            int triangulatedNgons = 0;
            std::string objectName;
            auto ensureActiveGroup = [&]() {
                if (activeGroupIdx >= 0) return;
                wowee::pipeline::WoweeBuilding::Group g;
                g.name = activeGroup;
                if (g.name.size() >= 8 &&
                    g.name.substr(g.name.size() - 8) == "_outdoor") {
                    g.name = g.name.substr(0, g.name.size() - 8);
                    g.isOutdoor = true;
                }
                bld.groups.push_back(g);
                activeGroupIdx = static_cast<int>(bld.groups.size()) - 1;
                groupDedupe.clear();
            };
            auto resolveCorner = [&](const std::string& token) -> int {
                int v = 0, t = 0, n = 0;
                {
                    const char* p = token.c_str();
                    char* endp = nullptr;
                    v = std::strtol(p, &endp, 10);
                    if (*endp == '/') {
                        ++endp;
                        if (*endp != '/') t = std::strtol(endp, &endp, 10);
                        if (*endp == '/') {
                            ++endp;
                            n = std::strtol(endp, &endp, 10);
                        }
                    }
                }
                auto absIdx = [](int idx, size_t pool) {
                    if (idx < 0) return static_cast<int>(pool) + idx;
                    return idx - 1;
                };
                int vi = absIdx(v, positions.size());
                int ti = (t == 0) ? -1 : absIdx(t, texcoords.size());
                int ni = (n == 0) ? -1 : absIdx(n, normals.size());
                if (vi < 0 || vi >= static_cast<int>(positions.size())) return -1;
                ensureActiveGroup();
                std::string key = std::to_string(vi) + "/" +
                                  std::to_string(ti) + "/" +
                                  std::to_string(ni);
                auto it = groupDedupe.find(key);
                if (it != groupDedupe.end()) return static_cast<int>(it->second);
                wowee::pipeline::WoweeBuilding::Vertex vert;
                vert.position = positions[vi];
                if (ti >= 0 && ti < static_cast<int>(texcoords.size())) {
                    vert.texCoord = texcoords[ti];
                    // Reverse the V-flip from --export-wob-obj.
                    vert.texCoord.y = 1.0f - vert.texCoord.y;
                } else {
                    vert.texCoord = {0, 0};
                }
                if (ni >= 0 && ni < static_cast<int>(normals.size())) {
                    vert.normal = normals[ni];
                } else {
                    vert.normal = {0, 0, 1};
                }
                vert.color = {1, 1, 1, 1};
                auto& grp = bld.groups[activeGroupIdx];
                uint32_t newIdx = static_cast<uint32_t>(grp.vertices.size());
                grp.vertices.push_back(vert);
                groupDedupe[key] = newIdx;
                return static_cast<int>(newIdx);
            };
            std::string line;
            while (std::getline(in, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.empty()) continue;
                // Recognize doodad placement comment lines emitted by
                // --export-wob-obj so the round-trip preserves them.
                if (line[0] == '#') {
                    if (line.find("# doodad ") == 0) {
                        std::istringstream ss(line);
                        std::string hash, doodadKw, modelPath, posKw, posStr,
                                    rotKw, rotStr, scaleKw;
                        float scale = 1.0f;
                        ss >> hash >> doodadKw >> modelPath
                           >> posKw >> posStr
                           >> rotKw >> rotStr
                           >> scaleKw >> scale;
                        auto parse3 = [](const std::string& s, glm::vec3& out) {
                            int got = std::sscanf(s.c_str(), "%f,%f,%f",
                                                  &out.x, &out.y, &out.z);
                            return got == 3;
                        };
                        wowee::pipeline::WoweeBuilding::DoodadPlacement d;
                        d.modelPath = modelPath;
                        if (parse3(posStr, d.position) &&
                            parse3(rotStr, d.rotation) &&
                            std::isfinite(scale) && scale > 0.0f) {
                            d.scale = scale;
                            bld.doodads.push_back(d);
                        }
                    }
                    continue;
                }
                std::istringstream ss(line);
                std::string tag;
                ss >> tag;
                if (tag == "v") {
                    glm::vec3 p; ss >> p.x >> p.y >> p.z;
                    positions.push_back(p);
                } else if (tag == "vt") {
                    glm::vec2 t; ss >> t.x >> t.y;
                    texcoords.push_back(t);
                } else if (tag == "vn") {
                    glm::vec3 n; ss >> n.x >> n.y >> n.z;
                    normals.push_back(n);
                } else if (tag == "o") {
                    if (objectName.empty()) ss >> objectName;
                } else if (tag == "g") {
                    // New group — flush dedupe table so the next batch of
                    // verts is local to this group.
                    std::string name;
                    ss >> name;
                    activeGroup = name.empty() ? "group" : name;
                    activeGroupIdx = -1;
                    groupDedupe.clear();
                } else if (tag == "f") {
                    std::vector<std::string> corners;
                    std::string c;
                    while (ss >> c) corners.push_back(c);
                    if (corners.size() < 3) { badFaces++; continue; }
                    std::vector<int> resolved;
                    resolved.reserve(corners.size());
                    bool ok = true;
                    for (const auto& cc : corners) {
                        int idx = resolveCorner(cc);
                        if (idx < 0) { ok = false; break; }
                        resolved.push_back(idx);
                    }
                    if (!ok) { badFaces++; continue; }
                    if (resolved.size() > 3) triangulatedNgons++;
                    auto& grp = bld.groups[activeGroupIdx];
                    for (size_t k = 1; k + 1 < resolved.size(); ++k) {
                        grp.indices.push_back(static_cast<uint32_t>(resolved[0]));
                        grp.indices.push_back(static_cast<uint32_t>(resolved[k]));
                        grp.indices.push_back(static_cast<uint32_t>(resolved[k + 1]));
                    }
                }
                // mtllib/usemtl/s lines silently skipped.
            }
            // Compute per-group bounds + global building bound.
            if (bld.groups.empty()) {
                std::fprintf(stderr, "import-wob-obj: no geometry found in %s\n",
                             objPath.c_str());
                return 1;
            }
            glm::vec3 bMin{1e30f}, bMax{-1e30f};
            for (auto& grp : bld.groups) {
                if (grp.vertices.empty()) continue;
                grp.boundMin = grp.vertices[0].position;
                grp.boundMax = grp.boundMin;
                for (const auto& v : grp.vertices) {
                    grp.boundMin = glm::min(grp.boundMin, v.position);
                    grp.boundMax = glm::max(grp.boundMax, v.position);
                }
                bMin = glm::min(bMin, grp.boundMin);
                bMax = glm::max(bMax, grp.boundMax);
            }
            glm::vec3 center = (bMin + bMax) * 0.5f;
            float r2 = 0;
            for (const auto& grp : bld.groups) {
                for (const auto& v : grp.vertices) {
                    glm::vec3 d = v.position - center;
                    r2 = std::max(r2, glm::dot(d, d));
                }
            }
            bld.boundRadius = std::sqrt(r2);
            bld.name = objectName.empty()
                ? std::filesystem::path(objPath).stem().string()
                : objectName;
            if (!wowee::pipeline::WoweeBuildingLoader::save(bld, wobBase)) {
                std::fprintf(stderr, "import-wob-obj: failed to write %s.wob\n",
                             wobBase.c_str());
                return 1;
            }
            size_t totalV = 0, totalI = 0;
            for (const auto& g : bld.groups) {
                totalV += g.vertices.size();
                totalI += g.indices.size();
            }
            std::printf("Imported %s -> %s.wob\n", objPath.c_str(), wobBase.c_str());
            std::printf("  %zu groups, %zu verts, %zu tris, %zu doodad placements\n",
                        bld.groups.size(), totalV, totalI / 3, bld.doodads.size());
            if (triangulatedNgons > 0) {
                std::printf("  fan-triangulated %d n-gon(s)\n", triangulatedNgons);
            }
            if (badFaces > 0) {
                std::printf("  warning: skipped %d malformed face(s)\n", badFaces);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--export-woc-obj") == 0 && i + 1 < argc) {
            // Visualize a WOC collision mesh in any 3D tool. Each
            // walkability class becomes its own OBJ group (walkable /
            // steep / water / indoor) so designers can hide categories
            // independently in Blender to debug 'why can the player
            // walk here?' or 'why can't they walk there?'.
            std::string path = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "WOC not found: %s\n", path.c_str());
                return 1;
            }
            if (outPath.empty()) {
                outPath = path;
                if (outPath.size() >= 4 &&
                    outPath.substr(outPath.size() - 4) == ".woc") {
                    outPath = outPath.substr(0, outPath.size() - 4);
                }
                outPath += ".obj";
            }
            auto woc = wowee::pipeline::WoweeCollisionBuilder::load(path);
            if (!woc.isValid()) {
                std::fprintf(stderr, "WOC has no triangles: %s\n", path.c_str());
                return 1;
            }
            std::ofstream obj(outPath);
            if (!obj) {
                std::fprintf(stderr, "Failed to open output file: %s\n", outPath.c_str());
                return 1;
            }
            // Bucket triangles by flag combination so the OBJ can split
            // them into named groups. Flag bits: walkable=0x01, water=0x02,
            // steep=0x04, indoor=0x08 (per WoweeCollision::Triangle).
            // Triangles can have multiple flags set so a per-flag group
            // would over-count; instead we bucket by exact flag value.
            std::unordered_map<uint8_t, std::vector<size_t>> byFlag;
            for (size_t t = 0; t < woc.triangles.size(); ++t) {
                byFlag[woc.triangles[t].flags].push_back(t);
            }
            obj << "# Wavefront OBJ generated by wowee_editor --export-woc-obj\n";
            obj << "# Source: " << path << "\n";
            obj << "# Triangles: " << woc.triangles.size()
                << " (walkable=" << woc.walkableCount()
                << " steep=" << woc.steepCount() << ")\n";
            obj << "# Tile: (" << woc.tileX << ", " << woc.tileY << ")\n\n";
            obj << "o WoweeCollision\n";
            // Emit ALL vertices first (3 per triangle, no dedupe — the
            // collision mesh has triangle-soup topology where shared
            // verts often have different flags, so deduping would
            // actually merge categories).
            for (const auto& tri : woc.triangles) {
                obj << "v " << tri.v0.x << " " << tri.v0.y << " " << tri.v0.z << "\n";
                obj << "v " << tri.v1.x << " " << tri.v1.y << " " << tri.v1.z << "\n";
                obj << "v " << tri.v2.x << " " << tri.v2.y << " " << tri.v2.z << "\n";
            }
            // Emit faces grouped by flag class. OBJ index of triangle t
            // vertex k is (t * 3 + k + 1) — 1-based, three verts per tri.
            auto flagName = [](uint8_t f) {
                if (f == 0) return std::string("nonwalkable");
                std::string s;
                if (f & 0x01) s += "walkable";
                if (f & 0x02) { if (!s.empty()) s += "_"; s += "water"; }
                if (f & 0x04) { if (!s.empty()) s += "_"; s += "steep"; }
                if (f & 0x08) { if (!s.empty()) s += "_"; s += "indoor"; }
                if (s.empty()) s = "flag" + std::to_string(int(f));
                return s;
            };
            for (const auto& [flag, tris] : byFlag) {
                obj << "g " << flagName(flag) << "\n";
                for (size_t t : tris) {
                    uint32_t base = static_cast<uint32_t>(t * 3 + 1);
                    obj << "f " << base << " " << (base + 1) << " " << (base + 2) << "\n";
                }
            }
            obj.close();
            std::printf("Exported %s -> %s\n", path.c_str(), outPath.c_str());
            std::printf("  %zu triangles in %zu flag class(es), tile (%u, %u)\n",
                        woc.triangles.size(), byFlag.size(), woc.tileX, woc.tileY);
            return 0;
        } else if (std::strcmp(argv[i], "--export-whm-obj") == 0 && i + 1 < argc) {
            // Convert a WHM/WOT terrain pair to OBJ for visualization in
            // Blender / MeshLab. Emits the 9x9 outer vertex grid per
            // chunk (skipping the 8x8 inner verts the engine uses for
            // 4-tri fans) — that's the canonical 'heightmap as mesh'
            // view, 256 chunks × 81 verts = 20736 verts, 32768 tris.
            // Geometry mirrors WoweeCollisionBuilder's outer-grid layout
            // exactly so the OBJ aligns with the corresponding WOC.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
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
            if (outPath.empty()) outPath = base + ".obj";
            wowee::pipeline::ADTTerrain terrain;
            wowee::pipeline::WoweeTerrainLoader::load(base, terrain);
            std::ofstream obj(outPath);
            if (!obj) {
                std::fprintf(stderr, "Failed to open output file: %s\n", outPath.c_str());
                return 1;
            }
            // Tile + chunk constants — must match WoweeCollisionBuilder so
            // exports of the same source align in space when overlaid.
            constexpr float kTileSize = 533.33333f;
            constexpr float kChunkSize = kTileSize / 16.0f;
            constexpr float kVertSpacing = kChunkSize / 8.0f;
            obj << "# Wavefront OBJ generated by wowee_editor --export-whm-obj\n";
            obj << "# Source: " << base << ".whm\n";
            obj << "# Tile coord: (" << terrain.coord.x << ", " << terrain.coord.y << ")\n";
            obj << "# Layout: 9x9 outer vertex grid per chunk, 8x8 quads -> 2 tris each\n\n";
            obj << "o WoweeTerrain_" << terrain.coord.x << "_" << terrain.coord.y << "\n";
            int loadedChunks = 0;
            uint32_t vertOffset = 0;
            for (int cx = 0; cx < 16; ++cx) {
                for (int cy = 0; cy < 16; ++cy) {
                    const auto& chunk = terrain.getChunk(cx, cy);
                    if (!chunk.heightMap.isLoaded()) continue;
                    loadedChunks++;
                    // Same XY origin formula as collision builder so
                    // overlaid OBJ exports line up exactly.
                    float chunkBaseX = (32.0f - terrain.coord.y) * kTileSize - cy * kChunkSize;
                    float chunkBaseY = (32.0f - terrain.coord.x) * kTileSize - cx * kChunkSize;
                    // Emit 9x9 outer verts. Layout: heights[row*17 + col]
                    // for col in [0,8] (the inner 8 verts at col 9..16
                    // are skipped — they're the quad-center verts).
                    for (int row = 0; row < 9; ++row) {
                        for (int col = 0; col < 9; ++col) {
                            float x = chunkBaseX - row * kVertSpacing;
                            float y = chunkBaseY - col * kVertSpacing;
                            float z = chunk.position[2] +
                                      chunk.heightMap.heights[row * 17 + col];
                            obj << "v " << x << " " << y << " " << z << "\n";
                        }
                    }
                    // Per-vertex UV: just the row/col in 0..1 — Blender
                    // can use this to slap a checker texture for scale.
                    for (int row = 0; row < 9; ++row) {
                        for (int col = 0; col < 9; ++col) {
                            obj << "vt " << (col / 8.0f) << " "
                                          << (row / 8.0f) << "\n";
                        }
                    }
                    // 8x8 quads — two tris each, respecting hole bits so
                    // cave-entrance quads correctly disappear from the mesh.
                    bool isHoleChunk = (chunk.holes != 0);
                    obj << "g chunk_" << cx << "_" << cy << "\n";
                    auto idx = [&](int r, int c) {
                        return vertOffset + r * 9 + c + 1;  // 1-based
                    };
                    for (int row = 0; row < 8; ++row) {
                        for (int col = 0; col < 8; ++col) {
                            if (isHoleChunk) {
                                int hx = col / 2, hy = row / 2;
                                if (chunk.holes & (1 << (hy * 4 + hx))) continue;
                            }
                            uint32_t i00 = idx(row, col);
                            uint32_t i10 = idx(row, col + 1);
                            uint32_t i01 = idx(row + 1, col);
                            uint32_t i11 = idx(row + 1, col + 1);
                            obj << "f " << i00 << "/" << i00 << " "
                                << i10 << "/" << i10 << " "
                                << i11 << "/" << i11 << "\n";
                            obj << "f " << i00 << "/" << i00 << " "
                                << i11 << "/" << i11 << " "
                                << i01 << "/" << i01 << "\n";
                        }
                    }
                    vertOffset += 81;  // 9x9 verts per chunk
                }
            }
            obj.close();
            // Estimated tri count: chunks × 128 (8x8 quads × 2 tris).
            // Holes reduce this but counting exactly would mean walking
            // the bitmask again — the rough estimate is the user-visible
            // useful number anyway.
            std::printf("Exported %s.whm -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  %d chunks loaded, ~%d verts, ~%d tris\n",
                        loadedChunks, loadedChunks * 81, loadedChunks * 128);
            return 0;
        } else if (std::strcmp(argv[i], "--import-obj") == 0 && i + 1 < argc) {
            // Convert a Wavefront OBJ back into WOM. Round-trips with
            // --export-obj for the geometry/UV/normal data; bones,
            // animations, and material flags are not in OBJ and stay
            // empty (the resulting WOM is WOM1, static-only). The intent
            // is "edit a static prop in Blender, ship it".
            std::string objPath = argv[++i];
            std::string womBase;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                womBase = argv[++i];
            }
            if (!std::filesystem::exists(objPath)) {
                std::fprintf(stderr, "OBJ not found: %s\n", objPath.c_str());
                return 1;
            }
            if (womBase.empty()) {
                womBase = objPath;
                if (womBase.size() >= 4 &&
                    womBase.substr(womBase.size() - 4) == ".obj") {
                    womBase = womBase.substr(0, womBase.size() - 4);
                }
            }
            std::ifstream in(objPath);
            if (!in) {
                std::fprintf(stderr, "Failed to open OBJ: %s\n", objPath.c_str());
                return 1;
            }
            // Pools — OBJ stores positions/UVs/normals in independent
            // arrays and references them by index in face lines, so we
            // collect each pool first then expand into WOM vertices on
            // the fly (one WOM vertex per (vIdx, vtIdx, vnIdx) triple
            // since WOM has interleaved vertex data, not pooled).
            std::vector<glm::vec3> positions;
            std::vector<glm::vec2> texcoords;
            std::vector<glm::vec3> normals;
            wowee::pipeline::WoweeModel wom;
            wom.version = 1;
            std::unordered_map<std::string, uint32_t> dedupe;
            int badFaces = 0;
            int triangulatedNgons = 0;
            std::string objectName;
            std::string line;
            // Convert a single OBJ vertex token like "3/4/5" or "3//5" or
            // "3/4" or "3" into a WOM vertex index, deduping identical
            // (pos, uv, normal) triples to keep the buffer compact.
            auto resolveCorner = [&](const std::string& token) -> int {
                int v = 0, t = 0, n = 0;
                {
                    const char* p = token.c_str();
                    char* endp = nullptr;
                    v = std::strtol(p, &endp, 10);
                    if (*endp == '/') {
                        ++endp;
                        if (*endp != '/') {
                            t = std::strtol(endp, &endp, 10);
                        }
                        if (*endp == '/') {
                            ++endp;
                            n = std::strtol(endp, &endp, 10);
                        }
                    }
                }
                // Translate negative (relative) indices to absolute.
                auto absIdx = [](int idx, size_t poolSize) -> int {
                    if (idx < 0) return static_cast<int>(poolSize) + idx;
                    return idx - 1;  // OBJ is 1-based
                };
                int vi = absIdx(v, positions.size());
                int ti = (t == 0) ? -1 : absIdx(t, texcoords.size());
                int ni = (n == 0) ? -1 : absIdx(n, normals.size());
                if (vi < 0 || vi >= static_cast<int>(positions.size())) return -1;
                std::string key = std::to_string(vi) + "/" +
                                  std::to_string(ti) + "/" +
                                  std::to_string(ni);
                auto it = dedupe.find(key);
                if (it != dedupe.end()) return static_cast<int>(it->second);
                wowee::pipeline::WoweeModel::Vertex vert;
                vert.position = positions[vi];
                if (ti >= 0 && ti < static_cast<int>(texcoords.size())) {
                    vert.texCoord = texcoords[ti];
                    // Reverse the V-flip from --export-obj so a round-trip
                    // returns the original UVs unchanged.
                    vert.texCoord.y = 1.0f - vert.texCoord.y;
                } else {
                    vert.texCoord = {0, 0};
                }
                if (ni >= 0 && ni < static_cast<int>(normals.size())) {
                    vert.normal = normals[ni];
                } else {
                    vert.normal = {0, 0, 1};
                }
                uint32_t newIdx = static_cast<uint32_t>(wom.vertices.size());
                wom.vertices.push_back(vert);
                dedupe[key] = newIdx;
                return static_cast<int>(newIdx);
            };
            while (std::getline(in, line)) {
                // Strip CR for CRLF files.
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                std::istringstream ss(line);
                std::string tag;
                ss >> tag;
                if (tag == "v") {
                    glm::vec3 p; ss >> p.x >> p.y >> p.z;
                    positions.push_back(p);
                } else if (tag == "vt") {
                    glm::vec2 t; ss >> t.x >> t.y;
                    texcoords.push_back(t);
                } else if (tag == "vn") {
                    glm::vec3 n; ss >> n.x >> n.y >> n.z;
                    normals.push_back(n);
                } else if (tag == "o") {
                    if (objectName.empty()) ss >> objectName;
                } else if (tag == "f") {
                    std::vector<std::string> corners;
                    std::string c;
                    while (ss >> c) corners.push_back(c);
                    if (corners.size() < 3) { badFaces++; continue; }
                    std::vector<int> resolved;
                    resolved.reserve(corners.size());
                    bool ok = true;
                    for (const auto& cc : corners) {
                        int idx = resolveCorner(cc);
                        if (idx < 0) { ok = false; break; }
                        resolved.push_back(idx);
                    }
                    if (!ok) { badFaces++; continue; }
                    // Fan-triangulate (works for triangles, quads, and
                    // n-gons; assumes the polygon is convex which is the
                    // common case from DCC exporters).
                    if (resolved.size() > 3) triangulatedNgons++;
                    for (size_t k = 1; k + 1 < resolved.size(); ++k) {
                        wom.indices.push_back(static_cast<uint32_t>(resolved[0]));
                        wom.indices.push_back(static_cast<uint32_t>(resolved[k]));
                        wom.indices.push_back(static_cast<uint32_t>(resolved[k + 1]));
                    }
                }
                // mtllib/usemtl/g/s lines are silently skipped — material
                // info doesn't survive the round-trip but groups would
                // (left as future work; current import keeps it simple).
            }
            if (wom.vertices.empty() || wom.indices.empty()) {
                std::fprintf(stderr, "import-obj: no geometry found in %s\n",
                             objPath.c_str());
                return 1;
            }
            wom.name = objectName.empty()
                ? std::filesystem::path(objPath).stem().string()
                : objectName;
            // Compute bounds from positions — the renderer culls by these
            // so wrong values cause the model to disappear at distance.
            wom.boundMin = wom.vertices[0].position;
            wom.boundMax = wom.boundMin;
            for (const auto& v : wom.vertices) {
                wom.boundMin = glm::min(wom.boundMin, v.position);
                wom.boundMax = glm::max(wom.boundMax, v.position);
            }
            glm::vec3 center = (wom.boundMin + wom.boundMax) * 0.5f;
            float r2 = 0;
            for (const auto& v : wom.vertices) {
                glm::vec3 d = v.position - center;
                r2 = std::max(r2, glm::dot(d, d));
            }
            wom.boundRadius = std::sqrt(r2);
            if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
                std::fprintf(stderr, "import-obj: failed to write %s.wom\n",
                             womBase.c_str());
                return 1;
            }
            std::printf("Imported %s -> %s.wom\n", objPath.c_str(), womBase.c_str());
            std::printf("  %zu verts, %zu tris, bounds [%.2f, %.2f, %.2f] - [%.2f, %.2f, %.2f]\n",
                        wom.vertices.size(), wom.indices.size() / 3,
                        wom.boundMin.x, wom.boundMin.y, wom.boundMin.z,
                        wom.boundMax.x, wom.boundMax.y, wom.boundMax.z);
            if (triangulatedNgons > 0) {
                std::printf("  fan-triangulated %d n-gon(s)\n", triangulatedNgons);
            }
            if (badFaces > 0) {
                std::printf("  warning: skipped %d malformed face(s)\n", badFaces);
            }
            return 0;
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
        } else if (std::strcmp(argv[i], "--add-quest-objective") == 0 && i + 4 < argc) {
            // Append a single objective to an existing quest. The quest
            // must already exist (use --add-quest first); index is 0-based
            // and matches --list-quests output.
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string typeStr = argv[++i];
            std::string targetName = argv[++i];
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "add-quest-objective: %s not found — run --add-quest first\n",
                             path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "add-quest-objective: bad questIdx '%s'\n", idxStr.c_str());
                return 1;
            }
            using OT = wowee::editor::QuestObjectiveType;
            OT type;
            if      (typeStr == "kill")    type = OT::KillCreature;
            else if (typeStr == "collect") type = OT::CollectItem;
            else if (typeStr == "talk")    type = OT::TalkToNPC;
            else if (typeStr == "explore") type = OT::ExploreArea;
            else if (typeStr == "escort")  type = OT::EscortNPC;
            else if (typeStr == "use")     type = OT::UseObject;
            else {
                std::fprintf(stderr,
                    "add-quest-objective: type must be kill/collect/talk/explore/escort/use, got '%s'\n",
                    typeStr.c_str());
                return 1;
            }
            uint32_t count = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try {
                    count = static_cast<uint32_t>(std::stoul(argv[++i]));
                    if (count == 0) count = 1;
                } catch (...) {}
            }
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "add-quest-objective: failed to load %s\n", path.c_str());
                return 1;
            }
            if (idx < 0 || idx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr,
                    "add-quest-objective: questIdx %d out of range [0, %zu)\n",
                    idx, qe.questCount());
                return 1;
            }
            wowee::editor::QuestObjective obj;
            obj.type = type;
            obj.targetName = targetName;
            obj.targetCount = count;
            // Auto-generate a description from type+name+count so addons
            // and tooltips have something useful by default. The user can
            // edit quests.json directly if they want bespoke prose.
            const char* verb = "complete";
            switch (type) {
                case OT::KillCreature: verb = "Slay"; break;
                case OT::CollectItem:  verb = "Collect"; break;
                case OT::TalkToNPC:    verb = "Talk to"; break;
                case OT::ExploreArea:  verb = "Explore"; break;
                case OT::EscortNPC:    verb = "Escort"; break;
                case OT::UseObject:    verb = "Use"; break;
            }
            obj.description = std::string(verb) + " " +
                              (count > 1 ? std::to_string(count) + " " : "") +
                              targetName;
            // Quest is stored by value in the editor's vector; mutate via
            // the non-const getter, which gives us a pointer we can write
            // through.
            wowee::editor::Quest* q = qe.getQuest(idx);
            if (!q) {
                std::fprintf(stderr, "add-quest-objective: getQuest(%d) returned null\n", idx);
                return 1;
            }
            q->objectives.push_back(obj);
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "add-quest-objective: failed to write %s\n",
                             path.c_str());
                return 1;
            }
            std::printf("Added objective '%s' to quest %d ('%s'), now %zu objective(s)\n",
                        obj.description.c_str(), idx, q->title.c_str(),
                        q->objectives.size());
            return 0;
        } else if (std::strcmp(argv[i], "--remove-quest-objective") == 0 && i + 3 < argc) {
            // Symmetric counterpart to --add-quest-objective. Removes the
            // objective at <objIdx> within quest <questIdx>. Pair with
            // --info-quests / --list-quests to find the right indices.
            std::string zoneDir = argv[++i];
            std::string qIdxStr = argv[++i];
            std::string oIdxStr = argv[++i];
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "remove-quest-objective: %s not found\n", path.c_str());
                return 1;
            }
            int qIdx, oIdx;
            try {
                qIdx = std::stoi(qIdxStr);
                oIdx = std::stoi(oIdxStr);
            } catch (...) {
                std::fprintf(stderr, "remove-quest-objective: bad index\n");
                return 1;
            }
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "remove-quest-objective: failed to load %s\n",
                             path.c_str());
                return 1;
            }
            if (qIdx < 0 || qIdx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr,
                    "remove-quest-objective: questIdx %d out of range [0, %zu)\n",
                    qIdx, qe.questCount());
                return 1;
            }
            wowee::editor::Quest* q = qe.getQuest(qIdx);
            if (!q) return 1;
            if (oIdx < 0 || oIdx >= static_cast<int>(q->objectives.size())) {
                std::fprintf(stderr,
                    "remove-quest-objective: objIdx %d out of range [0, %zu)\n",
                    oIdx, q->objectives.size());
                return 1;
            }
            std::string removedDesc = q->objectives[oIdx].description;
            q->objectives.erase(q->objectives.begin() + oIdx);
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "remove-quest-objective: failed to write %s\n",
                             path.c_str());
                return 1;
            }
            std::printf("Removed objective '%s' (was index %d) from quest %d ('%s'), now %zu remaining\n",
                        removedDesc.c_str(), oIdx, qIdx, q->title.c_str(),
                        q->objectives.size());
            return 0;
        } else if (std::strcmp(argv[i], "--clone-quest") == 0 && i + 2 < argc) {
            // Duplicate a quest. Useful for templating: create a base
            // quest with objectives + rewards once, then clone N times
            // for variants ('Slay Wolves', 'Slay Bears' with the same
            // shape). Optional newTitle replaces the cloned copy's title;
            // omit to get '<original> (copy)'.
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string newTitle;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                newTitle = argv[++i];
            }
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "clone-quest: %s not found\n", path.c_str());
                return 1;
            }
            int qIdx;
            try { qIdx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "clone-quest: bad questIdx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "clone-quest: failed to load %s\n", path.c_str());
                return 1;
            }
            if (qIdx < 0 || qIdx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr,
                    "clone-quest: questIdx %d out of range [0, %zu)\n",
                    qIdx, qe.questCount());
                return 1;
            }
            // Deep-copy by value via vector iteration; .objectives and
            // .reward are STL containers so the copy is automatic.
            wowee::editor::Quest clone = qe.getQuests()[qIdx];
            // Reset id so the editor's auto-id sequence assigns a fresh
            // one — addQuest does this internally if id==0.
            clone.id = 0;
            // Reset chain link too — copying a chained quest with the
            // same nextQuestId would corrupt the chain semantics.
            clone.nextQuestId = 0;
            clone.title = newTitle.empty()
                ? (clone.title + " (copy)")
                : newTitle;
            qe.addQuest(clone);
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "clone-quest: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Cloned quest %d -> '%s' (now %zu total)\n",
                        qIdx, clone.title.c_str(), qe.questCount());
            std::printf("  carried %zu objective(s), %zu item reward(s), xp=%u\n",
                        clone.objectives.size(),
                        clone.reward.itemRewards.size(),
                        clone.reward.xp);
            return 0;
        } else if (std::strcmp(argv[i], "--clone-creature") == 0 && i + 2 < argc) {
            // Duplicate a creature spawn. Common workflow: design one
            // 'patrol guard' archetype, then clone it across spawn points
            // around a town. Preserves stats, faction, behavior, equipment;
            // resets id and offsets position by 5 yards by default so the
            // copy doesn't z-fight with the original.
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string newName;
            float dx = 5.0f, dy = 0.0f, dz = 0.0f;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                newName = argv[++i];
            }
            // Optional 3-axis offset after newName.
            if (i + 3 < argc && argv[i + 1][0] != '-') {
                try {
                    dx = std::stof(argv[++i]);
                    dy = std::stof(argv[++i]);
                    dz = std::stof(argv[++i]);
                } catch (...) {
                    std::fprintf(stderr, "clone-creature: bad offset coordinate\n");
                    return 1;
                }
            }
            std::string path = zoneDir + "/creatures.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "clone-creature: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "clone-creature: bad idx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::NpcSpawner sp;
            if (!sp.loadFromFile(path)) {
                std::fprintf(stderr, "clone-creature: failed to load %s\n", path.c_str());
                return 1;
            }
            if (idx < 0 || idx >= static_cast<int>(sp.spawnCount())) {
                std::fprintf(stderr,
                    "clone-creature: idx %d out of range [0, %zu)\n",
                    idx, sp.spawnCount());
                return 1;
            }
            // Deep-copy by value; CreatureSpawn is POD-ish (vectors for
            // patrol points copy automatically).
            wowee::editor::CreatureSpawn clone = sp.getSpawns()[idx];
            clone.id = 0;  // addCreature auto-assigns a fresh id
            clone.name = newName.empty()
                ? (clone.name + " (copy)")
                : newName;
            clone.position.x += dx;
            clone.position.y += dy;
            clone.position.z += dz;
            // Patrol path is intentionally NOT offset — patrol points are
            // typically authored as world-space waypoints, not relative to
            // the spawn. Designers re-author the path if needed.
            sp.getSpawns().push_back(clone);
            if (!sp.saveToFile(path)) {
                std::fprintf(stderr, "clone-creature: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Cloned creature %d -> '%s' at (%.1f, %.1f, %.1f) (now %zu total)\n",
                        idx, clone.name.c_str(),
                        clone.position.x, clone.position.y, clone.position.z,
                        sp.spawnCount());
            return 0;
        } else if (std::strcmp(argv[i], "--clone-object") == 0 && i + 2 < argc) {
            // Symmetric to --clone-creature/--clone-quest. Common
            // workflow: place one tree/lamp/barrel just right, then
            // clone N copies along a path or around a square. Default
            // 5-yard X offset prevents z-fighting; rotation/scale are
            // preserved so a tilted object stays tilted.
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            float dx = 5.0f, dy = 0.0f, dz = 0.0f;
            if (i + 3 < argc && argv[i + 1][0] != '-') {
                try {
                    dx = std::stof(argv[++i]);
                    dy = std::stof(argv[++i]);
                    dz = std::stof(argv[++i]);
                } catch (...) {
                    std::fprintf(stderr, "clone-object: bad offset\n");
                    return 1;
                }
            }
            std::string path = zoneDir + "/objects.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "clone-object: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "clone-object: bad idx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::ObjectPlacer placer;
            if (!placer.loadFromFile(path)) {
                std::fprintf(stderr, "clone-object: failed to load %s\n", path.c_str());
                return 1;
            }
            auto& objs = placer.getObjects();
            if (idx < 0 || idx >= static_cast<int>(objs.size())) {
                std::fprintf(stderr,
                    "clone-object: idx %d out of range [0, %zu)\n",
                    idx, objs.size());
                return 1;
            }
            // Deep-copy by value. uniqueId is reset so the new object
            // doesn't collide with the source's identifier in any
            // downstream system that dedups by it.
            wowee::editor::PlacedObject clone = objs[idx];
            clone.uniqueId = 0;
            clone.selected = false;
            clone.position.x += dx;
            clone.position.y += dy;
            clone.position.z += dz;
            objs.push_back(clone);
            if (!placer.saveToFile(path)) {
                std::fprintf(stderr, "clone-object: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Cloned object %d -> '%s' at (%.1f, %.1f, %.1f) (now %zu total)\n",
                        idx, clone.path.c_str(),
                        clone.position.x, clone.position.y, clone.position.z,
                        objs.size());
            return 0;
        } else if (std::strcmp(argv[i], "--add-quest-reward-item") == 0 && i + 3 < argc) {
            // Append one or more item rewards to a quest. Multiple paths
            // can be passed in a single invocation:
            //   --add-quest-reward-item zone 0 'Item:Sword' 'Item:Shield'
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "add-quest-reward-item: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "add-quest-reward-item: bad questIdx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "add-quest-reward-item: failed to load %s\n", path.c_str());
                return 1;
            }
            if (idx < 0 || idx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr,
                    "add-quest-reward-item: questIdx %d out of range [0, %zu)\n",
                    idx, qe.questCount());
                return 1;
            }
            wowee::editor::Quest* q = qe.getQuest(idx);
            if (!q) return 1;
            int added = 0;
            // Greedy-consume any remaining args that don't start with '-'
            // so the caller can batch-add a whole loot table in one shot.
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                q->reward.itemRewards.push_back(argv[++i]);
                added++;
            }
            if (added == 0) {
                std::fprintf(stderr, "add-quest-reward-item: need at least one itemPath\n");
                return 1;
            }
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "add-quest-reward-item: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Added %d item reward(s) to quest %d ('%s'), now %zu total\n",
                        added, idx, q->title.c_str(), q->reward.itemRewards.size());
            return 0;
        } else if (std::strcmp(argv[i], "--set-quest-reward") == 0 && i + 2 < argc) {
            // Update XP / coin reward fields on an existing quest. Each
            // field is optional — only the ones explicitly passed are
            // changed. This avoids the round-trip-and-clobber footgun of
            // a "replace whole reward" command.
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "set-quest-reward: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "set-quest-reward: bad questIdx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "set-quest-reward: failed to load %s\n", path.c_str());
                return 1;
            }
            if (idx < 0 || idx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr,
                    "set-quest-reward: questIdx %d out of range [0, %zu)\n",
                    idx, qe.questCount());
                return 1;
            }
            wowee::editor::Quest* q = qe.getQuest(idx);
            if (!q) return 1;
            int changed = 0;
            auto consumeUint = [&](const char* flag, uint32_t& target) {
                if (i + 2 < argc && std::strcmp(argv[i + 1], flag) == 0) {
                    try {
                        target = static_cast<uint32_t>(std::stoul(argv[i + 2]));
                        i += 2;
                        changed++;
                        return true;
                    } catch (...) {
                        std::fprintf(stderr, "set-quest-reward: bad %s value '%s'\n",
                                     flag, argv[i + 2]);
                    }
                }
                return false;
            };
            // Loop until no more recognised flags consume their value —
            // order-independent, so callers can pass --gold then --xp.
            bool any = true;
            while (any) {
                any = false;
                if (consumeUint("--xp",     q->reward.xp))     any = true;
                if (consumeUint("--gold",   q->reward.gold))   any = true;
                if (consumeUint("--silver", q->reward.silver)) any = true;
                if (consumeUint("--copper", q->reward.copper)) any = true;
            }
            if (changed == 0) {
                std::fprintf(stderr,
                    "set-quest-reward: no fields changed — pass --xp / --gold / --silver / --copper\n");
                return 1;
            }
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "set-quest-reward: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Updated %d field(s) on quest %d ('%s'): xp=%u gold=%u silver=%u copper=%u\n",
                        changed, idx, q->title.c_str(),
                        q->reward.xp, q->reward.gold,
                        q->reward.silver, q->reward.copper);
            return 0;
        } else if (std::strcmp(argv[i], "--remove-creature") == 0 && i + 2 < argc) {
            // Remove a creature spawn by 0-based index. Pair with
            // --info-creatures (or your editor) to find the right index
            // first; nothing identifies entries reliably across reloads.
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string path = zoneDir + "/creatures.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "remove-creature: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "remove-creature: bad index '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::NpcSpawner sp;
            sp.loadFromFile(path);
            if (idx < 0 || idx >= static_cast<int>(sp.spawnCount())) {
                std::fprintf(stderr, "remove-creature: index %d out of range [0, %zu)\n",
                             idx, sp.spawnCount());
                return 1;
            }
            std::string removedName = sp.getSpawns()[idx].name;
            sp.removeCreature(idx);
            if (!sp.saveToFile(path)) {
                std::fprintf(stderr, "remove-creature: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Removed creature '%s' (was index %d) from %s (now %zu total)\n",
                        removedName.c_str(), idx, path.c_str(), sp.spawnCount());
            return 0;
        } else if (std::strcmp(argv[i], "--remove-object") == 0 && i + 2 < argc) {
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string path = zoneDir + "/objects.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "remove-object: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "remove-object: bad index '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::ObjectPlacer placer;
            placer.loadFromFile(path);
            auto& objs = placer.getObjects();
            if (idx < 0 || idx >= static_cast<int>(objs.size())) {
                std::fprintf(stderr, "remove-object: index %d out of range [0, %zu)\n",
                             idx, objs.size());
                return 1;
            }
            std::string removedPath = objs[idx].path;
            objs.erase(objs.begin() + idx);
            if (!placer.saveToFile(path)) {
                std::fprintf(stderr, "remove-object: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Removed object '%s' (was index %d) from %s (now %zu total)\n",
                        removedPath.c_str(), idx, path.c_str(), objs.size());
            return 0;
        } else if (std::strcmp(argv[i], "--remove-quest") == 0 && i + 2 < argc) {
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "remove-quest: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "remove-quest: bad index '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::QuestEditor qe;
            qe.loadFromFile(path);
            if (idx < 0 || idx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr, "remove-quest: index %d out of range [0, %zu)\n",
                             idx, qe.questCount());
                return 1;
            }
            std::string removedTitle = qe.getQuests()[idx].title;
            qe.removeQuest(idx);
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "remove-quest: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Removed quest '%s' (was index %d) from %s (now %zu total)\n",
                        removedTitle.c_str(), idx, path.c_str(), qe.questCount());
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
        } else if (std::strcmp(argv[i], "--add-tile") == 0 && i + 3 < argc) {
            // Extend an existing zone with another ADT tile. Zones can
            // span multiple tiles (e.g. a continent fragment), but
            // --scaffold-zone only creates one. This adds another:
            //   wowee_editor --add-tile custom_zones/MyZone 29 30
            // Generates a fresh blank-flat WHM/WOT pair at the new tile
            // and appends to the zone manifest's tiles list.
            std::string zoneDir = argv[++i];
            int tx, ty;
            try {
                tx = std::stoi(argv[++i]);
                ty = std::stoi(argv[++i]);
            } catch (...) {
                std::fprintf(stderr, "add-tile: bad coordinates\n");
                return 1;
            }
            float baseHeight = 100.0f;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try { baseHeight = std::stof(argv[++i]); }
                catch (...) {}
            }
            if (tx < 0 || tx >= 64 || ty < 0 || ty >= 64) {
                std::fprintf(stderr, "add-tile: tile coord (%d, %d) out of WoW grid [0, 64)\n",
                             tx, ty);
                return 1;
            }
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr, "add-tile: %s has no zone.json — not a zone dir\n",
                             zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "add-tile: failed to parse %s\n", manifestPath.c_str());
                return 1;
            }
            // Reject duplicates so we don't silently overwrite an existing
            // tile's heightmap when the user makes a typo.
            for (const auto& [ex, ey] : zm.tiles) {
                if (ex == tx && ey == ty) {
                    std::fprintf(stderr,
                        "add-tile: tile (%d, %d) already in manifest\n", tx, ty);
                    return 1;
                }
            }
            // Also bail if the file would clobber an existing one outside
            // the manifest (e.g. user hand-created tiles without updating
            // zone.json). Catches drift between disk and manifest.
            std::string base = zoneDir + "/" + zm.mapName + "_" +
                               std::to_string(tx) + "_" + std::to_string(ty);
            if (fs::exists(base + ".whm") || fs::exists(base + ".wot")) {
                std::fprintf(stderr,
                    "add-tile: %s.{whm,wot} already exists on disk (manifest out of sync?)\n",
                    base.c_str());
                return 1;
            }
            // Generate the new heightmap. Reuses the same factory that
            // --scaffold-zone uses, so the output is consistent.
            auto terrain = wowee::editor::TerrainEditor::createBlankTerrain(
                tx, ty, baseHeight, wowee::editor::Biome::Grassland);
            wowee::editor::WoweeTerrain::exportOpen(terrain, base, tx, ty);
            // Append + save manifest. ZoneManifest::save rebuilds the
            // files block from the tiles list, so the new adt_tx_ty entry
            // appears automatically in zone.json.
            zm.tiles.push_back({tx, ty});
            if (!zm.save(manifestPath)) {
                std::fprintf(stderr, "add-tile: failed to save %s\n", manifestPath.c_str());
                return 1;
            }
            std::printf("Added tile (%d, %d) to %s\n", tx, ty, zoneDir.c_str());
            std::printf("  files     : %s.whm, %s.wot\n",
                        (zm.mapName + "_" + std::to_string(tx) + "_" + std::to_string(ty)).c_str(),
                        (zm.mapName + "_" + std::to_string(tx) + "_" + std::to_string(ty)).c_str());
            std::printf("  tiles now : %zu total\n", zm.tiles.size());
            return 0;
        } else if (std::strcmp(argv[i], "--remove-tile") == 0 && i + 3 < argc) {
            // Symmetric counterpart to --add-tile. Drops the entry from
            // ZoneManifest::tiles AND deletes the WHM/WOT/WOC files on
            // disk so the zone is left consistent (no orphan sidecars).
            std::string zoneDir = argv[++i];
            int tx, ty;
            try {
                tx = std::stoi(argv[++i]);
                ty = std::stoi(argv[++i]);
            } catch (...) {
                std::fprintf(stderr, "remove-tile: bad coordinates\n");
                return 1;
            }
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr, "remove-tile: %s has no zone.json — not a zone dir\n",
                             zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "remove-tile: failed to parse %s\n", manifestPath.c_str());
                return 1;
            }
            auto it = std::find_if(zm.tiles.begin(), zm.tiles.end(),
                [&](const std::pair<int,int>& p) { return p.first == tx && p.second == ty; });
            if (it == zm.tiles.end()) {
                std::fprintf(stderr,
                    "remove-tile: tile (%d, %d) not in manifest\n", tx, ty);
                return 1;
            }
            // Don't strand a zone with zero tiles — server module gen and
            // pack-wcp both expect at least one. The user can --rename-zone
            // or rm -rf if they want the zone gone entirely.
            if (zm.tiles.size() == 1) {
                std::fprintf(stderr,
                    "remove-tile: refusing to remove last tile (zone would be empty)\n");
                return 1;
            }
            zm.tiles.erase(it);
            // Delete the slug-prefixed files for this tile. Use error_code
            // so we don't throw on missing files — partial removal from
            // earlier failures shouldn't block cleanup of what's left.
            std::string base = zoneDir + "/" + zm.mapName + "_" +
                               std::to_string(tx) + "_" + std::to_string(ty);
            int deleted = 0;
            std::error_code ec;
            for (const char* ext : {".whm", ".wot", ".woc"}) {
                if (fs::remove(base + ext, ec)) deleted++;
            }
            if (!zm.save(manifestPath)) {
                std::fprintf(stderr, "remove-tile: failed to save %s\n", manifestPath.c_str());
                return 1;
            }
            std::printf("Removed tile (%d, %d) from %s\n", tx, ty, zoneDir.c_str());
            std::printf("  deleted   : %d file(s) (.whm/.wot/.woc)\n", deleted);
            std::printf("  tiles now : %zu remaining\n", zm.tiles.size());
            return 0;
        } else if (std::strcmp(argv[i], "--list-tiles") == 0 && i + 1 < argc) {
            // Enumerate every tile in the zone manifest with on-disk
            // file presence — useful for spotting missing/orphan files
            // before pack-wcp would fail.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr, "list-tiles: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "list-tiles: failed to parse %s\n", manifestPath.c_str());
                return 1;
            }
            auto baseFor = [&](int tx, int ty) {
                return zoneDir + "/" + zm.mapName + "_" +
                       std::to_string(tx) + "_" + std::to_string(ty);
            };
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["mapName"] = zm.mapName;
                j["count"] = zm.tiles.size();
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& [tx, ty] : zm.tiles) {
                    std::string b = baseFor(tx, ty);
                    arr.push_back({
                        {"x", tx}, {"y", ty},
                        {"whm", fs::exists(b + ".whm")},
                        {"wot", fs::exists(b + ".wot")},
                        {"woc", fs::exists(b + ".woc")},
                    });
                }
                j["tiles"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone: %s (%s, %zu tile(s))\n",
                        zoneDir.c_str(), zm.mapName.c_str(), zm.tiles.size());
            std::printf("   tx   ty   whm  wot  woc\n");
            for (const auto& [tx, ty] : zm.tiles) {
                std::string b = baseFor(tx, ty);
                std::printf("  %3d  %3d   %s    %s    %s\n",
                            tx, ty,
                            fs::exists(b + ".whm") ? "y" : "-",
                            fs::exists(b + ".wot") ? "y" : "-",
                            fs::exists(b + ".woc") ? "y" : "-");
            }
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
        } else if (std::strcmp(argv[i], "--rename-zone") == 0 && i + 2 < argc) {
            // In-place rename — like --copy-zone but no copy. Useful when
            // the user wants to fix a typo or change a name without
            // doubling disk usage. Renames the directory itself too
            // (Old/ -> New/ under the same parent), so paths shift.
            std::string srcDir = argv[++i];
            std::string rawName = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(srcDir) || !fs::is_directory(srcDir)) {
                std::fprintf(stderr, "rename-zone: source dir not found: %s\n",
                             srcDir.c_str());
                return 1;
            }
            if (!fs::exists(srcDir + "/zone.json")) {
                std::fprintf(stderr, "rename-zone: %s has no zone.json — not a zone dir\n",
                             srcDir.c_str());
                return 1;
            }
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
                std::fprintf(stderr, "rename-zone: name '%s' has no valid characters\n",
                             rawName.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(srcDir + "/zone.json")) {
                std::fprintf(stderr, "rename-zone: failed to parse %s/zone.json\n",
                             srcDir.c_str());
                return 1;
            }
            std::string oldSlug = zm.mapName;
            if (oldSlug == newSlug && rawName == zm.displayName) {
                std::fprintf(stderr,
                    "rename-zone: nothing to do (slug=%s, displayName=%s already match)\n",
                    oldSlug.c_str(), rawName.c_str());
                return 1;
            }
            // Compute target directory: same parent, new slug name. If the
            // current directory name already matches the new slug, skip
            // the dir rename (only manifest + slug-prefixed files change).
            fs::path srcPath = fs::absolute(srcDir);
            fs::path parent = srcPath.parent_path();
            fs::path dstPath = parent / newSlug;
            bool needDirRename = (srcPath.filename() != newSlug);
            if (needDirRename && fs::exists(dstPath)) {
                std::fprintf(stderr, "rename-zone: target dir already exists: %s\n",
                             dstPath.string().c_str());
                return 1;
            }
            // Rename slug-prefixed files inside the source dir BEFORE
            // moving the directory — fewer paths to fix up if anything
            // fails midway. fs::rename is atomic per-call.
            std::error_code ec;
            int renamed = 0;
            for (const auto& entry : fs::recursive_directory_iterator(srcDir)) {
                if (!entry.is_regular_file()) continue;
                std::string fname = entry.path().filename().string();
                bool match = (oldSlug != newSlug &&
                              fname.size() > oldSlug.size() + 1 &&
                              fname.compare(0, oldSlug.size(), oldSlug) == 0 &&
                              (fname[oldSlug.size()] == '_' ||
                               fname[oldSlug.size()] == '.'));
                if (!match) continue;
                std::string newName = newSlug + fname.substr(oldSlug.size());
                fs::rename(entry.path(), entry.path().parent_path() / newName, ec);
                if (!ec) renamed++;
            }
            // Update manifest and save BEFORE the dir rename so the file
            // exists at the path we're saving to.
            zm.mapName = newSlug;
            zm.displayName = rawName;
            if (!zm.save(srcDir + "/zone.json")) {
                std::fprintf(stderr, "rename-zone: failed to write zone.json\n");
                return 1;
            }
            // Now move the directory itself.
            std::string finalDir = srcDir;
            if (needDirRename) {
                fs::rename(srcPath, dstPath, ec);
                if (ec) {
                    std::fprintf(stderr,
                        "rename-zone: dir rename failed (%s); manifest already updated\n",
                        ec.message().c_str());
                    return 1;
                }
                finalDir = dstPath.string();
            }
            std::printf("Renamed %s -> %s\n", srcDir.c_str(), finalDir.c_str());
            std::printf("  mapName  : %s -> %s\n", oldSlug.c_str(), newSlug.c_str());
            std::printf("  renamed  : %d slug-prefixed file(s)\n", renamed);
            return 0;
        } else if (std::strcmp(argv[i], "--clear-zone-content") == 0 && i + 1 < argc) {
            // Wipe content files (creatures.json / objects.json /
            // quests.json) from a zone while keeping terrain + manifest
            // intact. Useful for templating: --copy-zone gives you a
            // duplicate; --clear-zone-content turns it into an empty
            // shell ready for fresh population.
            //
            // Pass --creatures / --objects / --quests to wipe individually,
            // or --all to wipe everything. At least one selector is required.
            std::string zoneDir = argv[++i];
            bool wipeCreatures = false, wipeObjects = false, wipeQuests = false;
            while (i + 1 < argc && argv[i + 1][0] == '-') {
                std::string opt = argv[i + 1];
                if      (opt == "--creatures") { wipeCreatures = true; ++i; }
                else if (opt == "--objects")   { wipeObjects = true;   ++i; }
                else if (opt == "--quests")    { wipeQuests = true;    ++i; }
                else if (opt == "--all") {
                    wipeCreatures = wipeObjects = wipeQuests = true; ++i;
                }
                else break;  // unknown flag — stop consuming, surface the error
            }
            if (!wipeCreatures && !wipeObjects && !wipeQuests) {
                std::fprintf(stderr,
                    "clear-zone-content: pass --creatures / --objects / --quests / --all\n");
                return 1;
            }
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "clear-zone-content: %s has no zone.json — not a zone dir\n",
                    zoneDir.c_str());
                return 1;
            }
            // Delete (not blank-write) so the next --info-* doesn't see
            // an empty file and report 'total: 0' as if data existed.
            // Missing files are the canonical 'no content' state.
            int deleted = 0;
            std::error_code ec;
            auto wipe = [&](const std::string& fname) {
                std::string p = zoneDir + "/" + fname;
                if (fs::exists(p) && fs::remove(p, ec)) {
                    ++deleted;
                    std::printf("  removed  : %s\n", fname.c_str());
                } else if (fs::exists(p)) {
                    std::fprintf(stderr,
                        "  WARN: failed to remove %s (%s)\n",
                        p.c_str(), ec.message().c_str());
                } else {
                    std::printf("  skipped  : %s (already absent)\n", fname.c_str());
                }
            };
            std::printf("Cleared content from %s\n", zoneDir.c_str());
            if (wipeCreatures) wipe("creatures.json");
            if (wipeObjects)   wipe("objects.json");
            if (wipeQuests)    wipe("quests.json");
            // Also reset manifest.hasCreatures so server module gen
            // doesn't expect an NPC table that's no longer there.
            if (wipeCreatures) {
                wowee::editor::ZoneManifest zm;
                if (zm.load(zoneDir + "/zone.json")) {
                    if (zm.hasCreatures) {
                        zm.hasCreatures = false;
                        zm.save(zoneDir + "/zone.json");
                        std::printf("  updated  : zone.json hasCreatures = false\n");
                    }
                }
            }
            std::printf("  removed  : %d file(s) total\n", deleted);
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
        } else if (std::strcmp(argv[i], "--zone-stats") == 0 && i + 1 < argc) {
            // Multi-zone aggregator. Walks <projectDir> for every dir
            // with a zone.json and emits totals across the project:
            // tile counts, creature/object/quest counts, on-disk byte
            // sizes per format. Useful for content-pack release notes
            // and capacity planning.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "zone-stats: %s is not a directory\n", projectDir.c_str());
                return 1;
            }
            // Collect zone dirs.
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (fs::exists(entry.path() / "zone.json")) {
                    zones.push_back(entry.path().string());
                }
            }
            std::sort(zones.begin(), zones.end());
            // Aggregate.
            struct Totals {
                int zoneCount = 0;
                int tileCount = 0;
                int creatures = 0, objects = 0, quests = 0;
                int hostileCreatures = 0;
                int chainedQuests = 0;
                uint64_t totalXp = 0;
                uint64_t whmBytes = 0, wotBytes = 0, wocBytes = 0;
                uint64_t womBytes = 0, wobBytes = 0;
                uint64_t pngBytes = 0, jsonBytes = 0;
                uint64_t otherBytes = 0;
            } T;
            T.zoneCount = static_cast<int>(zones.size());
            // Per-zone breakdown for the table view (kept short — not
            // every field, just the high-signal ones).
            struct ZoneRow {
                std::string name;
                int tiles = 0, creatures = 0, objects = 0, quests = 0;
                uint64_t bytes = 0;
            };
            std::vector<ZoneRow> rows;
            for (const auto& zoneDir : zones) {
                wowee::editor::ZoneManifest zm;
                if (!zm.load(zoneDir + "/zone.json")) continue;
                wowee::editor::NpcSpawner sp;
                sp.loadFromFile(zoneDir + "/creatures.json");
                wowee::editor::ObjectPlacer op;
                op.loadFromFile(zoneDir + "/objects.json");
                wowee::editor::QuestEditor qe;
                qe.loadFromFile(zoneDir + "/quests.json");
                ZoneRow row;
                row.name = zm.mapName.empty()
                    ? fs::path(zoneDir).filename().string()
                    : zm.mapName;
                row.tiles = static_cast<int>(zm.tiles.size());
                row.creatures = static_cast<int>(sp.spawnCount());
                row.objects = static_cast<int>(op.getObjects().size());
                row.quests = static_cast<int>(qe.questCount());
                T.tileCount += row.tiles;
                T.creatures += row.creatures;
                T.objects += row.objects;
                T.quests += row.quests;
                for (const auto& s : sp.getSpawns()) {
                    if (s.hostile) T.hostileCreatures++;
                }
                for (const auto& q : qe.getQuests()) {
                    if (q.nextQuestId != 0) T.chainedQuests++;
                    T.totalXp += q.reward.xp;
                }
                // Walk on-disk files in the zone (recursive — sub-dirs
                // like data/ may hold sidecars). Bucket by extension.
                std::error_code ec;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    uint64_t sz = e.file_size(ec);
                    if (ec) continue;
                    row.bytes += sz;
                    std::string ext = e.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if      (ext == ".whm")  T.whmBytes  += sz;
                    else if (ext == ".wot")  T.wotBytes  += sz;
                    else if (ext == ".woc")  T.wocBytes  += sz;
                    else if (ext == ".wom")  T.womBytes  += sz;
                    else if (ext == ".wob")  T.wobBytes  += sz;
                    else if (ext == ".png")  T.pngBytes  += sz;
                    else if (ext == ".json") T.jsonBytes += sz;
                    else                     T.otherBytes += sz;
                }
                rows.push_back(row);
            }
            uint64_t totalBytes = T.whmBytes + T.wotBytes + T.wocBytes +
                                   T.womBytes + T.wobBytes + T.pngBytes +
                                   T.jsonBytes + T.otherBytes;
            if (jsonOut) {
                nlohmann::json j;
                j["projectDir"] = projectDir;
                j["zoneCount"] = T.zoneCount;
                j["tileCount"] = T.tileCount;
                j["creatures"] = T.creatures;
                j["hostileCreatures"] = T.hostileCreatures;
                j["objects"] = T.objects;
                j["quests"] = T.quests;
                j["chainedQuests"] = T.chainedQuests;
                j["totalXp"] = T.totalXp;
                j["bytes"] = {
                    {"whm", T.whmBytes},  {"wot", T.wotBytes},
                    {"woc", T.wocBytes},  {"wom", T.womBytes},
                    {"wob", T.wobBytes},  {"png", T.pngBytes},
                    {"json", T.jsonBytes}, {"other", T.otherBytes},
                    {"total", totalBytes}
                };
                nlohmann::json zarr = nlohmann::json::array();
                for (const auto& r : rows) {
                    zarr.push_back({
                        {"name", r.name}, {"tiles", r.tiles},
                        {"creatures", r.creatures}, {"objects", r.objects},
                        {"quests", r.quests}, {"bytes", r.bytes}
                    });
                }
                j["zones"] = zarr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone stats: %s\n", projectDir.c_str());
            std::printf("  zones      : %d\n", T.zoneCount);
            std::printf("  tiles      : %d total\n", T.tileCount);
            std::printf("  creatures  : %d (%d hostile)\n",
                        T.creatures, T.hostileCreatures);
            std::printf("  objects    : %d\n", T.objects);
            std::printf("  quests     : %d (%d chained, %llu total XP)\n",
                        T.quests, T.chainedQuests,
                        static_cast<unsigned long long>(T.totalXp));
            constexpr double kKB = 1024.0;
            std::printf("  bytes      : %.1f KB total\n", totalBytes / kKB);
            std::printf("    whm/wot  : %.1f KB / %.1f KB\n",
                        T.whmBytes / kKB, T.wotBytes / kKB);
            std::printf("    woc      : %.1f KB\n", T.wocBytes / kKB);
            std::printf("    wom/wob  : %.1f KB / %.1f KB\n",
                        T.womBytes / kKB, T.wobBytes / kKB);
            std::printf("    png/json : %.1f KB / %.1f KB\n",
                        T.pngBytes / kKB, T.jsonBytes / kKB);
            if (T.otherBytes > 0) {
                std::printf("    other    : %.1f KB\n", T.otherBytes / kKB);
            }
            std::printf("\n  per-zone breakdown:\n");
            std::printf("    name                tiles  creat  obj  quest    bytes\n");
            for (const auto& r : rows) {
                std::printf("    %-18s  %5d  %5d  %3d  %5d  %7.1f KB\n",
                            r.name.substr(0, 18).c_str(),
                            r.tiles, r.creatures, r.objects, r.quests,
                            r.bytes / kKB);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--list-zone-deps") == 0 && i + 1 < argc) {
            // Enumerate every external model path a zone references —
            // both directly placed (objects.json) and indirectly via
            // doodad placements inside any WOB sitting next to the
            // zone manifest. Useful when packaging a content pack to
            // confirm every needed asset will ship.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "list-zone-deps: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            // Collect with usage counts so duplicates report '×N' instead
            // of cluttering the table.
            std::map<std::string, int> directM2;   // m2 placements
            std::map<std::string, int> directWMO;  // wmo placements
            std::map<std::string, int> doodadM2;   // m2s referenced inside WOBs
            wowee::editor::ObjectPlacer op;
            if (op.loadFromFile(zoneDir + "/objects.json")) {
                for (const auto& o : op.getObjects()) {
                    if (o.type == wowee::editor::PlaceableType::M2) directM2[o.path]++;
                    else if (o.type == wowee::editor::PlaceableType::WMO) directWMO[o.path]++;
                }
            }
            // Walk WOBs in the zone directory recursively and pull in
            // their doodad model paths. Sub-dirs caught too in case the
            // user organizes buildings under a buildings/ subfolder.
            int wobCount = 0;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file()) continue;
                std::string ext = e.path().extension().string();
                if (ext != ".wob") continue;
                wobCount++;
                std::string base = e.path().string();
                if (base.size() >= 4) base = base.substr(0, base.size() - 4);
                auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
                for (const auto& d : bld.doodads) {
                    if (d.modelPath.empty()) continue;
                    doodadM2[d.modelPath]++;
                }
            }
            // For each direct WMO placement, also recurse into the WOB
            // sitting at that path (relative to the zone) so transitive
            // doodad deps surface — this matches the runtime's actual
            // load chain.
            for (const auto& [path, count] : directWMO) {
                // Strip extension since loader takes a base path.
                std::string base = path;
                if (base.size() >= 4 && base.substr(base.size() - 4) == ".wmo")
                    base = base.substr(0, base.size() - 4);
                // Try relative-to-zone first, then absolute.
                std::string trial = zoneDir + "/" + base;
                if (!wowee::pipeline::WoweeBuildingLoader::exists(trial)) trial = base;
                if (!wowee::pipeline::WoweeBuildingLoader::exists(trial)) continue;
                auto bld = wowee::pipeline::WoweeBuildingLoader::load(trial);
                for (const auto& d : bld.doodads) {
                    if (d.modelPath.empty()) continue;
                    doodadM2[d.modelPath]++;
                }
            }
            size_t totalUnique = directM2.size() + directWMO.size() + doodadM2.size();
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["wobCount"] = wobCount;
                j["totalUnique"] = totalUnique;
                auto toArr = [](const std::map<std::string, int>& m) {
                    nlohmann::json a = nlohmann::json::array();
                    for (const auto& [path, count] : m) {
                        a.push_back({{"path", path}, {"count", count}});
                    }
                    return a;
                };
                j["directM2"] = toArr(directM2);
                j["directWMO"] = toArr(directWMO);
                j["doodadM2"] = toArr(doodadM2);
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone deps: %s\n", zoneDir.c_str());
            std::printf("  WOBs scanned        : %d\n", wobCount);
            std::printf("  unique paths total  : %zu\n", totalUnique);
            auto emit = [](const char* tag, const std::map<std::string, int>& m) {
                std::printf("\n  %s (%zu unique):\n", tag, m.size());
                if (m.empty()) {
                    std::printf("    *none*\n");
                    return;
                }
                for (const auto& [path, count] : m) {
                    if (count > 1) std::printf("    %s ×%d\n", path.c_str(), count);
                    else            std::printf("    %s\n",     path.c_str());
                }
            };
            emit("Direct M2 placements",  directM2);
            emit("Direct WMO placements", directWMO);
            emit("WOB doodad M2 refs",    doodadM2);
            return 0;
        } else if (std::strcmp(argv[i], "--export-zone-deps-md") == 0 && i + 1 < argc) {
            // Markdown counterpart to --list-zone-deps. Writes a sortable
            // GitHub-rendered table of every external model the zone
            // references plus on-disk presence (so PR reviewers see at a
            // glance whether dependencies are accounted for in the
            // accompanying asset bundle).
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "export-zone-deps-md: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            zm.load(zoneDir + "/zone.json");
            if (outPath.empty()) outPath = zoneDir + "/DEPS.md";
            // Same dep-collection pass as --list-zone-deps.
            std::map<std::string, int> directM2;
            std::map<std::string, int> directWMO;
            std::map<std::string, int> doodadM2;
            wowee::editor::ObjectPlacer op;
            if (op.loadFromFile(zoneDir + "/objects.json")) {
                for (const auto& o : op.getObjects()) {
                    if (o.type == wowee::editor::PlaceableType::M2)  directM2[o.path]++;
                    else if (o.type == wowee::editor::PlaceableType::WMO) directWMO[o.path]++;
                }
            }
            int wobCount = 0;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file() ||
                    e.path().extension() != ".wob") continue;
                wobCount++;
                std::string base = e.path().string();
                if (base.size() >= 4) base = base.substr(0, base.size() - 4);
                auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
                for (const auto& d : bld.doodads) {
                    if (!d.modelPath.empty()) doodadM2[d.modelPath]++;
                }
            }
            // Resolve dep on disk. Same heuristic as --check-zone-refs:
            // try both open + proprietary in conventional roots.
            auto stripExt = [](const std::string& p, const char* ext) {
                size_t n = std::strlen(ext);
                if (p.size() >= n) {
                    std::string tail = p.substr(p.size() - n);
                    std::string lower = tail;
                    for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
                    if (lower == ext) return p.substr(0, p.size() - n);
                }
                return p;
            };
            auto resolveStatus = [&](const std::string& path, bool isWMO) {
                std::string base, openExt, propExt;
                if (isWMO) {
                    base = stripExt(path, ".wmo");
                    openExt = ".wob"; propExt = ".wmo";
                } else {
                    base = stripExt(path, ".m2");
                    openExt = ".wom"; propExt = ".m2";
                }
                std::vector<std::string> roots = {
                    "", zoneDir + "/", "output/", "custom_zones/", "Data/"
                };
                bool hasOpen = false, hasProp = false;
                for (const auto& root : roots) {
                    if (fs::exists(root + base + openExt)) hasOpen = true;
                    if (fs::exists(root + base + propExt)) hasProp = true;
                }
                if (hasOpen && hasProp) return "open + proprietary";
                if (hasOpen) return "open only";
                if (hasProp) return "proprietary only";
                return "MISSING";
            };
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "export-zone-deps-md: cannot write %s\n", outPath.c_str());
                return 1;
            }
            out << "# Dependencies — " <<
                (zm.displayName.empty() ? zm.mapName : zm.displayName) << "\n\n";
            out << "*Auto-generated by `wowee_editor --export-zone-deps-md`. "
                   "Status is best-effort — checks zone-local, output/, "
                   "custom_zones/, Data/ roots in that order.*\n\n";
            auto emitTable = [&](const char* heading,
                                  const std::map<std::string,int>& m,
                                  bool isWMO) {
                out << "## " << heading << " (" << m.size() << ")\n\n";
                if (m.empty()) {
                    out << "*None.*\n\n";
                    return;
                }
                out << "| Refs | Path | Status |\n";
                out << "|---:|---|---|\n";
                for (const auto& [path, count] : m) {
                    out << "| " << count << " | `" << path << "` | "
                        << resolveStatus(path, isWMO) << " |\n";
                }
                out << "\n";
            };
            emitTable("Direct M2 placements",  directM2,  false);
            emitTable("Direct WMO placements", directWMO, true);
            emitTable("WOB doodad M2 refs",    doodadM2,  false);
            out << "## Summary\n\n";
            out << "- Zone: `" << zm.mapName << "`\n";
            out << "- WOBs scanned: " << wobCount << "\n";
            out << "- Unique dependencies: " <<
                directM2.size() + directWMO.size() + doodadM2.size() << "\n";
            out.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  %zu M2 placements, %zu WMO placements, %zu WOB doodad refs\n",
                        directM2.size(), directWMO.size(), doodadM2.size());
            return 0;
        } else if (std::strcmp(argv[i], "--check-zone-refs") == 0 && i + 1 < argc) {
            // Cross-reference checker: every model path in objects.json
            // must resolve as either an open WOM/WOB sidecar or a
            // proprietary M2/WMO; every quest's giver/turnIn NPC ID must
            // appear in creatures.json (when the zone has creatures).
            // Catches dangling references that --validate doesn't, since
            // --validate only checks open-format file presence.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "check-zone-refs: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            // Try to find a model on disk in any of the conventional
            // locations (zone-local, output/, custom_zones/, Data/).
            // Strips extension and tries each open + proprietary variant.
            auto stripExt = [](const std::string& p, const char* ext) {
                size_t n = std::strlen(ext);
                if (p.size() >= n) {
                    std::string tail = p.substr(p.size() - n);
                    std::string lower = tail;
                    for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
                    if (lower == ext) return p.substr(0, p.size() - n);
                }
                return p;
            };
            auto modelExists = [&](const std::string& path, bool isWMO) {
                std::string base;
                std::vector<std::string> exts;
                if (isWMO) {
                    base = stripExt(path, ".wmo");
                    exts = {".wob", ".wmo"};
                } else {
                    base = stripExt(path, ".m2");
                    exts = {".wom", ".m2"};
                }
                std::vector<std::string> roots = {
                    "", zoneDir + "/", "output/", "custom_zones/", "Data/"
                };
                for (const auto& root : roots) {
                    for (const auto& ext : exts) {
                        if (fs::exists(root + base + ext)) return true;
                        // Case-fold fallback for case-sensitive filesystems
                        // (designers usually type Mixed Case but Linux
                        // stores asset paths lowercase after extraction).
                        std::string lower = base + ext;
                        for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
                        if (fs::exists(root + lower)) return true;
                    }
                }
                return false;
            };
            std::vector<std::string> errors;
            // Object placements -> models on disk
            wowee::editor::ObjectPlacer op;
            int objectsChecked = 0, objectsMissing = 0;
            if (op.loadFromFile(zoneDir + "/objects.json")) {
                for (size_t k = 0; k < op.getObjects().size(); ++k) {
                    const auto& o = op.getObjects()[k];
                    objectsChecked++;
                    bool isWMO = (o.type == wowee::editor::PlaceableType::WMO);
                    if (!modelExists(o.path, isWMO)) {
                        objectsMissing++;
                        if (errors.size() < 30) {
                            errors.push_back("object[" + std::to_string(k) +
                                             "] missing: " + o.path);
                        }
                    }
                }
            }
            // Quest NPCs -> creatures.json IDs (only when creatures exist;
            // otherwise NPC IDs may legitimately reference upstream content
            // outside the zone).
            wowee::editor::NpcSpawner sp;
            wowee::editor::QuestEditor qe;
            int questsChecked = 0, questsMissing = 0;
            bool hasCreatures = sp.loadFromFile(zoneDir + "/creatures.json");
            std::unordered_set<uint32_t> creatureIds;
            if (hasCreatures) {
                for (const auto& s : sp.getSpawns()) creatureIds.insert(s.id);
            }
            if (qe.loadFromFile(zoneDir + "/quests.json") && hasCreatures) {
                for (size_t k = 0; k < qe.getQuests().size(); ++k) {
                    const auto& q = qe.getQuests()[k];
                    questsChecked++;
                    bool localGiver = (q.questGiverNpcId != 0 &&
                                       creatureIds.count(q.questGiverNpcId) == 0);
                    bool localTurn  = (q.turnInNpcId != 0 &&
                                       q.turnInNpcId != q.questGiverNpcId &&
                                       creatureIds.count(q.turnInNpcId) == 0);
                    // Only flag IDs that look 'small' (likely zone-local).
                    // Production uses 6-digit IDs that reference upstream
                    // content; designers wire those in deliberately.
                    if (localGiver && q.questGiverNpcId < 100000) {
                        questsMissing++;
                        if (errors.size() < 30) {
                            errors.push_back("quest[" + std::to_string(k) + "] '" +
                                             q.title + "' giver " +
                                             std::to_string(q.questGiverNpcId) +
                                             " not in creatures.json");
                        }
                    }
                    if (localTurn && q.turnInNpcId < 100000) {
                        questsMissing++;
                        if (errors.size() < 30) {
                            errors.push_back("quest[" + std::to_string(k) + "] '" +
                                             q.title + "' turn-in " +
                                             std::to_string(q.turnInNpcId) +
                                             " not in creatures.json");
                        }
                    }
                }
            }
            int totalErrors = objectsMissing + questsMissing;
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["objectsChecked"] = objectsChecked;
                j["objectsMissing"] = objectsMissing;
                j["questsChecked"] = questsChecked;
                j["questsMissing"] = questsMissing;
                j["errors"] = errors;
                j["passed"] = (totalErrors == 0);
                std::printf("%s\n", j.dump(2).c_str());
                return totalErrors == 0 ? 0 : 1;
            }
            std::printf("Zone refs: %s\n", zoneDir.c_str());
            std::printf("  objects checked  : %d (%d missing)\n",
                        objectsChecked, objectsMissing);
            std::printf("  quests checked   : %d (%d bad NPC refs)\n",
                        questsChecked, questsMissing);
            if (totalErrors == 0) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %d issue(s):\n", totalErrors);
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--for-each-zone") == 0 && i + 1 < argc) {
            // Batch runner: enumerates zones in <projectDir> and runs the
            // command after '--' for each one. '{}' in the command is
            // substituted with the zone path (find -exec convention).
            //
            //   wowee_editor --for-each-zone custom_zones -- \\
            //     wowee_editor --validate-all {}
            //
            // Returns the count of failed runs as the exit code (capped
            // at 255 so the shell can still see it).
            std::string projectDir = argv[++i];
            // The literal '--' separates the projectDir from the command.
            // Skip it; everything after is the command template.
            if (i + 1 < argc && std::strcmp(argv[i + 1], "--") == 0) ++i;
            if (i + 1 >= argc) {
                std::fprintf(stderr,
                    "for-each-zone: need command after '--'\n");
                return 1;
            }
            // Collect command tokens until end of argv. Don't try to be
            // clever about quoting — just escape each token for shell
            // safety using single quotes (' inside is escaped as '\\'').
            std::vector<std::string> cmdTokens;
            for (int k = i + 1; k < argc; ++k) cmdTokens.push_back(argv[k]);
            i = argc - 1;  // consume rest of argv
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr, "for-each-zone: %s is not a directory\n",
                             projectDir.c_str());
                return 1;
            }
            // Find every child dir that contains a zone.json — that's the
            // canonical 'is this a zone?' test the rest of the editor uses.
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (fs::exists(entry.path() / "zone.json")) {
                    zones.push_back(entry.path().string());
                }
            }
            std::sort(zones.begin(), zones.end());
            if (zones.empty()) {
                std::fprintf(stderr, "for-each-zone: no zones found in %s\n",
                             projectDir.c_str());
                return 1;
            }
            auto shellEscape = [](const std::string& s) {
                std::string out = "'";
                for (char c : s) {
                    if (c == '\'') out += "'\\''";
                    else out += c;
                }
                out += "'";
                return out;
            };
            int failed = 0;
            for (const auto& zone : zones) {
                std::string cmd;
                for (size_t k = 0; k < cmdTokens.size(); ++k) {
                    if (k > 0) cmd += " ";
                    std::string token = cmdTokens[k];
                    // Replace {} with zone path (every occurrence).
                    size_t pos;
                    while ((pos = token.find("{}")) != std::string::npos) {
                        token.replace(pos, 2, zone);
                    }
                    cmd += shellEscape(token);
                }
                std::printf("[%s]\n", zone.c_str());
                // Flush before std::system so the header lands above the
                // child's output rather than after (parent stdout is line-
                // buffered, child writes go straight to the terminal).
                std::fflush(stdout);
                int rc = std::system(cmd.c_str());
                if (rc != 0) {
                    failed++;
                    std::fprintf(stderr,
                        "for-each-zone: command exited %d for %s\n",
                        rc, zone.c_str());
                }
            }
            std::printf("\nfor-each-zone: %zu zones, %d failed\n",
                        zones.size(), failed);
            return failed > 255 ? 255 : failed;
        } else if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("Wowee World Editor v1.0.0\n");
            std::printf("Open formats: WOT/WHM/WOM/WOB/WOC/WCP + PNG/JSON (all novel)\n");
            std::printf("By Kelsi Davis\n");
            return 0;
        } else if (std::strcmp(argv[i], "--list-commands") == 0) {
            // Capture printUsage's stdout and grep for '--flag' tokens at
            // the start of each line. This auto-tracks the help text as
            // commands are added — no parallel list to maintain. Result
            // is a sorted, deduped, one-per-line list of recognized flags.
            FILE* old = stdout;
            // Temp file lets us read printUsage's output back. fmemopen
            // would be cleaner but isn't available on Windows; tmpfile is
            // portable.
            FILE* tmp = std::tmpfile();
            if (!tmp) { std::fprintf(stderr, "list-commands: tmpfile failed\n"); return 1; }
            stdout = tmp;
            printUsage(argv[0]);
            stdout = old;
            std::fseek(tmp, 0, SEEK_SET);
            std::set<std::string> commands;
            char line[512];
            while (std::fgets(line, sizeof(line), tmp)) {
                // Match leading whitespace then '--' then [a-z-]+
                const char* p = line;
                while (*p == ' ' || *p == '\t') ++p;
                if (p[0] != '-' || p[1] != '-') continue;
                std::string flag;
                while (*p && (std::isalnum(static_cast<unsigned char>(*p)) ||
                              *p == '-' || *p == '_')) {
                    flag += *p++;
                }
                if (flag.size() > 2) commands.insert(flag);
            }
            std::fclose(tmp);
            // Always include the meta-flags that printUsage describes
            // alongside others (-h/-v aliases) since the regex above only
            // captures double-dash forms.
            commands.insert("--help");
            commands.insert("--version");
            for (const auto& c : commands) std::printf("%s\n", c.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--gen-completion") == 0 && i + 1 < argc) {
            // Emit a bash or zsh completion script. Re-execs the editor's
            // own --list-commands at completion time so newly-added flags
            // light up automatically without regenerating the script.
            std::string shell = argv[++i];
            if (shell != "bash" && shell != "zsh") {
                std::fprintf(stderr,
                    "gen-completion: shell must be 'bash' or 'zsh', got '%s'\n",
                    shell.c_str());
                return 1;
            }
            // Use argv[0] as the binary name in the completion so it
            // works whether the user installed it as 'wowee_editor' or
            // a custom alias. Strip directory components for the
            // completion-name registration (bash 'complete -F' expects
            // a basename).
            std::string self = argv[0];
            auto slash = self.find_last_of('/');
            std::string baseName = (slash != std::string::npos)
                ? self.substr(slash + 1)
                : self;
            if (shell == "bash") {
                std::printf(
                    "# wowee_editor bash completion — source from ~/.bashrc:\n"
                    "#   source <(%s --gen-completion bash)\n"
                    "_wowee_editor_complete() {\n"
                    "  local cur prev cmds\n"
                    "  COMPREPLY=()\n"
                    "  cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
                    "  prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
                    "  # Cache the command list per shell session.\n"
                    "  if [[ -z \"$_WOWEE_EDITOR_CMDS\" ]]; then\n"
                    "    _WOWEE_EDITOR_CMDS=$(%s --list-commands 2>/dev/null)\n"
                    "  fi\n"
                    "  if [[ \"$cur\" == --* ]]; then\n"
                    "    COMPREPLY=( $(compgen -W \"$_WOWEE_EDITOR_CMDS\" -- \"$cur\") )\n"
                    "    return 0\n"
                    "  fi\n"
                    "  # Default: complete file paths for arg slots.\n"
                    "  COMPREPLY=( $(compgen -f -- \"$cur\") )\n"
                    "}\n"
                    "complete -F _wowee_editor_complete %s\n",
                    self.c_str(), self.c_str(), baseName.c_str());
            } else {
                // zsh — simpler descriptor-based completion.
                std::printf(
                    "# wowee_editor zsh completion — source from ~/.zshrc:\n"
                    "#   source <(%s --gen-completion zsh)\n"
                    "_wowee_editor_complete() {\n"
                    "  local -a cmds\n"
                    "  if [[ -z \"$_WOWEE_EDITOR_CMDS\" ]]; then\n"
                    "    export _WOWEE_EDITOR_CMDS=$(%s --list-commands 2>/dev/null)\n"
                    "  fi\n"
                    "  cmds=( ${(f)_WOWEE_EDITOR_CMDS} )\n"
                    "  _arguments \"*: :($cmds)\"\n"
                    "}\n"
                    "compdef _wowee_editor_complete %s\n",
                    self.c_str(), self.c_str(), baseName.c_str());
            }
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
        if (std::strcmp(argv[i], "--convert-dbc-json") == 0 && i + 1 < argc) {
            // Standalone DBC -> JSON sidecar conversion. Mirrors what
            // asset_extract --emit-open does for one file at a time, so
            // designers don't have to re-run a full extraction just to
            // refresh one DBC sidecar.
            std::string dbcPath = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (outPath.empty()) {
                outPath = dbcPath;
                if (outPath.size() >= 4 &&
                    outPath.substr(outPath.size() - 4) == ".dbc") {
                    outPath = outPath.substr(0, outPath.size() - 4);
                }
                outPath += ".json";
            }
            std::ifstream in(dbcPath, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "convert-dbc-json: cannot open %s\n", dbcPath.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            wowee::pipeline::DBCFile dbc;
            if (!dbc.load(bytes)) {
                std::fprintf(stderr, "convert-dbc-json: failed to parse %s\n", dbcPath.c_str());
                return 1;
            }
            // Same JSON schema asset_extract emits, so the editor's runtime
            // overlay loader picks the file up without changes.
            nlohmann::json j;
            j["format"] = "wowee-dbc-json-1.0";
            j["source"] = std::filesystem::path(dbcPath).filename().string();
            j["recordCount"] = dbc.getRecordCount();
            j["fieldCount"] = dbc.getFieldCount();
            nlohmann::json records = nlohmann::json::array();
            for (uint32_t r = 0; r < dbc.getRecordCount(); ++r) {
                nlohmann::json row = nlohmann::json::array();
                for (uint32_t f = 0; f < dbc.getFieldCount(); ++f) {
                    // Same heuristic as open_format_emitter::emitJsonFromDbc:
                    // prefer string > float > uint32 based on what the
                    // bytes plausibly are. Round-trips through loadJSON.
                    uint32_t val = dbc.getUInt32(r, f);
                    std::string s = dbc.getString(r, f);
                    if (!s.empty() && s[0] != '\0' && s.size() < 200) {
                        row.push_back(s);
                    } else {
                        float fv = dbc.getFloat(r, f);
                        if (val != 0 && fv != 0.0f && fv > -1e10f && fv < 1e10f &&
                            static_cast<uint32_t>(fv) != val) {
                            row.push_back(fv);
                        } else {
                            row.push_back(val);
                        }
                    }
                }
                records.push_back(std::move(row));
            }
            j["records"] = std::move(records);
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr, "convert-dbc-json: cannot write %s\n", outPath.c_str());
                return 1;
            }
            out << j.dump(2) << "\n";
            std::printf("Converted %s -> %s\n", dbcPath.c_str(), outPath.c_str());
            std::printf("  %u records x %u fields\n",
                        dbc.getRecordCount(), dbc.getFieldCount());
            return 0;
        }
        if (std::strcmp(argv[i], "--convert-json-dbc") == 0 && i + 1 < argc) {
            // Reverse direction — JSON sidecar back to binary DBC. Useful
            // for shipping edited content to private servers (AzerothCore /
            // TrinityCore) which only consume binary DBC. The output is
            // byte-compatible with the original Blizzard format.
            std::string jsonPath = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (outPath.empty()) {
                outPath = jsonPath;
                if (outPath.size() >= 5 &&
                    outPath.substr(outPath.size() - 5) == ".json") {
                    outPath = outPath.substr(0, outPath.size() - 5);
                }
                outPath += ".dbc";
            }
            std::ifstream in(jsonPath);
            if (!in) {
                std::fprintf(stderr, "convert-json-dbc: cannot open %s\n", jsonPath.c_str());
                return 1;
            }
            nlohmann::json doc;
            try { in >> doc; }
            catch (const std::exception& e) {
                std::fprintf(stderr, "convert-json-dbc: bad JSON in %s (%s)\n",
                             jsonPath.c_str(), e.what());
                return 1;
            }
            uint32_t fieldCount = doc.value("fieldCount", 0u);
            if (!doc.contains("records") || !doc["records"].is_array()) {
                std::fprintf(stderr, "convert-json-dbc: missing 'records' array in %s\n",
                             jsonPath.c_str());
                return 1;
            }
            const auto& records = doc["records"];
            uint32_t recordCount = static_cast<uint32_t>(records.size());
            if (fieldCount == 0 && recordCount > 0 && records[0].is_array()) {
                // Tolerate JSON files that drop fieldCount — derive from row.
                fieldCount = static_cast<uint32_t>(records[0].size());
            }
            if (fieldCount == 0) {
                std::fprintf(stderr,
                    "convert-json-dbc: cannot determine fieldCount in %s\n",
                    jsonPath.c_str());
                return 1;
            }
            uint32_t recordSize = fieldCount * 4;
            // Build records + string block. Strings are deduped: identical
            // strings reuse the same offset in the block. The first byte
            // of the block is always '\0' so offset=0 means empty string,
            // matching Blizzard's convention.
            std::vector<uint8_t> recordBytes(recordCount * recordSize, 0);
            std::vector<uint8_t> stringBlock;
            stringBlock.push_back(0);  // leading NUL — empty-string offset
            std::unordered_map<std::string, uint32_t> stringOffsets;
            stringOffsets[""] = 0;
            auto internString = [&](const std::string& s) -> uint32_t {
                if (s.empty()) return 0;
                auto it = stringOffsets.find(s);
                if (it != stringOffsets.end()) return it->second;
                uint32_t off = static_cast<uint32_t>(stringBlock.size());
                for (char c : s) stringBlock.push_back(static_cast<uint8_t>(c));
                stringBlock.push_back(0);
                stringOffsets[s] = off;
                return off;
            };
            int convertErrors = 0;
            for (uint32_t r = 0; r < recordCount; ++r) {
                const auto& row = records[r];
                if (!row.is_array() || row.size() != fieldCount) {
                    convertErrors++;
                    continue;
                }
                uint8_t* dst = recordBytes.data() + r * recordSize;
                for (uint32_t f = 0; f < fieldCount; ++f) {
                    uint32_t val = 0;
                    const auto& cell = row[f];
                    if (cell.is_string()) {
                        val = internString(cell.get<std::string>());
                    } else if (cell.is_number_float()) {
                        float fv = cell.get<float>();
                        std::memcpy(&val, &fv, 4);
                    } else if (cell.is_number_unsigned()) {
                        val = cell.get<uint32_t>();
                    } else if (cell.is_number_integer()) {
                        // Negative ints reinterpret as uint32 (DBC has no
                        // separate signed type; the consumer interprets).
                        int32_t sv = cell.get<int32_t>();
                        std::memcpy(&val, &sv, 4);
                    } else if (cell.is_boolean()) {
                        val = cell.get<bool>() ? 1u : 0u;
                    } else if (cell.is_null()) {
                        val = 0;
                    } else {
                        convertErrors++;
                    }
                    // Little-endian write — DBC is always LE per Blizzard
                    // format spec, regardless of host architecture.
                    dst[f * 4 + 0] =  val        & 0xFF;
                    dst[f * 4 + 1] = (val >>  8) & 0xFF;
                    dst[f * 4 + 2] = (val >> 16) & 0xFF;
                    dst[f * 4 + 3] = (val >> 24) & 0xFF;
                }
            }
            // Header: WDBC magic + 4 uint32s (recordCount, fieldCount,
            // recordSize, stringBlockSize).
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "convert-json-dbc: cannot write %s\n", outPath.c_str());
                return 1;
            }
            uint32_t header[5] = {
                0x43424457u,                       // 'WDBC' little-endian
                recordCount, fieldCount, recordSize,
                static_cast<uint32_t>(stringBlock.size())
            };
            out.write(reinterpret_cast<const char*>(header), sizeof(header));
            out.write(reinterpret_cast<const char*>(recordBytes.data()),
                      recordBytes.size());
            out.write(reinterpret_cast<const char*>(stringBlock.data()),
                      stringBlock.size());
            out.close();
            std::printf("Converted %s -> %s\n", jsonPath.c_str(), outPath.c_str());
            std::printf("  %u records x %u fields, %zu-byte string block\n",
                        recordCount, fieldCount, stringBlock.size());
            if (convertErrors > 0) {
                std::printf("  warning: %d cell(s) had unrecognized types\n", convertErrors);
            }
            return 0;
        }
        if (std::strcmp(argv[i], "--convert-blp-png") == 0 && i + 1 < argc) {
            // Standalone BLP -> PNG conversion. Same code path as
            // asset_extract --emit-open's per-file walker, but for one
            // texture without re-running a full extraction.
            std::string blpPath = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (outPath.empty()) {
                outPath = blpPath;
                if (outPath.size() >= 4 &&
                    outPath.substr(outPath.size() - 4) == ".blp") {
                    outPath = outPath.substr(0, outPath.size() - 4);
                }
                outPath += ".png";
            }
            std::ifstream in(blpPath, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "convert-blp-png: cannot open %s\n", blpPath.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            auto img = wowee::pipeline::BLPLoader::load(bytes);
            if (!img.isValid()) {
                std::fprintf(stderr, "convert-blp-png: failed to decode %s\n",
                             blpPath.c_str());
                return 1;
            }
            // Same dimension/buffer-size guards as the asset_extract
            // emitter so we never feed stbi_write_png an invalid buffer.
            const size_t expected = static_cast<size_t>(img.width) * img.height * 4;
            if (img.width <= 0 || img.height <= 0 ||
                img.width > 8192 || img.height > 8192 ||
                img.data.size() < expected) {
                std::fprintf(stderr, "convert-blp-png: invalid dimensions or data (%dx%d, %zu bytes)\n",
                             img.width, img.height, img.data.size());
                return 1;
            }
            // Ensure output directory exists; fs::create_directories with
            // an empty path is a no-op so we don't need to special-case
            // 'png in cwd'.
            std::filesystem::create_directories(
                std::filesystem::path(outPath).parent_path());
            int rc = stbi_write_png(outPath.c_str(),
                                     img.width, img.height, 4,
                                     img.data.data(), img.width * 4);
            if (!rc) {
                std::fprintf(stderr, "convert-blp-png: stbi_write_png failed for %s\n",
                             outPath.c_str());
                return 1;
            }
            std::printf("Converted %s -> %s\n", blpPath.c_str(), outPath.c_str());
            std::printf("  %dx%d, %zu bytes (RGBA8)\n",
                        img.width, img.height, img.data.size());
            return 0;
        }
        if (std::strcmp(argv[i], "--migrate-wom") == 0 && i + 1 < argc) {
            // Upgrade an older WOM (v1=static, v2=animated) to WOM3 by
            // adding a default single-batch entry that covers the whole
            // mesh. WOM3 is a strict superset; tooling that consumes
            // batches (--info-batches, --export-glb per-primitive split,
            // material-aware renderers) becomes useful on previously-
            // batchless content. The save() function picks WOM3 magic
            // automatically once batches.size() > 0.
            std::string base = argv[++i];
            std::string outBase;
            if (i + 1 < argc && argv[i + 1][0] != '-') outBase = argv[++i];
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr, "WOM not found: %s.wom\n", base.c_str());
                return 1;
            }
            if (outBase.empty()) outBase = base;
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            if (!wom.isValid()) {
                std::fprintf(stderr, "migrate-wom: %s.wom has no geometry\n", base.c_str());
                return 1;
            }
            int oldVersion = wom.version;
            int batchesAdded = 0;
            if (wom.batches.empty()) {
                // Single batch covering the entire index range with the
                // first texture (or 0 if no textures exist). Opaque
                // blend mode + no flags — safe defaults that match how
                // the renderer was treating the whole mesh implicitly.
                wowee::pipeline::WoweeModel::Batch b;
                b.indexStart = 0;
                b.indexCount = static_cast<uint32_t>(wom.indices.size());
                b.textureIndex = wom.texturePaths.empty() ? 0 : 0;
                b.blendMode = 0;
                b.flags = 0;
                wom.batches.push_back(b);
                batchesAdded = 1;
            }
            // version field is recomputed inside save() based on
            // hasBatches/hasAnimation, so we don't need to set it here.
            if (!wowee::pipeline::WoweeModelLoader::save(wom, outBase)) {
                std::fprintf(stderr, "migrate-wom: failed to write %s.wom\n",
                             outBase.c_str());
                return 1;
            }
            // Re-load to verify the new version flag landed correctly.
            auto check = wowee::pipeline::WoweeModelLoader::load(outBase);
            std::printf("Migrated %s.wom -> %s.wom\n", base.c_str(), outBase.c_str());
            std::printf("  version: %d -> %u  batches: %zu -> %zu (added %d)\n",
                        oldVersion, check.version,
                        size_t(0), check.batches.size(), batchesAdded);
            if (batchesAdded == 0) {
                std::printf("  (already had batches; no schema change)\n");
            }
            return 0;
        }
        if (std::strcmp(argv[i], "--migrate-zone") == 0 && i + 1 < argc) {
            // Batch-runs --migrate-wom in-place on every .wom under
            // a zone directory. Idempotent (already-migrated files
            // become no-ops). Useful when wowee_editor adds a new
            // WOM3-only feature and you want to upgrade legacy zones
            // in one shot.
            std::string zoneDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir) || !fs::is_directory(zoneDir)) {
                std::fprintf(stderr,
                    "migrate-zone: %s is not a directory\n", zoneDir.c_str());
                return 1;
            }
            int scanned = 0, upgraded = 0, alreadyV3 = 0, failed = 0;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file()) continue;
                std::string ext = e.path().extension().string();
                if (ext != ".wom") continue;
                scanned++;
                std::string base = e.path().string();
                if (base.size() >= 4) base = base.substr(0, base.size() - 4);
                auto wom = wowee::pipeline::WoweeModelLoader::load(base);
                if (!wom.isValid()) { failed++; continue; }
                if (!wom.batches.empty()) { alreadyV3++; continue; }
                wowee::pipeline::WoweeModel::Batch b;
                b.indexStart = 0;
                b.indexCount = static_cast<uint32_t>(wom.indices.size());
                b.textureIndex = 0;
                b.blendMode = 0;
                b.flags = 0;
                wom.batches.push_back(b);
                if (wowee::pipeline::WoweeModelLoader::save(wom, base)) {
                    upgraded++;
                    std::printf("  upgraded: %s.wom\n", base.c_str());
                } else {
                    failed++;
                    std::fprintf(stderr, "  FAILED: %s.wom\n", base.c_str());
                }
            }
            std::printf("\nmigrate-zone: %s\n", zoneDir.c_str());
            std::printf("  scanned   : %d WOM file(s)\n", scanned);
            std::printf("  upgraded  : %d (added single-batch entry)\n", upgraded);
            std::printf("  already v3: %d (no change needed)\n", alreadyV3);
            if (failed > 0) {
                std::printf("  FAILED    : %d (see stderr)\n", failed);
            }
            return failed == 0 ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--migrate-jsondbc") == 0 && i + 1 < argc) {
            // Auto-fix common schema problems in JSON DBC sidecars so they
            // pass --validate-jsondbc cleanly. Designed for upgrading
            // sidecars produced by older asset_extract versions or from
            // third-party tools that omit fields the runtime now expects:
            //   - missing 'format' tag → add 'wowee-dbc-json-1.0'
            //   - missing 'source' field → derive from filename
            //   - missing 'fieldCount' → infer from first row
            //   - recordCount mismatch → recompute from actual records[]
            // Wrong-width rows are not silently fixed (data loss risk);
            // they're surfaced as warnings so the user can decide.
            std::string path = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            if (outPath.empty()) outPath = path;  // in-place
            std::ifstream in(path);
            if (!in) {
                std::fprintf(stderr,
                    "migrate-jsondbc: cannot open %s\n", path.c_str());
                return 1;
            }
            nlohmann::json doc;
            try { in >> doc; }
            catch (const std::exception& e) {
                std::fprintf(stderr,
                    "migrate-jsondbc: bad JSON in %s (%s)\n",
                    path.c_str(), e.what());
                return 1;
            }
            in.close();
            if (!doc.is_object()) {
                std::fprintf(stderr,
                    "migrate-jsondbc: top-level value is not an object\n");
                return 1;
            }
            int fixes = 0;
            if (!doc.contains("format") || !doc["format"].is_string()) {
                doc["format"] = "wowee-dbc-json-1.0";
                fixes++;
                std::printf("  added: format = 'wowee-dbc-json-1.0'\n");
            } else if (doc["format"] != "wowee-dbc-json-1.0") {
                std::printf("  retained existing format: '%s' (not changed)\n",
                            doc["format"].get<std::string>().c_str());
            }
            if (!doc.contains("source") || !doc["source"].is_string() ||
                doc["source"].get<std::string>().empty()) {
                // Derive from input path's stem + .dbc — best-effort
                // matching the convention asset_extract uses.
                std::string stem = std::filesystem::path(path).stem().string();
                doc["source"] = stem + ".dbc";
                fixes++;
                std::printf("  added: source = '%s'\n",
                            doc["source"].get<std::string>().c_str());
            }
            // recordCount + fieldCount are non-negotiable for re-import.
            if (!doc.contains("records") || !doc["records"].is_array()) {
                std::fprintf(stderr,
                    "migrate-jsondbc: 'records' missing or not an array — cannot fix\n");
                return 1;
            }
            const auto& records = doc["records"];
            uint32_t actualCount = static_cast<uint32_t>(records.size());
            uint32_t headerCount = doc.value("recordCount", 0u);
            if (headerCount != actualCount) {
                doc["recordCount"] = actualCount;
                fixes++;
                std::printf("  fixed: recordCount %u -> %u (matches actual)\n",
                            headerCount, actualCount);
            }
            // Infer fieldCount from first row if missing.
            if (!doc.contains("fieldCount") ||
                !doc["fieldCount"].is_number_integer()) {
                if (!records.empty() && records[0].is_array()) {
                    uint32_t inferred = static_cast<uint32_t>(records[0].size());
                    doc["fieldCount"] = inferred;
                    fixes++;
                    std::printf("  inferred: fieldCount = %u (from first row)\n",
                                inferred);
                }
            }
            // Surface wrong-width rows as warnings (no auto-fix).
            uint32_t fc = doc.value("fieldCount", 0u);
            int badRows = 0;
            for (size_t r = 0; r < records.size(); ++r) {
                if (records[r].is_array() && records[r].size() != fc) {
                    if (++badRows <= 3) {
                        std::printf("  WARN: row %zu has %zu cells, expected %u\n",
                                    r, records[r].size(), fc);
                    }
                }
            }
            if (badRows > 3) {
                std::printf("  WARN: ... and %d more wrong-width rows\n",
                            badRows - 3);
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "migrate-jsondbc: cannot write %s\n", outPath.c_str());
                return 1;
            }
            out << doc.dump(2) << "\n";
            out.close();
            std::printf("Migrated %s -> %s\n", path.c_str(), outPath.c_str());
            std::printf("  fixes applied: %d\n", fixes);
            if (badRows > 0) {
                std::printf("  warnings     : %d wrong-width rows (NOT auto-fixed)\n",
                            badRows);
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
