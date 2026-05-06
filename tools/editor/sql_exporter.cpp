#include "sql_exporter.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <filesystem>
#include <ctime>
#include <chrono>

namespace wowee {
namespace editor {

static std::string escapeSql(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\'') out += "''";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

bool SQLExporter::exportCreatures(const std::vector<CreatureSpawn>& spawns,
                                   const std::string& path,
                                   uint32_t mapId,
                                   uint32_t startEntry) {
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(path).parent_path());

    std::ofstream f(path);
    if (!f) return false;

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    char timeBuf[32];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&time));

    f << "-- Wowee World Editor — Creature Spawn Export\n";
    f << "-- Generated: " << timeBuf << "\n";
    f << "-- Target: AzerothCore / TrinityCore 3.3.5a\n";
    f << "-- Map ID: " << mapId << "\n";
    f << "-- Creatures: " << spawns.size() << "\n\n";

    // creature_template entries
    f << "-- =============================================\n";
    f << "-- creature_template (NPC definitions)\n";
    f << "-- =============================================\n\n";

    for (size_t i = 0; i < spawns.size(); i++) {
        const auto& s = spawns[i];
        uint32_t entry = startEntry + static_cast<uint32_t>(i);

        uint32_t npcFlags = 0;
        if (s.questgiver) npcFlags |= 0x02;
        if (s.vendor) npcFlags |= 0x80;
        if (s.flightmaster) npcFlags |= 0x02000000;
        if (s.innkeeper) npcFlags |= 0x10000;

        uint32_t unitFlags = 0;
        if (!s.hostile) unitFlags |= 0x02; // NON_ATTACKABLE

        f << "INSERT INTO `creature_template` "
          << "(`entry`, `name`, `minlevel`, `maxlevel`, `minhealth`, `maxhealth`, "
          << "`mana`, `armor`, `mindmg`, `maxdmg`, `faction`, `npcflag`, "
          << "`unit_flags`, `modelid1`, `scale`) VALUES ("
          << entry << ", "
          << "'" << escapeSql(s.name) << "', "
          << s.level << ", " << s.level << ", "
          << s.health << ", " << s.health << ", "
          << s.mana << ", " << s.armor << ", "
          << s.minDamage << ", " << s.maxDamage << ", "
          << s.faction << ", " << npcFlags << ", "
          << unitFlags << ", "
          << s.displayId << ", "
          << s.scale
          << ") ON DUPLICATE KEY UPDATE `name`='" << escapeSql(s.name) << "';\n";
    }

    // creature spawn entries
    f << "\n-- =============================================\n";
    f << "-- creature (spawn positions)\n";
    f << "-- =============================================\n\n";

    for (size_t i = 0; i < spawns.size(); i++) {
        const auto& s = spawns[i];
        uint32_t entry = startEntry + static_cast<uint32_t>(i);
        uint32_t guid = startEntry + static_cast<uint32_t>(i);

        uint8_t movementType = 0;
        if (s.behavior == CreatureBehavior::Wander) movementType = 1;
        if (s.behavior == CreatureBehavior::Patrol) movementType = 2;

        f << "INSERT INTO `creature` "
          << "(`guid`, `id`, `map`, `position_x`, `position_y`, `position_z`, "
          << "`orientation`, `spawntimesecs`, `wander_distance`, `MovementType`) VALUES ("
          << guid << ", " << entry << ", " << mapId << ", "
          << s.position.x << ", " << s.position.y << ", " << s.position.z << ", "
          << s.orientation << ", "
          << (s.respawnTimeMs / 1000) << ", "
          << s.wanderRadius << ", "
          << static_cast<int>(movementType)
          << ") ON DUPLICATE KEY UPDATE `position_x`=" << s.position.x << ";\n";
    }

    // Patrol waypoints
    bool hasPatrols = false;
    for (const auto& s : spawns) {
        if (!s.patrolPath.empty()) { hasPatrols = true; break; }
    }
    if (hasPatrols) {
        f << "\n-- =============================================\n";
        f << "-- creature_addon + waypoint_data (patrol paths)\n";
        f << "-- =============================================\n\n";

        for (size_t i = 0; i < spawns.size(); i++) {
            const auto& s = spawns[i];
            if (s.patrolPath.empty()) continue;
            uint32_t guid = startEntry + static_cast<uint32_t>(i);
            uint32_t pathId = guid;

            f << "INSERT INTO `creature_addon` (`guid`, `path_id`) VALUES ("
              << guid << ", " << pathId
              << ") ON DUPLICATE KEY UPDATE `path_id`=" << pathId << ";\n";

            for (size_t pi = 0; pi < s.patrolPath.size(); pi++) {
                const auto& wp = s.patrolPath[pi];
                f << "INSERT INTO `waypoint_data` "
                  << "(`id`, `point`, `position_x`, `position_y`, `position_z`, `delay`) VALUES ("
                  << pathId << ", " << (pi + 1) << ", "
                  << wp.position.x << ", " << wp.position.y << ", " << wp.position.z << ", "
                  << wp.waitTimeMs
                  << ") ON DUPLICATE KEY UPDATE `position_x`=" << wp.position.x << ";\n";
            }
        }
    }

    LOG_INFO("SQL exported: ", path, " (", spawns.size(), " creatures)");
    return true;
}

bool SQLExporter::exportQuests(const std::vector<Quest>& quests,
                                const std::string& path,
                                uint32_t startEntry) {
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(path).parent_path());

    std::ofstream f(path, std::ios::app);
    if (!f) return false;

    if (quests.empty()) return true;

    f << "\n-- =============================================\n";
    f << "-- quest_template (quest definitions)\n";
    f << "-- =============================================\n\n";

    for (const auto& q : quests) {
        uint32_t entry = startEntry + q.id;
        uint32_t rewardMoney = q.reward.gold * 10000 + q.reward.silver * 100 + q.reward.copper;

        f << "INSERT INTO `quest_template` "
          << "(`ID`, `LogTitle`, `LogDescription`, `QuestCompletionLog`, "
          << "`MinLevel`, `RewardXP`, `RewardMoney`) VALUES ("
          << entry << ", "
          << "'" << escapeSql(q.title) << "', "
          << "'" << escapeSql(q.description) << "', "
          << "'" << escapeSql(q.completionText) << "', "
          << q.requiredLevel << ", "
          << q.reward.xp << ", "
          << rewardMoney
          << ") ON DUPLICATE KEY UPDATE `LogTitle`='" << escapeSql(q.title) << "';\n";
    }

    LOG_INFO("SQL quests exported: ", quests.size(), " quests");
    return true;
}

bool SQLExporter::exportAll(const std::vector<CreatureSpawn>& spawns,
                             const std::vector<Quest>& quests,
                             const std::string& path,
                             uint32_t mapId,
                             uint32_t startEntry) {
    if (!exportCreatures(spawns, path, mapId, startEntry)) return false;
    if (!quests.empty()) exportQuests(quests, path, startEntry);
    return true;
}

} // namespace editor
} // namespace wowee
