#include "npc_spawner.hpp"
#include "core/logger.hpp"
#include <nlohmann/json.hpp>
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

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : spawns_) {
        nlohmann::json js;
        js["name"] = s.name;
        js["model"] = s.modelPath;
        js["displayId"] = s.displayId;
        js["position"] = {s.position.x, s.position.y, s.position.z};
        js["orientation"] = s.orientation;
        js["scale"] = s.scale;
        js["level"] = s.level;
        js["health"] = s.health;
        js["mana"] = s.mana;
        js["minDamage"] = s.minDamage;
        js["maxDamage"] = s.maxDamage;
        js["armor"] = s.armor;
        js["faction"] = s.faction;
        js["behavior"] = static_cast<int>(s.behavior);
        js["wanderRadius"] = s.wanderRadius;
        js["aggroRadius"] = s.aggroRadius;
        js["leashRadius"] = s.leashRadius;
        js["respawnTimeMs"] = s.respawnTimeMs;
        js["hostile"] = s.hostile;
        js["questgiver"] = s.questgiver;
        js["vendor"] = s.vendor;
        js["flightmaster"] = s.flightmaster;
        js["innkeeper"] = s.innkeeper;
        js["trainer"] = s.trainer;
        js["auctioneer"] = s.auctioneer;
        js["banker"] = s.banker;
        js["repair"] = s.repair;

        nlohmann::json patrol = nlohmann::json::array();
        for (const auto& p : s.patrolPath) {
            patrol.push_back({p.position.x, p.position.y, p.position.z, p.waitTimeMs});
        }
        js["patrol"] = patrol;
        arr.push_back(js);
    }

    std::ofstream f(path);
    if (!f) { LOG_ERROR("Failed to write NPC file: ", path); return false; }
    f << arr.dump(2) << "\n";

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
    std::ifstream f(path);
    if (!f) { LOG_ERROR("Failed to open NPC file: ", path); return false; }

    try {
        auto arr = nlohmann::json::parse(f);
        if (!arr.is_array()) return false;

        spawns_.clear();
        selectedIdx_ = -1;
        idCounter_ = 1;

        for (const auto& js : arr) {
            CreatureSpawn s;
            s.name = js.value("name", "");
            s.modelPath = js.value("model", "");
            s.displayId = js.value("displayId", 0u);
            s.orientation = js.value("orientation", 0.0f);
            s.scale = js.value("scale", 1.0f);
            if (s.scale < 0.1f) s.scale = 1.0f;
            s.level = js.value("level", 1u);
            s.health = js.value("health", 100u);
            s.mana = js.value("mana", 0u);
            s.minDamage = js.value("minDamage", 5u);
            s.maxDamage = js.value("maxDamage", 10u);
            s.armor = js.value("armor", 0u);
            s.faction = js.value("faction", 0u);
            s.behavior = static_cast<CreatureBehavior>(js.value("behavior", 0));
            s.wanderRadius = js.value("wanderRadius", 0.0f);
            s.aggroRadius = js.value("aggroRadius", 15.0f);
            s.leashRadius = js.value("leashRadius", 40.0f);
            s.respawnTimeMs = js.value("respawnTimeMs", 60000u);
            s.hostile = js.value("hostile", false);
            s.questgiver = js.value("questgiver", false);
            s.vendor = js.value("vendor", false);
            s.flightmaster = js.value("flightmaster", false);
            s.innkeeper = js.value("innkeeper", false);
            s.trainer = js.value("trainer", false);
            s.auctioneer = js.value("auctioneer", false);
            s.banker = js.value("banker", false);
            s.repair = js.value("repair", false);

            if (js.contains("position") && js["position"].is_array() && js["position"].size() >= 3) {
                s.position = glm::vec3(js["position"][0].get<float>(),
                                       js["position"][1].get<float>(),
                                       js["position"][2].get<float>());
            }

            if (js.contains("patrol") && js["patrol"].is_array()) {
                for (const auto& pt : js["patrol"]) {
                    if (pt.is_array() && pt.size() >= 4) {
                        PatrolPoint pp;
                        pp.position = glm::vec3(pt[0].get<float>(), pt[1].get<float>(), pt[2].get<float>());
                        pp.waitTimeMs = pt[3].get<uint32_t>();
                        s.patrolPath.push_back(pp);
                    }
                }
            }

            if (!s.name.empty()) {
                s.id = nextId();
                spawns_.push_back(s);
            }
        }

        LOG_INFO("NPC spawns loaded: ", path, " (", spawns_.size(), " creatures)");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse NPC file: ", e.what());
        return false;
    }
}

} // namespace editor
} // namespace wowee
