#include "pipeline/custom_zone_discovery.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <filesystem>

namespace wowee {
namespace pipeline {

std::vector<CustomZoneInfo> CustomZoneDiscovery::scan(const std::vector<std::string>& searchPaths) {
    std::vector<CustomZoneInfo> results;
    for (const auto& path : searchPaths) {
        auto found = scanDirectory(path);
        results.insert(results.end(), found.begin(), found.end());
    }
    return results;
}

std::vector<CustomZoneInfo> CustomZoneDiscovery::scanDirectory(const std::string& path) {
    namespace fs = std::filesystem;
    std::vector<CustomZoneInfo> results;

    if (!fs::exists(path)) return results;

    for (auto& entry : fs::directory_iterator(path)) {
        if (!entry.is_directory()) continue;

        std::string zoneJson = entry.path().string() + "/zone.json";
        if (!fs::exists(zoneJson)) continue;

        std::ifstream f(zoneJson);
        if (!f) continue;
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        auto findStr = [&](const std::string& key) -> std::string {
            auto pos = content.find("\"" + key + "\"");
            if (pos == std::string::npos) return "";
            pos = content.find('"', content.find(':', pos) + 1);
            if (pos == std::string::npos) return "";
            auto end = content.find('"', pos + 1);
            return content.substr(pos + 1, end - pos - 1);
        };

        CustomZoneInfo info;
        info.name = findStr("mapName");
        if (info.name.empty()) info.name = findStr("name");
        info.author = findStr("author");
        info.description = findStr("description");
        info.directory = entry.path().string();
        info.hasCreatures = fs::exists(entry.path().string() + "/creatures.json");
        info.hasQuests = fs::exists(entry.path().string() + "/quests.json");

        if (!info.name.empty()) {
            results.push_back(info);
            LOG_INFO("Discovered custom zone: ", info.name, " in ", info.directory);
        }
    }

    return results;
}

} // namespace pipeline
} // namespace wowee
