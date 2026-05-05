#include "npc_spawner.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <sstream>
#include <cmath>
#include <random>
#include <algorithm>
#include <filesystem>

namespace wowee {
namespace editor {

uint32_t NpcSpawner::nextId() { return idCounter_++; }

void NpcSpawner::placeCreature(const CreatureSpawn& spawn) {
    CreatureSpawn s = spawn;
    s.id = nextId();
    s.selected = false;
    spawns_.push_back(s);
    LOG_INFO("Creature placed: ", s.name, " (id=", s.id, ") at (",
             s.position.x, ",", s.position.y, ",", s.position.z, ")");
}

void NpcSpawner::removeCreature(int index) {
    if (index < 0 || index >= static_cast<int>(spawns_.size())) return;
    spawns_.erase(spawns_.begin() + index);
    if (selectedIdx_ == index) selectedIdx_ = -1;
    else if (selectedIdx_ > index) selectedIdx_--;
}

int NpcSpawner::selectAt(const glm::vec3& worldPos, float maxDist) {
    clearSelection();
    float bestDist = maxDist;
    int bestIdx = -1;
    for (int i = 0; i < static_cast<int>(spawns_.size()); i++) {
        float dist = glm::length(spawns_[i].position - worldPos);
        if (dist < bestDist) { bestDist = dist; bestIdx = i; }
    }
    if (bestIdx >= 0) {
        selectedIdx_ = bestIdx;
        spawns_[bestIdx].selected = true;
    }
    return bestIdx;
}

void NpcSpawner::clearSelection() {
    if (selectedIdx_ >= 0 && selectedIdx_ < static_cast<int>(spawns_.size()))
        spawns_[selectedIdx_].selected = false;
    selectedIdx_ = -1;
}

CreatureSpawn* NpcSpawner::getSelected() {
    if (selectedIdx_ < 0 || selectedIdx_ >= static_cast<int>(spawns_.size())) return nullptr;
    return &spawns_[selectedIdx_];
}

bool NpcSpawner::saveToFile(const std::string& path) const {
    auto dir = std::filesystem::path(path).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir);

    std::ofstream f(path);
    if (!f) { LOG_ERROR("Failed to write NPC file: ", path); return false; }

    f << "[\n";
    for (size_t i = 0; i < spawns_.size(); i++) {
        const auto& s = spawns_[i];
        f << "  {\n";
        f << "    \"name\": \"" << s.name << "\",\n";
        f << "    \"model\": \"" << s.modelPath << "\",\n";
        f << "    \"displayId\": " << s.displayId << ",\n";
        f << "    \"position\": [" << s.position.x << "," << s.position.y << "," << s.position.z << "],\n";
        f << "    \"orientation\": " << s.orientation << ",\n";
        f << "    \"scale\": " << s.scale << ",\n";
        f << "    \"level\": " << s.level << ",\n";
        f << "    \"health\": " << s.health << ",\n";
        f << "    \"mana\": " << s.mana << ",\n";
        f << "    \"minDamage\": " << s.minDamage << ",\n";
        f << "    \"maxDamage\": " << s.maxDamage << ",\n";
        f << "    \"armor\": " << s.armor << ",\n";
        f << "    \"faction\": " << s.faction << ",\n";
        f << "    \"behavior\": " << static_cast<int>(s.behavior) << ",\n";
        f << "    \"wanderRadius\": " << s.wanderRadius << ",\n";
        f << "    \"aggroRadius\": " << s.aggroRadius << ",\n";
        f << "    \"leashRadius\": " << s.leashRadius << ",\n";
        f << "    \"respawnTimeMs\": " << s.respawnTimeMs << ",\n";
        f << "    \"hostile\": " << (s.hostile ? "true" : "false") << ",\n";
        f << "    \"questgiver\": " << (s.questgiver ? "true" : "false") << ",\n";
        f << "    \"vendor\": " << (s.vendor ? "true" : "false") << ",\n";
        f << "    \"flightmaster\": " << (s.flightmaster ? "true" : "false") << ",\n";
        f << "    \"innkeeper\": " << (s.innkeeper ? "true" : "false") << ",\n";
        f << "    \"patrol\": [";
        for (size_t p = 0; p < s.patrolPath.size(); p++) {
            f << "[" << s.patrolPath[p].position.x << "," << s.patrolPath[p].position.y
              << "," << s.patrolPath[p].position.z << "," << s.patrolPath[p].waitTimeMs << "]";
            if (p + 1 < s.patrolPath.size()) f << ",";
        }
        f << "]\n";
        f << "  }" << (i + 1 < spawns_.size() ? "," : "") << "\n";
    }
    f << "]\n";

    LOG_INFO("NPC spawns saved: ", path, " (", spawns_.size(), " creatures)");
    return true;
}

void NpcSpawner::scatter(const CreatureSpawn& base, const glm::vec3& center,
                          float radius, int count) {
    std::mt19937 rng(static_cast<uint32_t>(center.x * 100 + center.y * 37));
    std::uniform_real_distribution<float> distAngle(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> distDist(0.0f, radius);
    std::uniform_real_distribution<float> distRot(0.0f, 360.0f);

    for (int i = 0; i < count; i++) {
        float angle = distAngle(rng);
        float dist = std::sqrt(distDist(rng) / radius) * radius;
        CreatureSpawn s = base;
        s.position = center + glm::vec3(std::cos(angle) * dist, std::sin(angle) * dist, 0.0f);
        s.orientation = distRot(rng);
        placeCreature(s);
    }
}

bool NpcSpawner::loadFromFile(const std::string& path) {
    // Simple JSON-ish parser for our format — full JSON parsing would need a library
    LOG_INFO("NPC spawn loading not yet implemented for: ", path);
    return false;
}

} // namespace editor
} // namespace wowee
