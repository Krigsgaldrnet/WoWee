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
    static const char* kArgRequired[] = {
        "--data", "--info", "--info-batches", "--info-textures", "--info-doodads",
        "--info-attachments", "--info-particles", "--info-sequences",
        "--info-bones", "--export-bones-dot",
        "--list-zone-meshes", "--list-zone-audio", "--list-zone-textures",
        "--list-project-meshes", "--list-project-audio",
        "--list-project-textures",
        "--info-zone-models-total", "--info-project-models-total",
        "--list-zone-meshes-detail", "--list-project-meshes-detail", "--info-mesh",
        "--info-mesh-storage-budget",
        "--info-wob", "--info-woc", "--info-wot",
        "--info-creatures", "--info-objects", "--info-quests",
        "--info-extract", "--info-extract-tree", "--info-extract-budget",
        "--list-missing-sidecars",
        "--info-png", "--info-jsondbc", "--info-blp", "--info-pack-budget",
        "--info-pack-tree",
        "--info-m2", "--info-wmo", "--info-adt",
        "--info-zone", "--info-zone-overview", "--info-project-overview",
        "--copy-project", "--info-wcp", "--list-wcp",
        "--list-creatures", "--list-objects", "--list-quests",
        "--list-quest-objectives", "--list-quest-rewards",
        "--info-creature", "--info-quest", "--info-object",
        "--info-quest-graph-stats",
        "--info-creatures-by-faction", "--info-creatures-by-level",
        "--info-objects-by-path", "--info-objects-by-type",
        "--info-quests-by-level", "--info-quests-by-xp",
        "--unpack-wcp", "--pack-wcp",
        "--validate", "--validate-wom", "--validate-wob", "--validate-woc",
        "--validate-whm", "--validate-all", "--validate-project",
        "--validate-project-open-only", "--audit-project", "--bench-audit-project",
        "--bench-validate-project", "--bench-bake-project",
        "--bench-migrate-data-tree", "--list-data-tree-largest",
        "--export-data-tree-md", "--gen-texture", "--gen-mesh", "--gen-mesh-textured",
        "--add-texture-to-mesh", "--add-texture-to-zone",
        "--gen-mesh-stairs", "--gen-mesh-grid", "--gen-mesh-disc",
        "--gen-mesh-tube", "--gen-mesh-capsule", "--gen-mesh-arch",
        "--gen-mesh-pyramid", "--gen-mesh-fence", "--gen-mesh-tree",
        "--gen-mesh-rock", "--gen-mesh-pillar", "--gen-mesh-bridge",
        "--gen-mesh-tower", "--gen-mesh-house", "--gen-mesh-fountain",
        "--gen-mesh-statue", "--gen-mesh-altar", "--gen-mesh-portal",
        "--gen-mesh-archway", "--gen-mesh-barrel", "--gen-mesh-chest",
        "--gen-mesh-anvil", "--gen-mesh-mushroom", "--gen-mesh-cart",
        "--gen-mesh-banner", "--gen-mesh-grave", "--gen-mesh-bench",
        "--gen-mesh-shrine", "--gen-mesh-totem", "--gen-mesh-cage",
        "--gen-mesh-throne", "--gen-mesh-coffin", "--gen-mesh-bookshelf",
        "--gen-mesh-table", "--gen-mesh-lamppost", "--gen-mesh-bed",
        "--gen-mesh-ladder", "--gen-mesh-well", "--gen-mesh-signpost",
        "--gen-mesh-mailbox", "--gen-mesh-tombstone", "--gen-mesh-crate",
        "--gen-mesh-stool", "--gen-mesh-cauldron", "--gen-mesh-gate",
        "--gen-texture-gradient",
        "--gen-mesh-from-heightmap", "--export-mesh-heightmap",
        "--displace-mesh",
        "--scale-mesh", "--translate-mesh", "--strip-mesh",
        "--gen-texture-noise", "--gen-texture-noise-color", "--rotate-mesh",
        "--center-mesh", "--flip-mesh-normals", "--mirror-mesh",
        "--smooth-mesh-normals",
        "--merge-meshes",
        "--gen-texture-radial", "--gen-texture-stripes", "--gen-texture-dots",
        "--gen-texture-rings", "--gen-texture-checker", "--gen-texture-brick",
        "--gen-texture-wood", "--gen-texture-grass", "--gen-texture-fabric",
        "--gen-texture-cobble", "--gen-texture-marble", "--gen-texture-metal",
        "--gen-texture-leather", "--gen-texture-sand", "--gen-texture-snow",
        "--gen-texture-lava", "--gen-texture-tile", "--gen-texture-bark",
        "--gen-texture-clouds", "--gen-texture-stars", "--gen-texture-vines",
        "--gen-texture-mosaic", "--gen-texture-rust", "--gen-texture-circuit",
        "--gen-texture-coral", "--gen-texture-flame", "--gen-texture-tartan",
        "--gen-texture-argyle", "--gen-texture-herringbone",
        "--gen-texture-scales", "--gen-texture-stained-glass",
        "--gen-texture-shingles", "--gen-texture-frost",
        "--gen-texture-parquet", "--gen-texture-bubbles",
        "--gen-texture-spider-web", "--gen-texture-gingham",
        "--gen-texture-lattice", "--gen-texture-honeycomb",
        "--gen-texture-cracked",
        "--validate-glb", "--info-glb", "--info-glb-tree", "--info-glb-bytes",
        "--validate-jsondbc", "--check-glb-bounds", "--validate-stl",
        "--validate-png", "--validate-blp",
        "--zone-summary", "--info-zone-tree", "--info-project-tree",
        "--info-zone-bytes", "--info-project-bytes",
        "--info-zone-extents", "--info-project-extents",
        "--info-zone-water", "--info-project-water",
        "--info-zone-density", "--info-project-density",
        "--export-zone-summary-md", "--export-quest-graph",
        "--export-zone-csv", "--export-zone-html", "--export-project-html",
        "--export-project-md", "--export-zone-checksum", "--export-project-checksum",
        "--validate-project-checksum",
        "--scaffold-zone", "--mvp-zone", "--add-tile", "--remove-tile", "--list-tiles",
        "--for-each-zone", "--for-each-tile", "--zone-stats", "--info-tilemap",
        "--list-zone-deps", "--list-project-orphans", "--remove-project-orphans",
        "--check-zone-refs", "--check-zone-content",
        "--check-project-content", "--check-project-refs",
        "--export-zone-deps-md", "--export-zone-spawn-png",
        "--add-creature", "--add-object", "--add-quest", "--add-item",
        "--random-populate-zone", "--random-populate-items",
        "--info-zone-audio", "--snap-zone-to-ground", "--audit-zone-spawns",
        "--info-project-audio", "--snap-project-to-ground",
        "--audit-project-spawns", "--list-zone-spawns", "--list-project-spawns",
        "--gen-random-zone", "--gen-random-project", "--gen-zone-texture-pack",
        "--gen-zone-mesh-pack", "--gen-zone-starter-pack",
        "--gen-project-starter-pack", "--gen-audio-tone",
        "--gen-audio-noise", "--gen-audio-sweep", "--gen-zone-audio-pack",
        "--info-zone-summary", "--info-project-summary",
        "--info-zone-deps", "--info-project-deps",
        "--gen-zone-readme", "--gen-project-readme",
        "--validate-zone-pack", "--validate-project-packs", "--info-spawn",
        "--diff-zone-spawns",
        "--list-items", "--info-item", "--set-item", "--export-zone-items-md",
        "--export-project-items-md", "--export-project-items-csv",
        "--add-quest-objective", "--add-quest-reward-item", "--set-quest-reward",
        "--remove-quest-objective", "--clone-quest", "--clone-creature",
        "--clone-item", "--validate-items", "--validate-project-items",
        "--info-project-items",
        "--clone-object",
        "--remove-creature", "--remove-object", "--remove-quest", "--remove-item",
        "--copy-zone-items",
        "--copy-zone", "--rename-zone", "--remove-zone",
        "--clear-zone-content", "--strip-zone", "--strip-project",
        "--repair-zone", "--repair-project",
        "--gen-makefile", "--gen-project-makefile",
        "--build-woc", "--regen-collision", "--fix-zone",
        "--export-png", "--export-obj", "--import-obj",
        "--export-wob-obj", "--import-wob-obj",
        "--export-woc-obj", "--export-whm-obj",
        "--export-glb", "--export-wob-glb", "--export-whm-glb",
        "--export-stl", "--import-stl",
        "--bake-zone-glb", "--bake-zone-stl", "--bake-zone-obj",
        "--bake-project-obj", "--bake-project-stl", "--bake-project-glb",
        "--convert-m2", "--convert-m2-batch",
        "--convert-wmo", "--convert-wmo-batch",
        "--convert-dbc-json", "--convert-dbc-batch", "--convert-json-dbc",
        "--convert-blp-png", "--convert-blp-batch",
        "--migrate-wom", "--migrate-zone", "--migrate-project",
        "--migrate-data-tree", "--info-data-tree", "--strip-data-tree",
        "--audit-data-tree",
        "--migrate-jsondbc",
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
        }
        if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            dataPath = argv[++i];
        } else if (std::strcmp(argv[i], "--adt") == 0 && i + 3 < argc) {
            adtMap = argv[++i];
            adtX = std::atoi(argv[++i]);
            adtY = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--info-zone-models-total") == 0 && i + 1 < argc) {
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
        } else if (std::strcmp(argv[i], "--list-zone-meshes-detail") == 0 && i + 1 < argc) {
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
        } else if (std::strcmp(argv[i], "--info-mesh") == 0 && i + 1 < argc) {
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
        } else if (std::strcmp(argv[i], "--info-mesh-storage-budget") == 0 && i + 1 < argc) {
            // Estimated bytes-per-category breakdown for a WOM.
            // Numbers are based on the in-memory struct sizes, not
            // the actual on-disk encoding (which has framing
            // overhead) — but the relative shares are accurate and
            // help users decide where shrinking efforts pay off.
            //
            // For example: a heightmap mesh's bytes are dominated by
            // vertices, so reducing vertex count is the lever to
            // pull. A skeletal mesh's animation keyframes can dwarf
            // the geometry itself — surfacing that lets the user
            // know to consider --strip-mesh --anims.
            std::string base = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom") {
                base = base.substr(0, base.size() - 4);
            }
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr,
                    "info-mesh-storage-budget: %s.wom does not exist\n",
                    base.c_str());
                return 1;
            }
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            if (!wom.isValid()) {
                std::fprintf(stderr,
                    "info-mesh-storage-budget: failed to load %s.wom\n",
                    base.c_str());
                return 1;
            }
            // Per-category byte estimates. Vertex is 12+12+8+4+4=40
            // bytes (pos/normal/uv/4 weights/4 indices). Index is
            // 4 bytes. Bone is 4+2+12+4=22 bytes. Batch is 4+4+4+2+
            // 2=16. Animation keyframe is 4+12+16+12=44 bytes.
            // Texture path is summed length plus a small per-string
            // overhead.
            uint64_t vertBytes = wom.vertices.size() * 40;
            uint64_t idxBytes = wom.indices.size() * 4;
            uint64_t boneBytes = wom.bones.size() * 22;
            uint64_t batchBytes = wom.batches.size() * 16;
            uint64_t animBytes = 0;
            size_t totalKeyframes = 0;
            for (const auto& a : wom.animations) {
                animBytes += 12;  // id + duration + movingSpeed
                for (const auto& bone : a.boneKeyframes) {
                    animBytes += bone.size() * 44;
                    totalKeyframes += bone.size();
                }
            }
            uint64_t texBytes = 0;
            for (const auto& t : wom.texturePaths) texBytes += t.size() + 8;
            namespace fs = std::filesystem;
            uint64_t actualBytes = fs::file_size(base + ".wom");
            uint64_t estBytes = vertBytes + idxBytes + boneBytes +
                                 batchBytes + animBytes + texBytes;
            struct Row { const char* name; uint64_t bytes; };
            std::vector<Row> rows = {
                {"vertices  ", vertBytes},
                {"indices   ", idxBytes},
                {"bones     ", boneBytes},
                {"animations", animBytes},
                {"batches   ", batchBytes},
                {"textures  ", texBytes},
            };
            if (jsonOut) {
                nlohmann::json j;
                j["base"] = base;
                j["fileBytes"] = actualBytes;
                j["estimatedBytes"] = estBytes;
                j["categories"] = nlohmann::json::object();
                for (const auto& r : rows) {
                    double share = estBytes > 0
                                   ? 100.0 * r.bytes / estBytes : 0.0;
                    j["categories"][r.name] = {{"bytes", r.bytes},
                                                {"share", share}};
                }
                j["counts"] = {{"vertices", wom.vertices.size()},
                                {"indices", wom.indices.size()},
                                {"bones", wom.bones.size()},
                                {"animations", wom.animations.size()},
                                {"keyframes", totalKeyframes},
                                {"batches", wom.batches.size()},
                                {"textures", wom.texturePaths.size()}};
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Mesh storage budget: %s.wom\n", base.c_str());
            std::printf("  on-disk    : %llu bytes (%.1f KB)\n",
                        static_cast<unsigned long long>(actualBytes),
                        actualBytes / 1024.0);
            std::printf("  estimated  : %llu bytes (sum of in-memory parts)\n",
                        static_cast<unsigned long long>(estBytes));
            std::printf("\n  Per-category (estimated):\n");
            for (const auto& r : rows) {
                if (r.bytes == 0) continue;
                double share = estBytes > 0
                               ? 100.0 * r.bytes / estBytes : 0.0;
                std::printf("    %s : %10llu bytes  (%5.1f%%)\n",
                            r.name,
                            static_cast<unsigned long long>(r.bytes),
                            share);
            }
            std::printf("\n  Tips:\n");
            if (animBytes > vertBytes && wom.animations.size() > 0) {
                std::printf("    - animations dominate; --strip-mesh "
                            "--anims would save %.1f KB\n",
                            animBytes / 1024.0);
            }
            if (boneBytes > vertBytes / 2 && wom.bones.size() > 0) {
                std::printf("    - bones non-trivial; consider "
                            "--strip-mesh --bones for static placement\n");
            }
            if (vertBytes > estBytes / 2) {
                std::printf("    - vertices dominate; check if a "
                            "lower-poly variant works for placement\n");
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-project-models-total") == 0 && i + 1 < argc) {
            // Multi-zone aggregate. Walks every zone in <projectDir>,
            // sums the same WOM/WOB metrics --info-zone-models-total
            // emits, and prints a per-zone breakdown table followed
            // by project-wide totals. Useful for capacity planning
            // across an entire content project.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "info-project-models-total: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            struct ZRow {
                std::string name;
                int womCount = 0, wobCount = 0;
                uint64_t womVerts = 0, womIndices = 0, womBones = 0;
                uint64_t womAnims = 0, womBatches = 0;
                uint64_t wobGroups = 0, wobVerts = 0, wobIndices = 0;
                uint64_t wobDoodads = 0, wobPortals = 0;
            };
            std::vector<ZRow> rows;
            ZRow tot;
            tot.name = "TOTAL";
            for (const auto& zoneDir : zones) {
                ZRow r;
                r.name = fs::path(zoneDir).filename().string();
                std::error_code ec;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    std::string ext = e.path().extension().string();
                    std::string base = e.path().string();
                    if (base.size() > ext.size())
                        base = base.substr(0, base.size() - ext.size());
                    if (ext == ".wom") {
                        r.womCount++;
                        auto wom = wowee::pipeline::WoweeModelLoader::load(base);
                        r.womVerts += wom.vertices.size();
                        r.womIndices += wom.indices.size();
                        r.womBones += wom.bones.size();
                        r.womAnims += wom.animations.size();
                        r.womBatches += wom.batches.size();
                    } else if (ext == ".wob") {
                        r.wobCount++;
                        auto wob = wowee::pipeline::WoweeBuildingLoader::load(base);
                        r.wobGroups += wob.groups.size();
                        for (const auto& g : wob.groups) {
                            r.wobVerts += g.vertices.size();
                            r.wobIndices += g.indices.size();
                        }
                        r.wobDoodads += wob.doodads.size();
                        r.wobPortals += wob.portals.size();
                    }
                }
                tot.womCount += r.womCount;
                tot.wobCount += r.wobCount;
                tot.womVerts += r.womVerts;
                tot.womIndices += r.womIndices;
                tot.womBones += r.womBones;
                tot.womAnims += r.womAnims;
                tot.womBatches += r.womBatches;
                tot.wobGroups += r.wobGroups;
                tot.wobVerts += r.wobVerts;
                tot.wobIndices += r.wobIndices;
                tot.wobDoodads += r.wobDoodads;
                tot.wobPortals += r.wobPortals;
                rows.push_back(r);
            }
            if (jsonOut) {
                nlohmann::json j;
                j["project"] = projectDir;
                j["zones"] = nlohmann::json::array();
                auto rowJson = [](const ZRow& r) {
                    nlohmann::json z;
                    z["name"] = r.name;
                    z["wom"] = {{"count", r.womCount},
                                 {"vertices", r.womVerts},
                                 {"indices", r.womIndices},
                                 {"triangles", r.womIndices / 3},
                                 {"bones", r.womBones},
                                 {"animations", r.womAnims},
                                 {"batches", r.womBatches}};
                    z["wob"] = {{"count", r.wobCount},
                                 {"groups", r.wobGroups},
                                 {"vertices", r.wobVerts},
                                 {"indices", r.wobIndices},
                                 {"triangles", r.wobIndices / 3},
                                 {"doodads", r.wobDoodads},
                                 {"portals", r.wobPortals}};
                    return z;
                };
                for (const auto& r : rows) j["zones"].push_back(rowJson(r));
                j["total"] = rowJson(tot);
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Project models total: %s\n", projectDir.c_str());
            std::printf("  zones : %zu\n\n", zones.size());
            std::printf("  zone                  WOMs  WOMtri  bones  WOBs  WOBtri  doodads\n");
            for (const auto& r : rows) {
                std::printf("  %-20s %5d %7llu %6llu %5d %7llu %8llu\n",
                            r.name.substr(0, 20).c_str(),
                            r.womCount,
                            static_cast<unsigned long long>(r.womIndices / 3),
                            static_cast<unsigned long long>(r.womBones),
                            r.wobCount,
                            static_cast<unsigned long long>(r.wobIndices / 3),
                            static_cast<unsigned long long>(r.wobDoodads));
            }
            std::printf("  %-20s %5d %7llu %6llu %5d %7llu %8llu\n",
                        tot.name.c_str(),
                        tot.womCount,
                        static_cast<unsigned long long>(tot.womIndices / 3),
                        static_cast<unsigned long long>(tot.womBones),
                        tot.wobCount,
                        static_cast<unsigned long long>(tot.wobIndices / 3),
                        static_cast<unsigned long long>(tot.wobDoodads));
            std::printf("\n  Combined verts/tris (WOM+WOB): %llu / %llu\n",
                        static_cast<unsigned long long>(tot.womVerts + tot.wobVerts),
                        static_cast<unsigned long long>((tot.womIndices + tot.wobIndices) / 3));
            return 0;
        } else if (std::strcmp(argv[i], "--copy-project") == 0 && i + 2 < argc) {
            // Recursively copy an entire project tree. Refuses to
            // overwrite an existing destination so a typo doesn't
            // silently merge into the wrong project.
            std::string fromDir = argv[++i];
            std::string toDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(fromDir) || !fs::is_directory(fromDir)) {
                std::fprintf(stderr,
                    "copy-project: %s is not a directory\n", fromDir.c_str());
                return 1;
            }
            if (fs::exists(toDir)) {
                std::fprintf(stderr,
                    "copy-project: destination %s already exists "
                    "(delete it first if intentional)\n", toDir.c_str());
                return 1;
            }
            std::error_code ec;
            fs::copy(fromDir, toDir,
                      fs::copy_options::recursive | fs::copy_options::copy_symlinks,
                      ec);
            if (ec) {
                std::fprintf(stderr,
                    "copy-project: copy failed (%s)\n", ec.message().c_str());
                return 1;
            }
            // Count what was copied for the report.
            int zoneCount = 0, fileCount = 0;
            uint64_t totalBytes = 0;
            for (const auto& entry : fs::directory_iterator(toDir, ec)) {
                if (entry.is_directory() &&
                    fs::exists(entry.path() / "zone.json")) zoneCount++;
            }
            for (const auto& e : fs::recursive_directory_iterator(toDir, ec)) {
                if (e.is_regular_file()) {
                    fileCount++;
                    totalBytes += e.file_size(ec);
                }
            }
            std::printf("Copied %s -> %s\n", fromDir.c_str(), toDir.c_str());
            std::printf("  zones        : %d\n", zoneCount);
            std::printf("  files        : %d\n", fileCount);
            std::printf("  total bytes  : %llu (%.1f MB)\n",
                        static_cast<unsigned long long>(totalBytes),
                        totalBytes / (1024.0 * 1024.0));
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
        } else if (std::strcmp(argv[i], "--bench-bake-project") == 0 && i + 1 < argc) {
            // Time WHM/WOT load (the dominant cost in --bake-zone-glb/obj/
            // stl) per zone. The actual write side adds ~constant cost
            // proportional to vertex count, so load time is a strong
            // proxy. Useful for tracking 'has my latest geometry change
            // made baking 3× slower?' across releases.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "bench-bake-project: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            struct Timing {
                std::string name;
                int tiles;
                double loadMs;
                int chunks;
            };
            std::vector<Timing> timings;
            double totalMs = 0;
            for (const auto& zoneDir : zones) {
                wowee::editor::ZoneManifest zm;
                if (!zm.load(zoneDir + "/zone.json")) continue;
                Timing t{fs::path(zoneDir).filename().string(), 0, 0.0, 0};
                auto t0 = std::chrono::steady_clock::now();
                for (const auto& [tx, ty] : zm.tiles) {
                    std::string base = zoneDir + "/" + zm.mapName + "_" +
                                        std::to_string(tx) + "_" + std::to_string(ty);
                    if (!wowee::pipeline::WoweeTerrainLoader::exists(base)) continue;
                    wowee::pipeline::ADTTerrain terrain;
                    wowee::pipeline::WoweeTerrainLoader::load(base, terrain);
                    t.tiles++;
                    for (const auto& chunk : terrain.chunks) {
                        if (chunk.heightMap.isLoaded()) t.chunks++;
                    }
                }
                auto t1 = std::chrono::steady_clock::now();
                t.loadMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                totalMs += t.loadMs;
                timings.push_back(t);
            }
            double avgMs = !timings.empty() ? totalMs / timings.size() : 0.0;
            double minMs = 1e30, maxMs = 0;
            std::string slowest;
            for (const auto& t : timings) {
                if (t.loadMs < minMs) minMs = t.loadMs;
                if (t.loadMs > maxMs) { maxMs = t.loadMs; slowest = t.name; }
            }
            if (timings.empty()) { minMs = 0; maxMs = 0; }
            if (jsonOut) {
                nlohmann::json j;
                j["projectDir"] = projectDir;
                j["totalMs"] = totalMs;
                j["zoneCount"] = timings.size();
                j["avgMs"] = avgMs;
                j["minMs"] = minMs;
                j["maxMs"] = maxMs;
                j["slowestZone"] = slowest;
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& t : timings) {
                    arr.push_back({{"zone", t.name},
                                    {"loadMs", t.loadMs},
                                    {"tiles", t.tiles},
                                    {"chunks", t.chunks}});
                }
                j["perZone"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Bench bake (load-only): %s\n", projectDir.c_str());
            std::printf("  zones    : %zu\n", timings.size());
            std::printf("  total    : %.2f ms (terrain load)\n", totalMs);
            std::printf("  per zone : avg=%.2f min=%.2f max=%.2f ms\n",
                        avgMs, minMs, maxMs);
            if (!slowest.empty()) {
                std::printf("  slowest  : %s (%.2f ms)\n", slowest.c_str(), maxMs);
            }
            std::printf("\n  Per-zone:\n");
            std::printf("    zone                       ms     tiles  chunks  ms/tile\n");
            for (const auto& t : timings) {
                double mspt = t.tiles > 0 ? t.loadMs / t.tiles : 0.0;
                std::printf("    %-26s %7.2f  %5d   %5d   %6.2f\n",
                            t.name.substr(0, 26).c_str(),
                            t.loadMs, t.tiles, t.chunks, mspt);
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
        } else if (std::strcmp(argv[i], "--gen-texture") == 0 && i + 2 < argc) {
            // Synthesize a placeholder PNG texture. Lets users add a
            // working texture to their project without an external
            // image editor — useful for prototyping new meshes,
            // filling out a zone before art is final, or generating
            // test fixtures.
            //
            // <colorHex|pattern>:
            //   "RRGGBB" or "RGB" hex (case-insensitive) → solid color
            //   "checker" → 32x32 black/white checkerboard
            //   "grid"    → black background with white 1-px grid every 16
            std::string outPath = argv[++i];
            std::string spec = argv[++i];
            int W = 256, H = 256;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try { W = std::stoi(argv[++i]); } catch (...) {}
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try { H = std::stoi(argv[++i]); } catch (...) {}
            }
            if (W < 1 || H < 1 || W > 8192 || H > 8192) {
                std::fprintf(stderr,
                    "gen-texture: invalid size %dx%d (must be 1..8192)\n", W, H);
                return 1;
            }
            std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
            std::string lower = spec;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower == "checker") {
                for (int y = 0; y < H; ++y) {
                    for (int x = 0; x < W; ++x) {
                        bool dark = ((x / 32) + (y / 32)) & 1;
                        uint8_t v = dark ? 16 : 240;
                        size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
                        pixels[i2 + 0] = v;
                        pixels[i2 + 1] = v;
                        pixels[i2 + 2] = v;
                    }
                }
            } else if (lower == "grid") {
                for (int y = 0; y < H; ++y) {
                    for (int x = 0; x < W; ++x) {
                        bool line = (x % 16 == 0) || (y % 16 == 0);
                        uint8_t v = line ? 240 : 32;
                        size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
                        pixels[i2 + 0] = v;
                        pixels[i2 + 1] = v;
                        pixels[i2 + 2] = v;
                    }
                }
            } else {
                // Hex color. Accept "RGB" (3 chars) or "RRGGBB" (6 chars),
                // optional leading '#'.
                std::string hex = lower;
                if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
                auto fromHex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                    return -1;
                };
                uint8_t r = 0, g = 0, b = 0;
                if (hex.size() == 6) {
                    int hi, lo;
                    if ((hi = fromHex(hex[0])) < 0) goto bad_color;
                    if ((lo = fromHex(hex[1])) < 0) goto bad_color;
                    r = static_cast<uint8_t>((hi << 4) | lo);
                    if ((hi = fromHex(hex[2])) < 0) goto bad_color;
                    if ((lo = fromHex(hex[3])) < 0) goto bad_color;
                    g = static_cast<uint8_t>((hi << 4) | lo);
                    if ((hi = fromHex(hex[4])) < 0) goto bad_color;
                    if ((lo = fromHex(hex[5])) < 0) goto bad_color;
                    b = static_cast<uint8_t>((hi << 4) | lo);
                } else if (hex.size() == 3) {
                    int v0, v1, v2;
                    if ((v0 = fromHex(hex[0])) < 0) goto bad_color;
                    if ((v1 = fromHex(hex[1])) < 0) goto bad_color;
                    if ((v2 = fromHex(hex[2])) < 0) goto bad_color;
                    r = static_cast<uint8_t>((v0 << 4) | v0);
                    g = static_cast<uint8_t>((v1 << 4) | v1);
                    b = static_cast<uint8_t>((v2 << 4) | v2);
                } else {
                    goto bad_color;
                }
                for (int y = 0; y < H; ++y) {
                    for (int x = 0; x < W; ++x) {
                        size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
                        pixels[i2 + 0] = r;
                        pixels[i2 + 1] = g;
                        pixels[i2 + 2] = b;
                    }
                }
                goto color_ok;
              bad_color:
                std::fprintf(stderr,
                    "gen-texture: '%s' is not a valid hex color or 'checker'/'grid'\n",
                    spec.c_str());
                return 1;
              color_ok: ;
            }
            if (!stbi_write_png(outPath.c_str(), W, H, 3,
                                pixels.data(), W * 3)) {
                std::fprintf(stderr,
                    "gen-texture: stbi_write_png failed for %s\n", outPath.c_str());
                return 1;
            }
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  size      : %dx%d\n", W, H);
            std::printf("  spec      : %s\n", spec.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--add-texture-to-zone") == 0 && i + 2 < argc) {
            // Import an existing PNG into a zone directory. Useful
            // for the "I have an artist-painted texture, get it into
            // my project" workflow — complements --gen-texture
            // (procedural placeholder) and --convert-blp-png (legacy
            // BLP migration).
            //
            // Optional <renameTo> argument lets the user store the
            // PNG under a project-specific name (e.g., a generic
            // "stone.png" downloaded from a tileset becomes
            // "courtyard_floor.png" in the zone).
            //
            // Refuses to overwrite an existing destination unless the
            // source and destination are byte-identical (idempotent
            // re-runs are safe).
            std::string zoneDir = argv[++i];
            std::string srcPng = argv[++i];
            std::string renameTo;
            if (i + 1 < argc && argv[i + 1][0] != '-') renameTo = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir) || !fs::is_directory(zoneDir)) {
                std::fprintf(stderr,
                    "add-texture-to-zone: %s is not a directory\n",
                    zoneDir.c_str());
                return 1;
            }
            if (!fs::exists(srcPng) || !fs::is_regular_file(srcPng)) {
                std::fprintf(stderr,
                    "add-texture-to-zone: %s is not a file\n",
                    srcPng.c_str());
                return 1;
            }
            // Sanity-check: must end in .png (any case) so users
            // don't accidentally drop a .blp/.tga and get surprised
            // when nothing renders.
            std::string srcExt = fs::path(srcPng).extension().string();
            std::transform(srcExt.begin(), srcExt.end(), srcExt.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (srcExt != ".png") {
                std::fprintf(stderr,
                    "add-texture-to-zone: %s is not a .png "
                    "(use --convert-blp-png for .blp first)\n",
                    srcPng.c_str());
                return 1;
            }
            std::string destLeaf = renameTo.empty()
                                   ? fs::path(srcPng).filename().string()
                                   : renameTo;
            // If the rename arg lacks an extension, append .png so
            // common typos ("stone" -> "stone.png") just work.
            if (fs::path(destLeaf).extension().string().empty()) {
                destLeaf += ".png";
            }
            std::string destPath = zoneDir + "/" + destLeaf;
            std::error_code ec;
            if (fs::exists(destPath)) {
                // Allow re-running if the bytes already match — makes
                // makefile-driven workflows idempotent.
                if (fs::file_size(srcPng, ec) == fs::file_size(destPath, ec)) {
                    std::ifstream a(srcPng, std::ios::binary);
                    std::ifstream b(destPath, std::ios::binary);
                    std::stringstream sa, sb;
                    sa << a.rdbuf(); sb << b.rdbuf();
                    if (sa.str() == sb.str()) {
                        std::printf("Already present: %s (no-op)\n",
                                    destPath.c_str());
                        return 0;
                    }
                }
                std::fprintf(stderr,
                    "add-texture-to-zone: %s already exists with different "
                    "content (delete it first if intentional)\n",
                    destPath.c_str());
                return 1;
            }
            fs::copy_file(srcPng, destPath, ec);
            if (ec) {
                std::fprintf(stderr,
                    "add-texture-to-zone: copy failed (%s)\n",
                    ec.message().c_str());
                return 1;
            }
            uint64_t bytes = fs::file_size(destPath, ec);
            std::printf("Imported %s -> %s\n",
                        srcPng.c_str(), destPath.c_str());
            std::printf("  bytes : %llu\n",
                        static_cast<unsigned long long>(bytes));
            std::printf("  next  : --add-texture-to-mesh <wom-base> %s\n",
                        destPath.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--info-tilemap") == 0 && i + 1 < argc) {
            // Visualize the WoW 64x64 ADT grid showing which tiles are
            // claimed by which zones across a project. Useful for
            // spotting tile-coord collisions before two zones try to
            // ship overlapping content, and for getting a 'where am I
            // working?' overview of a multi-zone project.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "info-tilemap: %s is not a directory\n", projectDir.c_str());
                return 1;
            }
            // Map (tx, ty) -> vector<zone names> so collision overlaps
            // are visible. Walk every zone in the project.
            std::map<std::pair<int,int>, std::vector<std::string>> claims;
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                wowee::editor::ZoneManifest zm;
                if (!zm.load((entry.path() / "zone.json").string())) continue;
                std::string zname = zm.mapName.empty()
                    ? entry.path().filename().string() : zm.mapName;
                zones.push_back(zname);
                for (const auto& [tx, ty] : zm.tiles) {
                    if (tx >= 0 && tx < 64 && ty >= 0 && ty < 64) {
                        claims[{tx, ty}].push_back(zname);
                    }
                }
            }
            // Per-zone label glyph: first letter of the zone name,
            // uppercased so different zones get distinct chars in the
            // grid. Multi-letter overlap collapses to '*'.
            std::map<std::string, char> zoneGlyph;
            char nextGlyph = 'A';
            for (const auto& z : zones) {
                if (zoneGlyph.count(z)) continue;
                if (!z.empty() && std::isalpha(static_cast<unsigned char>(z[0]))) {
                    zoneGlyph[z] = static_cast<char>(std::toupper(static_cast<unsigned char>(z[0])));
                } else {
                    zoneGlyph[z] = nextGlyph++;
                    if (nextGlyph > 'Z') nextGlyph = 'a';
                }
            }
            int collisions = 0;
            for (const auto& [coord, owners] : claims) {
                if (owners.size() > 1) collisions++;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["projectDir"] = projectDir;
                j["zoneCount"] = zones.size();
                j["claimedTiles"] = claims.size();
                j["collisions"] = collisions;
                nlohmann::json claimsJson = nlohmann::json::array();
                for (const auto& [coord, owners] : claims) {
                    claimsJson.push_back({{"x", coord.first},
                                           {"y", coord.second},
                                           {"zones", owners}});
                }
                j["claims"] = claimsJson;
                std::printf("%s\n", j.dump(2).c_str());
                return collisions == 0 ? 0 : 1;
            }
            std::printf("Tilemap: %s\n", projectDir.c_str());
            std::printf("  zones      : %zu\n", zones.size());
            std::printf("  tiles used : %zu\n", claims.size());
            std::printf("  collisions : %d (multiple zones claiming same tile)\n",
                        collisions);
            std::printf("  legend     :");
            for (const auto& [name, glyph] : zoneGlyph) {
                std::printf(" %c=%s", glyph, name.c_str());
            }
            std::printf("\n\n");
            // Render 64x64 grid. Print column header in groups of 10
            // for readability.
            std::printf("       ");
            for (int x = 0; x < 64; ++x) {
                std::printf("%c", (x % 10 == 0) ? '0' + (x / 10) : ' ');
            }
            std::printf("\n");
            std::printf("       ");
            for (int x = 0; x < 64; ++x) std::printf("%d", x % 10);
            std::printf("\n");
            for (int y = 0; y < 64; ++y) {
                // Skip rows that have no tiles claimed — keeps the
                // output bounded for projects in one corner of the map.
                bool rowHasContent = false;
                for (int x = 0; x < 64 && !rowHasContent; ++x) {
                    if (claims.count({x, y})) rowHasContent = true;
                }
                if (!rowHasContent) continue;
                std::printf("  y=%2d ", y);
                for (int x = 0; x < 64; ++x) {
                    auto it = claims.find({x, y});
                    if (it == claims.end()) {
                        std::printf(".");
                    } else if (it->second.size() > 1) {
                        std::printf("*");  // collision
                    } else {
                        std::printf("%c", zoneGlyph[it->second[0]]);
                    }
                }
                std::printf("\n");
            }
            if (collisions > 0) {
                std::printf("\n  COLLISIONS:\n");
                for (const auto& [coord, owners] : claims) {
                    if (owners.size() < 2) continue;
                    std::printf("    (%d, %d) claimed by:", coord.first, coord.second);
                    for (const auto& o : owners) std::printf(" %s", o.c_str());
                    std::printf("\n");
                }
            }
            return collisions == 0 ? 0 : 1;
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
        } else if (std::strcmp(argv[i], "--list-project-orphans") == 0 && i + 1 < argc) {
            // Inverse of --list-zone-deps. Walks every zone in
            // <projectDir>, collects the set of .wom/.wob files
            // sitting on disk and the set of paths actually
            // referenced by objects.json placements + WOB doodad
            // lists. Files in the first set but not the second are
            // orphans — candidates for removal before --pack-wcp so
            // the archive doesn't carry dead weight.
            //
            // Comparison is by basename (extension stripped) since
            // the reference paths sometimes include the extension and
            // sometimes don't.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "list-project-orphans: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            // Project-wide reference set. Normalize by stripping
            // extension and any leading "./".
            auto normalize = [](std::string p) {
                while (p.size() >= 2 && p[0] == '.' && p[1] == '/') p.erase(0, 2);
                std::string ext = fs::path(p).extension().string();
                if (ext == ".wom" || ext == ".wob" || ext == ".m2" || ext == ".wmo") {
                    p = p.substr(0, p.size() - ext.size());
                }
                return p;
            };
            std::set<std::string> referencedBases;  // normalized basenames
            for (const auto& zoneDir : zones) {
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile(zoneDir + "/objects.json")) {
                    for (const auto& o : op.getObjects()) {
                        if (o.path.empty()) continue;
                        // Reference can be relative to zone or just a
                        // bare model name; record both forms for the
                        // membership test.
                        std::string norm = normalize(o.path);
                        referencedBases.insert(norm);
                        // Also try the leaf basename so unqualified
                        // refs match.
                        referencedBases.insert(fs::path(norm).filename().string());
                    }
                }
                std::error_code ec;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    if (e.path().extension() != ".wob") continue;
                    std::string base = e.path().string();
                    if (base.size() >= 4) base = base.substr(0, base.size() - 4);
                    auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
                    for (const auto& d : bld.doodads) {
                        if (d.modelPath.empty()) continue;
                        std::string norm = normalize(d.modelPath);
                        referencedBases.insert(norm);
                        referencedBases.insert(fs::path(norm).filename().string());
                    }
                }
            }
            // Now walk every zone again and flag orphan .wom/.wob files.
            struct Orphan { std::string zone, path; uint64_t bytes; };
            std::vector<Orphan> orphans;
            uint64_t totalOrphanBytes = 0;
            for (const auto& zoneDir : zones) {
                std::string zoneName = fs::path(zoneDir).filename().string();
                std::error_code ec;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    std::string ext = e.path().extension().string();
                    if (ext != ".wom" && ext != ".wob") continue;
                    std::string rel = fs::relative(e.path(), zoneDir, ec).string();
                    if (ec) rel = e.path().filename().string();
                    std::string normRel = rel.substr(0, rel.size() - ext.size());
                    std::string leaf = e.path().stem().string();
                    if (referencedBases.count(normRel) ||
                        referencedBases.count(leaf)) {
                        continue;  // referenced, not orphan
                    }
                    uint64_t sz = e.file_size(ec);
                    if (ec) sz = 0;
                    orphans.push_back({zoneName, rel, sz});
                    totalOrphanBytes += sz;
                }
            }
            std::sort(orphans.begin(), orphans.end(),
                      [](const Orphan& a, const Orphan& b) {
                          if (a.zone != b.zone) return a.zone < b.zone;
                          return a.path < b.path;
                      });
            if (jsonOut) {
                nlohmann::json j;
                j["project"] = projectDir;
                j["referencedCount"] = referencedBases.size();
                j["orphanCount"] = orphans.size();
                j["orphanBytes"] = totalOrphanBytes;
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& o : orphans) {
                    arr.push_back({{"zone", o.zone},
                                    {"path", o.path},
                                    {"bytes", o.bytes}});
                }
                j["orphans"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Project orphans: %s\n", projectDir.c_str());
            std::printf("  zones scanned    : %zu\n", zones.size());
            std::printf("  refs collected   : %zu (normalized basenames)\n",
                        referencedBases.size());
            std::printf("  orphan .wom/.wob : %zu file(s), %.1f KB\n",
                        orphans.size(), totalOrphanBytes / 1024.0);
            if (orphans.empty()) {
                std::printf("\n  (no orphans — every model file is referenced)\n");
                return 0;
            }
            std::printf("\n  zone                  bytes      path\n");
            for (const auto& o : orphans) {
                std::printf("  %-20s  %8llu   %s\n",
                            o.zone.substr(0, 20).c_str(),
                            static_cast<unsigned long long>(o.bytes),
                            o.path.c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--remove-project-orphans") == 0 && i + 1 < argc) {
            // Destructive companion to --list-project-orphans. Reuses
            // the same reference-collection + orphan-detection logic
            // and then deletes the resulting files. --dry-run shows
            // what would be removed without touching anything.
            std::string projectDir = argv[++i];
            bool dryRun = false;
            if (i + 1 < argc && std::strcmp(argv[i + 1], "--dry-run") == 0) {
                dryRun = true; i++;
            }
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "remove-project-orphans: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            // Same normalize + reference collection as --list-project-orphans.
            // Keep both functions in sync if the matching rules evolve.
            auto normalize = [](std::string p) {
                while (p.size() >= 2 && p[0] == '.' && p[1] == '/') p.erase(0, 2);
                std::string ext = fs::path(p).extension().string();
                if (ext == ".wom" || ext == ".wob" || ext == ".m2" || ext == ".wmo") {
                    p = p.substr(0, p.size() - ext.size());
                }
                return p;
            };
            std::set<std::string> referencedBases;
            for (const auto& zoneDir : zones) {
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile(zoneDir + "/objects.json")) {
                    for (const auto& o : op.getObjects()) {
                        if (o.path.empty()) continue;
                        std::string norm = normalize(o.path);
                        referencedBases.insert(norm);
                        referencedBases.insert(fs::path(norm).filename().string());
                    }
                }
                std::error_code ec;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    if (e.path().extension() != ".wob") continue;
                    std::string base = e.path().string();
                    if (base.size() >= 4) base = base.substr(0, base.size() - 4);
                    auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
                    for (const auto& d : bld.doodads) {
                        if (d.modelPath.empty()) continue;
                        std::string norm = normalize(d.modelPath);
                        referencedBases.insert(norm);
                        referencedBases.insert(fs::path(norm).filename().string());
                    }
                }
            }
            int removed = 0, failed = 0;
            uint64_t freedBytes = 0;
            for (const auto& zoneDir : zones) {
                std::string zoneName = fs::path(zoneDir).filename().string();
                std::error_code ec;
                std::vector<fs::path> toRemove;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    std::string ext = e.path().extension().string();
                    if (ext != ".wom" && ext != ".wob") continue;
                    std::string rel = fs::relative(e.path(), zoneDir, ec).string();
                    if (ec) rel = e.path().filename().string();
                    std::string normRel = rel.substr(0, rel.size() - ext.size());
                    std::string leaf = e.path().stem().string();
                    if (referencedBases.count(normRel) ||
                        referencedBases.count(leaf)) continue;
                    toRemove.push_back(e.path());
                }
                // Materialize the deletion list before removing so we
                // don't mutate the directory while iterating.
                for (const auto& p : toRemove) {
                    uint64_t sz = fs::file_size(p, ec);
                    if (ec) sz = 0;
                    std::string rel = fs::relative(p, zoneDir, ec).string();
                    if (ec) rel = p.filename().string();
                    if (dryRun) {
                        std::printf("  would remove: %s/%s (%llu bytes)\n",
                                    zoneName.c_str(), rel.c_str(),
                                    static_cast<unsigned long long>(sz));
                        removed++;
                        freedBytes += sz;
                    } else {
                        if (fs::remove(p, ec)) {
                            std::printf("  removed: %s/%s (%llu bytes)\n",
                                        zoneName.c_str(), rel.c_str(),
                                        static_cast<unsigned long long>(sz));
                            removed++;
                            freedBytes += sz;
                        } else {
                            std::fprintf(stderr,
                                "  WARN: failed to remove %s (%s)\n",
                                p.c_str(), ec.message().c_str());
                            failed++;
                        }
                    }
                }
            }
            std::printf("\nremove-project-orphans: %s%s\n",
                        projectDir.c_str(), dryRun ? " (dry-run)" : "");
            std::printf("  zones    : %zu\n", zones.size());
            std::printf("  refs     : %zu (normalized basenames)\n",
                        referencedBases.size());
            std::printf("  %s : %d file(s)\n",
                        dryRun ? "would remove" : "removed     ", removed);
            std::printf("  freed    : %.1f KB\n", freedBytes / 1024.0);
            if (failed > 0) {
                std::printf("  FAILED   : %d (see stderr)\n", failed);
            }
            if (dryRun && removed > 0) {
                std::printf("  re-run without --dry-run to apply\n");
            }
            return failed == 0 ? 0 : 1;
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
        } else if (std::strcmp(argv[i], "--export-zone-spawn-png") == 0 && i + 1 < argc) {
            // Top-down PNG of spawn positions colored by type. Bound by
            // the zone's tile range so the image is properly framed.
            // Useful for design review (does the spawn distribution
            // match the intended encounter design?) and for showing
            // collaborators 'where are the mobs'.
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "export-zone-spawn-png: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr,
                    "export-zone-spawn-png: parse failed\n");
                return 1;
            }
            if (zm.tiles.empty()) {
                std::fprintf(stderr, "export-zone-spawn-png: zone has no tiles\n");
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/" + zm.mapName + "_spawns.png";
            // Compute world-space bounds from manifest tiles. Same math
            // as --info-zone-extents.
            constexpr float kTileSize = 533.33333f;
            int tileMinX = 64, tileMaxX = -1;
            int tileMinY = 64, tileMaxY = -1;
            for (const auto& [tx, ty] : zm.tiles) {
                tileMinX = std::min(tileMinX, tx);
                tileMaxX = std::max(tileMaxX, tx);
                tileMinY = std::min(tileMinY, ty);
                tileMaxY = std::max(tileMaxY, ty);
            }
            float worldMinX = (32.0f - tileMaxY - 1) * kTileSize;
            float worldMaxX = (32.0f - tileMinY)     * kTileSize;
            float worldMinY = (32.0f - tileMaxX - 1) * kTileSize;
            float worldMaxY = (32.0f - tileMinX)     * kTileSize;
            // Image dimensions: 256px per tile so detail is visible
            // without inflating per-pixel cost.
            int tilesX = tileMaxY - tileMinY + 1;  // tile.y maps to world.x
            int tilesY = tileMaxX - tileMinX + 1;
            const int kPxPerTile = 256;
            int imgW = tilesX * kPxPerTile;
            int imgH = tilesY * kPxPerTile;
            // Cap output size — 16-tile-wide projects shouldn't exceed
            // 4096 wide. Scale down if needed.
            int maxDim = std::max(imgW, imgH);
            if (maxDim > 4096) {
                int divisor = (maxDim + 4095) / 4096;
                imgW = std::max(64, imgW / divisor);
                imgH = std::max(64, imgH / divisor);
            }
            std::vector<uint8_t> img(imgW * imgH * 3, 32);  // dark grey background
            // Tile-grid lines so the boundary is visible.
            for (int t = 1; t < tilesX; ++t) {
                int x = (t * imgW) / tilesX;
                if (x >= 0 && x < imgW) {
                    for (int y = 0; y < imgH; ++y) {
                        size_t off = (y * imgW + x) * 3;
                        img[off] = img[off+1] = img[off+2] = 64;
                    }
                }
            }
            for (int t = 1; t < tilesY; ++t) {
                int y = (t * imgH) / tilesY;
                if (y >= 0 && y < imgH) {
                    for (int x = 0; x < imgW; ++x) {
                        size_t off = (y * imgW + x) * 3;
                        img[off] = img[off+1] = img[off+2] = 64;
                    }
                }
            }
            // Plot spawn points. Map world (X, Y) to image (px, py):
            //   px = (worldMaxX - X) / (worldMaxX - worldMinX) * imgW
            //   py = (worldMaxY - Y) / (worldMaxY - worldMinY) * imgH
            // since +X world is north (up) and +Y world is west (left)
            // in WoW coords.
            float wRangeX = worldMaxX - worldMinX;
            float wRangeY = worldMaxY - worldMinY;
            auto plotPoint = [&](float wx, float wy, uint8_t r, uint8_t g, uint8_t b) {
                if (wRangeX <= 0 || wRangeY <= 0) return;
                int px = static_cast<int>((worldMaxX - wx) / wRangeX * imgW);
                int py = static_cast<int>((worldMaxY - wy) / wRangeY * imgH);
                // 3×3 dot.
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int x = px + dx, y = py + dy;
                        if (x < 0 || x >= imgW || y < 0 || y >= imgH) continue;
                        size_t off = (y * imgW + x) * 3;
                        img[off] = r; img[off+1] = g; img[off+2] = b;
                    }
                }
            };
            // Creatures = red.
            wowee::editor::NpcSpawner sp;
            int creaturesPlotted = 0;
            if (sp.loadFromFile(zoneDir + "/creatures.json")) {
                for (const auto& s : sp.getSpawns()) {
                    plotPoint(s.position.x, s.position.y, 220, 60, 60);
                    creaturesPlotted++;
                }
            }
            // Objects = green (M2) / blue (WMO).
            wowee::editor::ObjectPlacer op;
            int objectsPlotted = 0;
            if (op.loadFromFile(zoneDir + "/objects.json")) {
                for (const auto& o : op.getObjects()) {
                    if (o.type == wowee::editor::PlaceableType::M2) {
                        plotPoint(o.position.x, o.position.y, 60, 200, 60);
                    } else {
                        plotPoint(o.position.x, o.position.y, 60, 120, 220);
                    }
                    objectsPlotted++;
                }
            }
            if (!stbi_write_png(outPath.c_str(), imgW, imgH, 3,
                                 img.data(), imgW * 3)) {
                std::fprintf(stderr,
                    "export-zone-spawn-png: stbi_write_png failed\n");
                return 1;
            }
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  %dx%d px, tile grid %dx%d, %d creatures (red), %d objects (green/blue)\n",
                        imgW, imgH, tilesX, tilesY, creaturesPlotted, objectsPlotted);
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
        } else if (std::strcmp(argv[i], "--check-zone-content") == 0 && i + 1 < argc) {
            // Sanity-check creature/object/quest fields for plausible
            // values. --check-zone-refs catches dangling references;
            // this catches data-quality issues like creatures with 0 HP,
            // objects with negative scale, quests with no objectives.
            // Both are needed — a quest can have valid NPC IDs (refs OK)
            // AND no objectives (content broken).
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "check-zone-content: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            std::vector<std::string> warnings;
            int creatureWarn = 0, objectWarn = 0, questWarn = 0;
            // Creatures
            wowee::editor::NpcSpawner sp;
            if (sp.loadFromFile(zoneDir + "/creatures.json")) {
                for (size_t k = 0; k < sp.spawnCount(); ++k) {
                    const auto& s = sp.getSpawns()[k];
                    if (s.name.empty()) {
                        warnings.push_back("creature[" + std::to_string(k) + "] has empty name");
                        creatureWarn++;
                    }
                    if (s.health == 0) {
                        warnings.push_back("creature[" + std::to_string(k) + "] '" +
                                          s.name + "' has 0 health");
                        creatureWarn++;
                    }
                    if (s.level == 0) {
                        warnings.push_back("creature[" + std::to_string(k) + "] '" +
                                          s.name + "' has level 0");
                        creatureWarn++;
                    }
                    if (s.minDamage > s.maxDamage) {
                        warnings.push_back("creature[" + std::to_string(k) + "] '" +
                                          s.name + "' has minDamage > maxDamage");
                        creatureWarn++;
                    }
                    if (s.scale <= 0.0f || !std::isfinite(s.scale)) {
                        warnings.push_back("creature[" + std::to_string(k) + "] '" +
                                          s.name + "' has non-positive or non-finite scale");
                        creatureWarn++;
                    }
                    if (s.displayId == 0) {
                        warnings.push_back("creature[" + std::to_string(k) + "] '" +
                                          s.name + "' has displayId=0 (will render invisibly)");
                        creatureWarn++;
                    }
                }
            }
            // Objects
            wowee::editor::ObjectPlacer op;
            if (op.loadFromFile(zoneDir + "/objects.json")) {
                for (size_t k = 0; k < op.getObjects().size(); ++k) {
                    const auto& o = op.getObjects()[k];
                    if (o.path.empty()) {
                        warnings.push_back("object[" + std::to_string(k) + "] has empty path");
                        objectWarn++;
                    }
                    if (o.scale <= 0.0f || !std::isfinite(o.scale)) {
                        warnings.push_back("object[" + std::to_string(k) +
                                          "] has non-positive or non-finite scale");
                        objectWarn++;
                    }
                    if (!std::isfinite(o.position.x) ||
                        !std::isfinite(o.position.y) ||
                        !std::isfinite(o.position.z)) {
                        warnings.push_back("object[" + std::to_string(k) +
                                          "] has non-finite position");
                        objectWarn++;
                    }
                }
            }
            // Quests
            wowee::editor::QuestEditor qe;
            if (qe.loadFromFile(zoneDir + "/quests.json")) {
                for (size_t k = 0; k < qe.questCount(); ++k) {
                    const auto& q = qe.getQuests()[k];
                    if (q.title.empty()) {
                        warnings.push_back("quest[" + std::to_string(k) + "] has empty title");
                        questWarn++;
                    }
                    if (q.objectives.empty()) {
                        warnings.push_back("quest[" + std::to_string(k) + "] '" +
                                          q.title + "' has no objectives (uncompletable)");
                        questWarn++;
                    }
                    if (q.reward.xp == 0 && q.reward.itemRewards.empty() &&
                        q.reward.gold == 0 && q.reward.silver == 0 && q.reward.copper == 0) {
                        warnings.push_back("quest[" + std::to_string(k) + "] '" +
                                          q.title + "' has no reward at all");
                        questWarn++;
                    }
                    if (q.requiredLevel == 0) {
                        warnings.push_back("quest[" + std::to_string(k) + "] '" +
                                          q.title + "' has requiredLevel=0");
                        questWarn++;
                    }
                }
            }
            int total = creatureWarn + objectWarn + questWarn;
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["creatureWarnings"] = creatureWarn;
                j["objectWarnings"] = objectWarn;
                j["questWarnings"] = questWarn;
                j["totalWarnings"] = total;
                j["warnings"] = warnings;
                j["passed"] = (total == 0);
                std::printf("%s\n", j.dump(2).c_str());
                return total == 0 ? 0 : 1;
            }
            std::printf("Zone content: %s\n", zoneDir.c_str());
            std::printf("  creature warnings: %d\n", creatureWarn);
            std::printf("  object warnings  : %d\n", objectWarn);
            std::printf("  quest warnings   : %d\n", questWarn);
            if (total == 0) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %d total warning(s):\n", total);
            for (const auto& w : warnings) std::printf("    - %s\n", w.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--check-project-content") == 0 && i + 1 < argc) {
            // Project-level content sanity check. Walks every zone and
            // runs the same per-zone checks that --check-zone-content
            // does, aggregating warnings per zone. Exit 1 if any zone
            // has any warning. Designed for CI gates before --pack-wcp.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "check-project-content: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            // Same per-zone walks as --check-zone-content. Reuse the
            // logic by counting issues directly here (cheaper than
            // shelling out to a sub-invocation per zone).
            struct ZoneRow { std::string name; int creatureWarn, objectWarn, questWarn; };
            std::vector<ZoneRow> rows;
            int projectFailedZones = 0;
            for (const auto& zoneDir : zones) {
                ZoneRow row{fs::path(zoneDir).filename().string(), 0, 0, 0};
                wowee::editor::NpcSpawner sp;
                if (sp.loadFromFile(zoneDir + "/creatures.json")) {
                    for (const auto& s : sp.getSpawns()) {
                        if (s.name.empty()) row.creatureWarn++;
                        if (s.health == 0) row.creatureWarn++;
                        if (s.level == 0) row.creatureWarn++;
                        if (s.minDamage > s.maxDamage) row.creatureWarn++;
                        if (s.scale <= 0.0f || !std::isfinite(s.scale)) row.creatureWarn++;
                        if (s.displayId == 0) row.creatureWarn++;
                    }
                }
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile(zoneDir + "/objects.json")) {
                    for (const auto& o : op.getObjects()) {
                        if (o.path.empty()) row.objectWarn++;
                        if (o.scale <= 0.0f || !std::isfinite(o.scale)) row.objectWarn++;
                        if (!std::isfinite(o.position.x) ||
                            !std::isfinite(o.position.y) ||
                            !std::isfinite(o.position.z)) row.objectWarn++;
                    }
                }
                wowee::editor::QuestEditor qe;
                if (qe.loadFromFile(zoneDir + "/quests.json")) {
                    for (const auto& q : qe.getQuests()) {
                        if (q.title.empty()) row.questWarn++;
                        if (q.objectives.empty()) row.questWarn++;
                        if (q.reward.xp == 0 && q.reward.itemRewards.empty() &&
                            q.reward.gold == 0 && q.reward.silver == 0 &&
                            q.reward.copper == 0) row.questWarn++;
                        if (q.requiredLevel == 0) row.questWarn++;
                    }
                }
                int rowTotal = row.creatureWarn + row.objectWarn + row.questWarn;
                if (rowTotal > 0) projectFailedZones++;
                rows.push_back(row);
            }
            int allPassed = (projectFailedZones == 0);
            int totalWarn = 0;
            for (const auto& r : rows) totalWarn += r.creatureWarn + r.objectWarn + r.questWarn;
            if (jsonOut) {
                nlohmann::json j;
                j["projectDir"] = projectDir;
                j["totalZones"] = zones.size();
                j["failedZones"] = projectFailedZones;
                j["totalWarnings"] = totalWarn;
                j["passed"] = bool(allPassed);
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& r : rows) {
                    arr.push_back({{"zone", r.name},
                                    {"creatureWarn", r.creatureWarn},
                                    {"objectWarn", r.objectWarn},
                                    {"questWarn", r.questWarn}});
                }
                j["zones"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return allPassed ? 0 : 1;
            }
            std::printf("check-project-content: %s\n", projectDir.c_str());
            std::printf("  zones        : %zu (%d failed)\n",
                        zones.size(), projectFailedZones);
            std::printf("  total warns  : %d\n", totalWarn);
            std::printf("\n  zone                       creat  object   quest  status\n");
            for (const auto& r : rows) {
                int rowTotal = r.creatureWarn + r.objectWarn + r.questWarn;
                std::printf("  %-26s  %5d   %5d   %5d  %s\n",
                            r.name.substr(0, 26).c_str(),
                            r.creatureWarn, r.objectWarn, r.questWarn,
                            rowTotal == 0 ? "PASS" : "FAIL");
            }
            if (allPassed) {
                std::printf("\n  ALL ZONES PASSED\n");
                return 0;
            }
            std::printf("\n  %d zone(s) have content warnings\n",
                        projectFailedZones);
            return 1;
        } else if (std::strcmp(argv[i], "--check-project-refs") == 0 && i + 1 < argc) {
            // Project-level cross-reference checker. Walks every zone
            // and runs the same model-path / NPC-id checks as
            // --check-zone-refs. Aggregates per zone with file-level
            // breakdown. Exit 1 if any zone has dangling refs.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "check-project-refs: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            // Same model-resolve logic as --check-zone-refs, applied
            // per zone with the appropriate root list.
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
            struct ZoneRow { std::string name; int objCheck, objMiss, qCheck, qMiss; };
            std::vector<ZoneRow> rows;
            int projectFailedZones = 0;
            for (const auto& zoneDir : zones) {
                ZoneRow row{fs::path(zoneDir).filename().string(), 0, 0, 0, 0};
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
                            std::string lower = base + ext;
                            for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
                            if (fs::exists(root + lower)) return true;
                        }
                    }
                    return false;
                };
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile(zoneDir + "/objects.json")) {
                    for (const auto& o : op.getObjects()) {
                        row.objCheck++;
                        bool isWMO = (o.type == wowee::editor::PlaceableType::WMO);
                        if (!modelExists(o.path, isWMO)) row.objMiss++;
                    }
                }
                wowee::editor::NpcSpawner sp;
                wowee::editor::QuestEditor qe;
                bool hasCreatures = sp.loadFromFile(zoneDir + "/creatures.json");
                std::unordered_set<uint32_t> creatureIds;
                if (hasCreatures) {
                    for (const auto& s : sp.getSpawns()) creatureIds.insert(s.id);
                }
                if (qe.loadFromFile(zoneDir + "/quests.json") && hasCreatures) {
                    for (const auto& q : qe.getQuests()) {
                        row.qCheck++;
                        bool localGiver = (q.questGiverNpcId != 0 &&
                                            q.questGiverNpcId < 100000 &&
                                            creatureIds.count(q.questGiverNpcId) == 0);
                        bool localTurn  = (q.turnInNpcId != 0 &&
                                            q.turnInNpcId < 100000 &&
                                            q.turnInNpcId != q.questGiverNpcId &&
                                            creatureIds.count(q.turnInNpcId) == 0);
                        if (localGiver) row.qMiss++;
                        if (localTurn) row.qMiss++;
                    }
                }
                if (row.objMiss + row.qMiss > 0) projectFailedZones++;
                rows.push_back(row);
            }
            int allPassed = (projectFailedZones == 0);
            int totalMiss = 0;
            for (const auto& r : rows) totalMiss += r.objMiss + r.qMiss;
            if (jsonOut) {
                nlohmann::json j;
                j["projectDir"] = projectDir;
                j["totalZones"] = zones.size();
                j["failedZones"] = projectFailedZones;
                j["totalMissing"] = totalMiss;
                j["passed"] = bool(allPassed);
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& r : rows) {
                    arr.push_back({{"zone", r.name},
                                    {"objectsChecked", r.objCheck},
                                    {"objectsMissing", r.objMiss},
                                    {"questsChecked", r.qCheck},
                                    {"questsMissing", r.qMiss}});
                }
                j["zones"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return allPassed ? 0 : 1;
            }
            std::printf("check-project-refs: %s\n", projectDir.c_str());
            std::printf("  zones        : %zu (%d failed)\n",
                        zones.size(), projectFailedZones);
            std::printf("  total missing: %d\n", totalMiss);
            std::printf("\n  zone                       obj_chk obj_miss  q_chk  q_miss  status\n");
            for (const auto& r : rows) {
                int rowMiss = r.objMiss + r.qMiss;
                std::printf("  %-26s   %5d    %5d  %5d   %5d  %s\n",
                            r.name.substr(0, 26).c_str(),
                            r.objCheck, r.objMiss, r.qCheck, r.qMiss,
                            rowMiss == 0 ? "PASS" : "FAIL");
            }
            if (allPassed) {
                std::printf("\n  ALL ZONES PASSED\n");
                return 0;
            }
            std::printf("\n  %d zone(s) have dangling refs\n", projectFailedZones);
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
        } else if (std::strcmp(argv[i], "--for-each-tile") == 0 && i + 1 < argc) {
            // Per-tile batch runner. --for-each-zone iterates zones in
            // a project; this iterates tiles within a zone. The '{}' in
            // the command template is replaced with the tile-base path
            // (zoneDir/mapName_TX_TY) — the form most tile-level
            // editor commands take.
            //
            //   wowee_editor --for-each-tile MyZone -- \\
            //     wowee_editor --build-woc {}
            //   wowee_editor --for-each-tile MyZone -- \\
            //     wowee_editor --validate-whm {}
            std::string zoneDir = argv[++i];
            if (i + 1 < argc && std::strcmp(argv[i + 1], "--") == 0) ++i;
            if (i + 1 >= argc) {
                std::fprintf(stderr,
                    "for-each-tile: need command after '--'\n");
                return 1;
            }
            std::vector<std::string> cmdTokens;
            for (int k = i + 1; k < argc; ++k) cmdTokens.push_back(argv[k]);
            i = argc - 1;
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "for-each-tile: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "for-each-tile: parse failed\n");
                return 1;
            }
            if (zm.tiles.empty()) {
                std::fprintf(stderr, "for-each-tile: zone has no tiles\n");
                return 1;
            }
            // Same shell-escape + cmd-substitution as --for-each-zone.
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
            // Sort tiles so order is deterministic across runs.
            auto tiles = zm.tiles;
            std::sort(tiles.begin(), tiles.end());
            for (const auto& [tx, ty] : tiles) {
                std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                        std::to_string(tx) + "_" + std::to_string(ty);
                std::string cmd;
                for (size_t k = 0; k < cmdTokens.size(); ++k) {
                    if (k > 0) cmd += " ";
                    std::string token = cmdTokens[k];
                    size_t pos;
                    while ((pos = token.find("{}")) != std::string::npos) {
                        token.replace(pos, 2, tileBase);
                    }
                    cmd += shellEscape(token);
                }
                std::printf("[%s (%d, %d)]\n", tileBase.c_str(), tx, ty);
                std::fflush(stdout);
                int rc = std::system(cmd.c_str());
                if (rc != 0) {
                    failed++;
                    std::fprintf(stderr,
                        "for-each-tile: command exited %d for (%d, %d)\n",
                        rc, tx, ty);
                }
            }
            std::printf("\nfor-each-tile: %zu tiles, %d failed\n",
                        tiles.size(), failed);
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
            wowee::editor::cli::printUsage(argv[0]);
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
        } else if (std::strcmp(argv[i], "--info-cli-stats") == 0) {
            // Meta-stats on the CLI surface: total command count + per-
            // category breakdown by prefix verb (--info-*, --validate-*,
            // --diff-*, etc.). Useful for tracking growth over time and
            // spotting category imbalances.
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            // Re-use --list-commands' parser. Capture printUsage stdout.
            FILE* old = stdout;
            FILE* tmp = std::tmpfile();
            if (!tmp) { std::fprintf(stderr, "info-cli-stats: tmpfile failed\n"); return 1; }
            stdout = tmp;
            wowee::editor::cli::printUsage(argv[0]);
            stdout = old;
            std::fseek(tmp, 0, SEEK_SET);
            std::set<std::string> commands;
            char line[512];
            while (std::fgets(line, sizeof(line), tmp)) {
                const char* p = line;
                while (*p == ' ' || *p == '\t') ++p;
                if (p[0] != '-' || p[1] != '-') continue;
                std::string flag;
                while (*p && (std::isalnum(static_cast<unsigned char>(*p)) ||
                              *p == '-' || *p == '_')) { flag += *p++; }
                if (flag.size() > 2) commands.insert(flag);
            }
            std::fclose(tmp);
            commands.insert("--help");
            commands.insert("--version");
            // Bucket by category — verb is the second token after '--',
            // up to the next dash. So '--info-zone-tree' -> 'info'.
            std::map<std::string, int> byCategory;
            int maxLen = 0;
            for (const auto& c : commands) {
                if (static_cast<int>(c.size()) > maxLen) maxLen = static_cast<int>(c.size());
                size_t verbStart = 2;  // skip '--'
                size_t verbEnd = c.find('-', verbStart);
                std::string verb = (verbEnd == std::string::npos)
                    ? c.substr(verbStart)
                    : c.substr(verbStart, verbEnd - verbStart);
                byCategory[verb]++;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["totalCommands"] = commands.size();
                j["maxFlagLength"] = maxLen;
                nlohmann::json cats = nlohmann::json::object();
                for (const auto& [v, c] : byCategory) cats[v] = c;
                j["byCategory"] = cats;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("CLI surface stats\n");
            std::printf("  total commands : %zu\n", commands.size());
            std::printf("  longest flag   : %d chars\n", maxLen);
            std::printf("\n  Categories (by verb prefix, sorted by count):\n");
            // Sort by count descending for the table.
            std::vector<std::pair<std::string, int>> sorted(
                byCategory.begin(), byCategory.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) {
                          return a.second > b.second;
                      });
            for (const auto& [verb, count] : sorted) {
                std::printf("    --%-12s %4d\n", verb.c_str(), count);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-cli-categories") == 0) {
            // Discovery view of every CLI flag grouped by verb prefix.
            // Where --info-cli-stats just counts per category, this
            // lists every command in each category — handy for "I
            // know I want to gen something but what shapes/textures
            // are available?"
            FILE* old = stdout;
            FILE* tmp = std::tmpfile();
            if (!tmp) {
                std::fprintf(stderr, "info-cli-categories: tmpfile failed\n");
                return 1;
            }
            stdout = tmp;
            wowee::editor::cli::printUsage(argv[0]);
            stdout = old;
            std::fseek(tmp, 0, SEEK_SET);
            std::set<std::string> commands;
            char line[512];
            while (std::fgets(line, sizeof(line), tmp)) {
                const char* p = line;
                while (*p == ' ' || *p == '\t') ++p;
                if (p[0] != '-' || p[1] != '-') continue;
                std::string flag;
                while (*p && (std::isalnum(static_cast<unsigned char>(*p)) ||
                              *p == '-' || *p == '_')) { flag += *p++; }
                if (flag.size() > 2) commands.insert(flag);
            }
            std::fclose(tmp);
            commands.insert("--help");
            commands.insert("--version");
            std::map<std::string, std::vector<std::string>> byCategory;
            for (const auto& c : commands) {
                size_t verbStart = 2;
                size_t verbEnd = c.find('-', verbStart);
                std::string verb = (verbEnd == std::string::npos)
                    ? c.substr(verbStart)
                    : c.substr(verbStart, verbEnd - verbStart);
                byCategory[verb].push_back(c);
            }
            std::printf("CLI commands by category (%zu total):\n\n",
                        commands.size());
            // Sort categories by count descending, commands within
            // each alphabetically.
            std::vector<std::pair<std::string, std::vector<std::string>>> sorted(
                byCategory.begin(), byCategory.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) {
                          if (a.second.size() != b.second.size())
                              return a.second.size() > b.second.size();
                          return a.first < b.first;
                      });
            for (const auto& [verb, cmds] : sorted) {
                std::printf("--%s (%zu):\n", verb.c_str(), cmds.size());
                for (const auto& c : cmds) {
                    std::printf("  %s\n", c.c_str());
                }
                std::printf("\n");
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-cli-help") == 0 && i + 1 < argc) {
            // Substring search through the help text. With 130+ commands,
            // 'is there a thing for X?' is a common ask — this answers it
            // without making the user scroll the full --help output:
            //
            //   wowee_editor --info-cli-help quest
            //   wowee_editor --info-cli-help validate
            //   wowee_editor --info-cli-help glb
            std::string pattern = argv[++i];
            // Lowercase the pattern for case-insensitive match.
            std::string patLower = pattern;
            for (auto& c : patLower) c = std::tolower(static_cast<unsigned char>(c));
            // Capture printUsage stdout, walk line-by-line, print every
            // line containing the pattern (case-insensitive). Continuation
            // lines (the indented description on the line after a flag)
            // are emitted along with the flag line for context.
            FILE* old = stdout;
            FILE* tmp = std::tmpfile();
            if (!tmp) {
                std::fprintf(stderr, "info-cli-help: tmpfile failed\n"); return 1;
            }
            stdout = tmp;
            wowee::editor::cli::printUsage(argv[0]);
            stdout = old;
            std::fseek(tmp, 0, SEEK_SET);
            std::vector<std::string> lines;
            char buf[1024];
            while (std::fgets(buf, sizeof(buf), tmp)) {
                std::string s = buf;
                if (!s.empty() && s.back() == '\n') s.pop_back();
                lines.push_back(std::move(s));
            }
            std::fclose(tmp);
            int matches = 0;
            for (size_t k = 0; k < lines.size(); ++k) {
                std::string lower = lines[k];
                for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
                if (lower.find(patLower) == std::string::npos) continue;
                std::printf("%s\n", lines[k].c_str());
                // Look ahead for a continuation line (indented and not
                // starting with '--'). Print it for context.
                if (k + 1 < lines.size()) {
                    const auto& next = lines[k + 1];
                    if (!next.empty() && next[0] == ' ' &&
                        next.find("--") == std::string::npos) {
                        std::printf("%s\n", next.c_str());
                    }
                }
                matches++;
            }
            if (matches == 0) {
                std::fprintf(stderr, "info-cli-help: no matches for '%s'\n",
                             pattern.c_str());
                return 1;
            }
            std::fprintf(stderr, "\n%d line(s) matched '%s'\n", matches, pattern.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--validate-cli-help") == 0) {
            // Self-check: every flag we declare in kArgRequired (the list
            // of commands needing positional args) must appear in the
            // help text printUsage emits. Catches drift where someone
            // adds a handler + argument check but forgets the help line.
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            // Capture printUsage's stdout.
            FILE* old = stdout;
            FILE* tmp = std::tmpfile();
            if (!tmp) { std::fprintf(stderr, "validate-cli-help: tmpfile failed\n"); return 1; }
            stdout = tmp;
            wowee::editor::cli::printUsage(argv[0]);
            stdout = old;
            std::fseek(tmp, 0, SEEK_SET);
            std::string helpText;
            char chunk[1024];
            while (std::fgets(chunk, sizeof(chunk), tmp)) helpText += chunk;
            std::fclose(tmp);
            // Walk kArgRequired and check each appears in the help.
            std::vector<std::string> missing;
            for (const char* opt : kArgRequired) {
                if (helpText.find(opt) == std::string::npos) {
                    missing.push_back(opt);
                }
            }
            if (jsonOut) {
                nlohmann::json j;
                j["totalArgRequired"] = sizeof(kArgRequired) / sizeof(kArgRequired[0]);
                j["missing"] = missing;
                j["passed"] = missing.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return missing.empty() ? 0 : 1;
            }
            std::printf("CLI help self-check\n");
            std::printf("  kArgRequired entries : %zu\n",
                        sizeof(kArgRequired) / sizeof(kArgRequired[0]));
            if (missing.empty()) {
                std::printf("  PASSED — every kArgRequired flag is documented\n");
                return 0;
            }
            std::printf("  FAILED — %zu flag(s) missing from help text:\n", missing.size());
            for (const auto& m : missing) std::printf("    - %s\n", m.c_str());
            return 1;
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
            wowee::editor::cli::printUsage(argv[0]);
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
