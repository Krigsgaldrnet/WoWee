#include "editor_app.hpp"
#include "cli_gen_audio.hpp"
#include "cli_zone_packs.hpp"
#include "cli_audits.hpp"
#include "cli_readmes.hpp"
#include "cli_zone_inventory.hpp"
#include "cli_project_inventory.hpp"
#include "cli_help.hpp"
#include "cli_gen_texture.hpp"
#include "cli_gen_mesh.hpp"
#include "cli_mesh_io.hpp"
#include "cli_mesh_edit.hpp"
#include "cli_wom_info.hpp"
#include "cli_format_validate.hpp"
#include "cli_convert.hpp"
#include "cli_format_info.hpp"
#include "cli_pack.hpp"
#include "cli_content_info.hpp"
#include "cli_zone_info.hpp"
#include "cli_data_tree.hpp"
#include "cli_diff.hpp"
#include "cli_spawn_audit.hpp"
#include "cli_items.hpp"
#include "cli_extract_info.hpp"
#include "cli_export.hpp"
#include "cli_bake.hpp"
#include "cli_migrate.hpp"
#include "cli_convert_single.hpp"
#include "cli_validate_interop.hpp"
#include "cli_glb_inspect.hpp"
#include "cli_wom_io.hpp"
#include "cli_world_io.hpp"
#include "cli_info_tree.hpp"
#include "cli_info_bytes.hpp"
#include "cli_info_extents.hpp"
#include "cli_info_water.hpp"
#include "cli_info_density.hpp"
#include "cli_info_audio.hpp"
#include "cli_world_info.hpp"
#include "cli_quest_objective.hpp"
#include "cli_quest_reward.hpp"
#include "cli_clone.hpp"
#include "cli_remove.hpp"
#include "cli_add.hpp"
#include "cli_random.hpp"
#include "cli_items_export.hpp"
#include "cli_items_mutate.hpp"
#include "cli_zone_create.hpp"
#include "cli_tiles.hpp"
#include "cli_zone_mgmt.hpp"
#include "cli_strip.hpp"
#include "cli_repair.hpp"
#include "cli_makefile.hpp"
#include "cli_zone_list.hpp"
#include "cli_tilemap.hpp"
#include "cli_deps.hpp"
#include "cli_for_each.hpp"
#include "cli_check.hpp"
#include "cli_introspect.hpp"
#include "cli_texture_helpers.hpp"
#include "cli_mesh_info.hpp"
#include "cli_zone_data.hpp"
#include "cli_project_actions.hpp"
#include "cli_zone_export.hpp"
#include "cli_arg_required.hpp"
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
#include <iomanip>
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
#include <chrono>
#include <functional>
#include <memory>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "stb_image_write.h"
#include "stb_image.h"  // implementation in stb_image_impl.cpp

// ─── Open-format consistency checks ─────────────────────────────
// Both validators are called from the per-file CLI commands AND
// from --validate-all which walks a zone dir. Returning a vector
// of error strings (empty == passed) keeps callers simple.
// Minimal SHA-256 implementation (FIPS 180-4) used by --export-zone-checksum
// to produce hashes that interoperate with `sha256sum -c`. Not exposed beyond
// this file — about 90 LoC, no external deps. See RFC 6234 for the algorithm.



int main(int argc, char* argv[]) {
    std::string dataPath;
    std::string adtMap;
    int adtX = -1, adtY = -1;

    // Detect non-GUI options that are missing their argument and bail out
    // with a helpful message instead of silently dropping into the GUI.
    for (int i = 1; i < argc; i++) {
        for (std::size_t k = 0; k < wowee::editor::cli::kArgRequiredSize; ++k) {
            const char* opt = wowee::editor::cli::kArgRequired[k];
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
        if (std::strcmp(argv[i], "--diff-extract") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-extract requires <dirA> <dirB>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-checksum") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-checksum requires <a.sha256> <b.sha256>\n");
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
        // Modular handler families: extracted from the in-line if/else
        // chain below to keep main.cpp from sprawling further. Each
        // family lives in its own .cpp; if it matches argv[i] it
        // sets outRc and we exit. Otherwise fall through to the
        // legacy in-line dispatch.
        {
            int outRc = 0;
            if (wowee::editor::cli::handleGenAudio(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleZonePacks(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleAudits(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleReadmes(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleZoneInventory(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleProjectInventory(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleGenTexture(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleGenMesh(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleMeshIO(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleMeshEdit(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleWomInfo(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleFormatValidate(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleConvert(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleFormatInfo(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handlePack(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleContentInfo(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleZoneInfo(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleDataTree(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleDiff(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleSpawnAudit(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleItems(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleExtractInfo(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleExport(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleBake(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleMigrate(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleConvertSingle(i, argc, argv,
                                                        dataPath, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleValidateInterop(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleGlbInspect(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleWomIo(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleWorldIo(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleInfoTree(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleInfoBytes(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleInfoExtents(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleInfoWater(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleInfoDensity(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleInfoAudio(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleWorldInfo(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleQuestObjective(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleQuestReward(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleClone(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleRemove(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleAdd(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleRandom(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleItemsExport(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleItemsMutate(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleZoneCreate(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleTiles(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleZoneMgmt(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleStrip(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleRepair(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleMakefile(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleZoneList(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleTilemap(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleDeps(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleForEach(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleCheck(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleIntrospect(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleTextureHelpers(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleMeshInfo(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleZoneData(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleProjectActions(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleZoneExport(i, argc, argv, outRc)) {
                return outRc;
            }
        }
        if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            dataPath = argv[++i];
        } else if (std::strcmp(argv[i], "--adt") == 0 && i + 3 < argc) {
            adtMap = argv[++i];
            adtX = std::atoi(argv[++i]);
            adtY = std::atoi(argv[++i]);
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
