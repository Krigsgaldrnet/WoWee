#include "cli_dispatch.hpp"

#include "cli_gen_audio.hpp"
#include "cli_zone_packs.hpp"
#include "cli_audits.hpp"
#include "cli_readmes.hpp"
#include "cli_zone_inventory.hpp"
#include "cli_project_inventory.hpp"
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
#include "cli_world_map.hpp"
#include "cli_sound_catalog.hpp"
#include "cli_spawns_catalog.hpp"
#include "cli_items_catalog.hpp"
#include "cli_loot_catalog.hpp"
#include "cli_creatures_catalog.hpp"
#include "cli_quests_catalog.hpp"
#include "cli_objects_catalog.hpp"
#include "cli_factions_catalog.hpp"
#include "cli_locks_catalog.hpp"
#include "cli_skills_catalog.hpp"
#include "cli_spells_catalog.hpp"
#include "cli_achievements_catalog.hpp"
#include "cli_trainers_catalog.hpp"
#include "cli_gossip_catalog.hpp"
#include "cli_taxi_catalog.hpp"
#include "cli_talents_catalog.hpp"
#include "cli_maps_catalog.hpp"
#include "cli_chars_catalog.hpp"
#include "cli_tokens_catalog.hpp"
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

namespace wowee {
namespace editor {
namespace cli {

namespace {

// Each handler family takes (int& i, int argc, char** argv,
// int& outRc) and returns true if it claimed the flag. The
// table is walked in order until one returns true. Order
// rarely matters — flags are exact-string-matched, so two
// families can't both claim the same flag — but families with
// shorter/cheaper checks still come first by convention.
using DispatchFn = bool (*)(int&, int, char**, int&);

constexpr DispatchFn kDispatchTable[] = {
    handleGenAudio,
    handleZonePacks,
    handleAudits,
    handleReadmes,
    handleZoneInventory,
    handleProjectInventory,
    handleGenTexture,
    handleGenMesh,
    handleMeshIO,
    handleMeshEdit,
    handleWomInfo,
    handleFormatValidate,
    handleConvert,
    handleFormatInfo,
    handlePack,
    handleContentInfo,
    handleZoneInfo,
    handleDataTree,
    handleDiff,
    handleSpawnAudit,
    handleItems,
    handleExtractInfo,
    handleExport,
    handleBake,
    handleMigrate,
    handleValidateInterop,
    handleGlbInspect,
    handleWomIo,
    handleWorldIo,
    handleInfoTree,
    handleInfoBytes,
    handleInfoExtents,
    handleInfoWater,
    handleInfoDensity,
    handleInfoAudio,
    handleWorldInfo,
    handleWorldMap,
    handleSoundCatalog,
    handleSpawnsCatalog,
    handleItemsCatalog,
    handleLootCatalog,
    handleCreaturesCatalog,
    handleQuestsCatalog,
    handleObjectsCatalog,
    handleFactionsCatalog,
    handleLocksCatalog,
    handleSkillsCatalog,
    handleSpellsCatalog,
    handleAchievementsCatalog,
    handleTrainersCatalog,
    handleGossipCatalog,
    handleTaxiCatalog,
    handleTalentsCatalog,
    handleMapsCatalog,
    handleCharsCatalog,
    handleTokensCatalog,
    handleQuestObjective,
    handleQuestReward,
    handleClone,
    handleRemove,
    handleAdd,
    handleRandom,
    handleItemsExport,
    handleItemsMutate,
    handleZoneCreate,
    handleTiles,
    handleZoneMgmt,
    handleStrip,
    handleRepair,
    handleMakefile,
    handleZoneList,
    handleTilemap,
    handleDeps,
    handleForEach,
    handleCheck,
    handleIntrospect,
    handleTextureHelpers,
    handleMeshInfo,
    handleZoneData,
    handleProjectActions,
    handleZoneExport,
};

}  // namespace

bool tryDispatchAll(int& i, int argc, char** argv, int& outRc) {
    for (DispatchFn fn : kDispatchTable) {
        if (fn(i, argc, argv, outRc)) return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
