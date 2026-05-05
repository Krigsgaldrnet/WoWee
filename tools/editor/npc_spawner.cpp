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
    std::ifstream f(path);
    if (!f) { LOG_ERROR("Failed to open NPC file: ", path); return false; }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Minimal JSON parser — extract fields from our known format
    spawns_.clear();
    selectedIdx_ = -1;

    auto findStr = [&](const std::string& block, const std::string& key) -> std::string {
        auto pos = block.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = block.find(':', pos);
        if (pos == std::string::npos) return "";
        pos = block.find('"', pos + 1);
        if (pos == std::string::npos) return "";
        auto end = block.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return block.substr(pos + 1, end - pos - 1);
    };

    auto findNum = [&](const std::string& block, const std::string& key) -> float {
        auto pos = block.find("\"" + key + "\"");
        if (pos == std::string::npos) return 0;
        pos = block.find(':', pos);
        if (pos == std::string::npos) return 0;
        return std::stof(block.substr(pos + 1));
    };

    auto findBool = [&](const std::string& block, const std::string& key) -> bool {
        auto pos = block.find("\"" + key + "\"");
        if (pos == std::string::npos) return false;
        return block.find("true", pos) < block.find('\n', pos);
    };

    // Split by object boundaries
    size_t start = 0;
    while ((start = content.find('{', start)) != std::string::npos) {
        auto end = content.find('}', start);
        if (end == std::string::npos) break;
        std::string block = content.substr(start, end - start + 1);

        CreatureSpawn s;
        s.name = findStr(block, "name");
        s.modelPath = findStr(block, "model");
        s.displayId = static_cast<uint32_t>(findNum(block, "displayId"));
        s.orientation = findNum(block, "orientation");
        s.scale = findNum(block, "scale");
        if (s.scale < 0.1f) s.scale = 1.0f;
        s.level = static_cast<uint32_t>(std::max(1.0f, findNum(block, "level")));
        s.health = static_cast<uint32_t>(std::max(1.0f, findNum(block, "health")));
        s.mana = static_cast<uint32_t>(findNum(block, "mana"));
        s.minDamage = static_cast<uint32_t>(findNum(block, "minDamage"));
        s.maxDamage = static_cast<uint32_t>(findNum(block, "maxDamage"));
        s.armor = static_cast<uint32_t>(findNum(block, "armor"));
        s.faction = static_cast<uint32_t>(findNum(block, "faction"));
        s.behavior = static_cast<CreatureBehavior>(static_cast<int>(findNum(block, "behavior")));
        s.wanderRadius = findNum(block, "wanderRadius");
        s.aggroRadius = findNum(block, "aggroRadius");
        s.leashRadius = findNum(block, "leashRadius");
        s.respawnTimeMs = static_cast<uint32_t>(findNum(block, "respawnTimeMs"));
        s.hostile = findBool(block, "hostile");
        s.questgiver = findBool(block, "questgiver");
        s.vendor = findBool(block, "vendor");
        s.flightmaster = findBool(block, "flightmaster");
        s.innkeeper = findBool(block, "innkeeper");

        // Parse position array
        auto posStart = block.find("\"position\"");
        if (posStart != std::string::npos) {
            auto bk = block.find('[', posStart);
            if (bk != std::string::npos) {
                float vals[3] = {};
                int vi = 0;
                auto p = bk + 1;
                while (vi < 3 && p < block.size()) {
                    vals[vi++] = std::stof(block.substr(p));
                    p = block.find(',', p);
                    if (p == std::string::npos) break;
                    p++;
                }
                s.position = glm::vec3(vals[0], vals[1], vals[2]);
            }
        }

        if (!s.name.empty()) {
            s.id = nextId();
            spawns_.push_back(s);
        }
        start = end + 1;
    }

    LOG_INFO("NPC spawns loaded: ", path, " (", spawns_.size(), " creatures)");
    return true;
}

} // namespace editor
} // namespace wowee
