#pragma once

#include "npc_spawner.hpp"
#include "quest_editor.hpp"
#include <string>
#include <cstdint>

namespace wowee {
namespace editor {

class SQLExporter {
public:
    // Export creature spawns as AzerothCore/TrinityCore SQL
    static bool exportCreatures(const std::vector<CreatureSpawn>& spawns,
                                 const std::string& path,
                                 uint32_t mapId = 9000,
                                 uint32_t startEntry = 100000);

    // Export quest definitions as AzerothCore quest_template SQL
    static bool exportQuests(const std::vector<Quest>& quests,
                              const std::string& path,
                              uint32_t startEntry = 100000);

    // Export everything to a single SQL file
    static bool exportAll(const std::vector<CreatureSpawn>& spawns,
                           const std::vector<Quest>& quests,
                           const std::string& path,
                           uint32_t mapId = 9000,
                           uint32_t startEntry = 100000);
};

} // namespace editor
} // namespace wowee
