#include "zone_manifest.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <filesystem>
#include <chrono>
#include <ctime>

namespace wowee {
namespace editor {

bool ZoneManifest::save(const std::string& path) const {
    auto dir = std::filesystem::path(path).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir);

    std::ofstream f(path);
    if (!f) { LOG_ERROR("Failed to write zone manifest: ", path); return false; }

    f << "{\n";
    f << "  \"mapName\": \"" << mapName << "\",\n";
    f << "  \"displayName\": \"" << displayName << "\",\n";
    f << "  \"mapId\": " << mapId << ",\n";
    f << "  \"biome\": \"" << biome << "\",\n";
    f << "  \"baseHeight\": " << baseHeight << ",\n";
    f << "  \"hasCreatures\": " << (hasCreatures ? "true" : "false") << ",\n";
    f << "  \"description\": \"" << description << "\",\n";
    f << "  \"editorVersion\": \"0.9.0\",\n";
    // Add export timestamp
    {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        char timeBuf[32];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", std::localtime(&time));
        f << "  \"exportTime\": \"" << timeBuf << "\",\n";
    }
    f << "  \"tiles\": [";
    for (size_t i = 0; i < tiles.size(); i++) {
        f << "[" << tiles[i].first << "," << tiles[i].second << "]";
        if (i + 1 < tiles.size()) f << ",";
    }
    f << "],\n";
    f << "  \"files\": {\n";
    f << "    \"wdt\": \"" << mapName << ".wdt\",\n";
    for (size_t i = 0; i < tiles.size(); i++) {
        f << "    \"adt_" << tiles[i].first << "_" << tiles[i].second << "\": \""
          << mapName << "_" << tiles[i].first << "_" << tiles[i].second << ".adt\"";
        if (i + 1 < tiles.size() || hasCreatures) f << ",";
        f << "\n";
    }
    if (hasCreatures)
        f << "    \"creatures\": \"creatures.json\"\n";
    f << "  }\n";
    f << "}\n";

    LOG_INFO("Zone manifest saved: ", path);
    return true;
}

bool ZoneManifest::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
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

    mapName = findStr("mapName");
    displayName = findStr("displayName");
    biome = findStr("biome");
    description = findStr("description");

    auto numPos = content.find("\"mapId\"");
    if (numPos != std::string::npos) {
        numPos = content.find(':', numPos);
        mapId = static_cast<uint32_t>(std::stoi(content.substr(numPos + 1)));
    }

    return !mapName.empty();
}

} // namespace editor
} // namespace wowee
