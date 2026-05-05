#include "quest_editor.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <filesystem>

namespace wowee {
namespace editor {

void QuestEditor::addQuest(const Quest& q) {
    Quest quest = q;
    quest.id = nextId_++;
    quests_.push_back(quest);
    LOG_INFO("Quest added: [", quest.id, "] ", quest.title);
}

void QuestEditor::removeQuest(int index) {
    if (index >= 0 && index < static_cast<int>(quests_.size()))
        quests_.erase(quests_.begin() + index);
}

Quest* QuestEditor::getQuest(int index) {
    if (index < 0 || index >= static_cast<int>(quests_.size())) return nullptr;
    return &quests_[index];
}

bool QuestEditor::saveToFile(const std::string& path) const {
    auto dir = std::filesystem::path(path).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir);

    std::ofstream f(path);
    if (!f) return false;

    f << "[\n";
    for (size_t i = 0; i < quests_.size(); i++) {
        const auto& q = quests_[i];
        f << "  {\n";
        f << "    \"id\": " << q.id << ",\n";
        f << "    \"title\": \"" << q.title << "\",\n";
        f << "    \"description\": \"" << q.description << "\",\n";
        f << "    \"completionText\": \"" << q.completionText << "\",\n";
        f << "    \"requiredLevel\": " << q.requiredLevel << ",\n";
        f << "    \"questGiverNpcId\": " << q.questGiverNpcId << ",\n";
        f << "    \"turnInNpcId\": " << q.turnInNpcId << ",\n";
        f << "    \"nextQuestId\": " << q.nextQuestId << ",\n";
        f << "    \"reward\": {\"xp\":" << q.reward.xp
          << ",\"gold\":" << q.reward.gold
          << ",\"silver\":" << q.reward.silver
          << ",\"copper\":" << q.reward.copper << "},\n";
        f << "    \"objectives\": [";
        for (size_t j = 0; j < q.objectives.size(); j++) {
            const auto& obj = q.objectives[j];
            f << "{\"type\":" << static_cast<int>(obj.type)
              << ",\"desc\":\"" << obj.description << "\""
              << ",\"target\":\"" << obj.targetName << "\""
              << ",\"count\":" << obj.targetCount << "}";
            if (j + 1 < q.objectives.size()) f << ",";
        }
        f << "]\n";
        f << "  }" << (i + 1 < quests_.size() ? "," : "") << "\n";
    }
    f << "]\n";

    LOG_INFO("Quests saved: ", path, " (", quests_.size(), " quests)");
    return true;
}

} // namespace editor
} // namespace wowee
