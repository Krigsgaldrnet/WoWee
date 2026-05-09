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
        "--gen-mesh-beehive", "--gen-mesh-weathervane",
        "--gen-mesh-scarecrow", "--gen-mesh-sundial",
        "--gen-mesh-podium",
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
        "--gen-texture-cracked", "--gen-texture-runes",
        "--gen-texture-leopard",
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
        }
        if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            dataPath = argv[++i];
        } else if (std::strcmp(argv[i], "--adt") == 0 && i + 3 < argc) {
            adtMap = argv[++i];
            adtX = std::atoi(argv[++i]);
            adtY = std::atoi(argv[++i]);
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
        } else if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("Wowee World Editor v1.0.0\n");
            std::printf("Open formats: WOT/WHM/WOM/WOB/WOC/WCP + PNG/JSON (all novel)\n");
            std::printf("By Kelsi Davis\n");
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
