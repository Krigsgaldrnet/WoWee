#include "editor_project.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <filesystem>

namespace wowee {
namespace editor {

bool EditorProject::save(const std::string& path) const {
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(path).parent_path());

    std::ofstream f(path);
    if (!f) return false;

    f << "{\n";
    f << "  \"format\": \"wowee-project-1.0\",\n";
    f << "  \"name\": \"" << name << "\",\n";
    f << "  \"author\": \"" << author << "\",\n";
    f << "  \"description\": \"" << description << "\",\n";
    f << "  \"version\": \"" << version << "\",\n";
    f << "  \"startMapId\": " << startMapId << ",\n";
    f << "  \"zones\": [\n";
    for (size_t i = 0; i < zones.size(); i++) {
        const auto& z = zones[i];
        f << "    {\"mapName\": \"" << z.mapName << "\""
          << ", \"tileX\": " << z.tileX
          << ", \"tileY\": " << z.tileY
          << ", \"biome\": \"" << z.biome << "\""
          << ", \"description\": \"" << z.description << "\""
          << "}" << (i + 1 < zones.size() ? "," : "") << "\n";
    }
    f << "  ]\n";
    f << "}\n";

    LOG_INFO("Project saved: ", path, " (", zones.size(), " zones)");
    return true;
}

bool EditorProject::load(const std::string& path) {
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

    name = findStr("name");
    author = findStr("author");
    description = findStr("description");
    version = findStr("version");
    projectDir = std::filesystem::path(path).parent_path().string();

    // Parse zones from JSON array
    zones.clear();
    size_t zonesPos = content.find("\"zones\"");
    if (zonesPos != std::string::npos) {
        size_t start = zonesPos;
        while ((start = content.find('{', start + 1)) != std::string::npos) {
            auto end = content.find('}', start);
            if (end == std::string::npos) break;
            // Check we're still inside the zones array
            auto closeBracket = content.find(']', zonesPos);
            if (start > closeBracket) break;

            std::string block = content.substr(start, end - start + 1);
            ProjectZone z;

            auto blockFindStr = [&](const std::string& key) -> std::string {
                auto p = block.find("\"" + key + "\"");
                if (p == std::string::npos) return "";
                p = block.find('"', block.find(':', p) + 1);
                if (p == std::string::npos) return "";
                auto e = block.find('"', p + 1);
                return block.substr(p + 1, e - p - 1);
            };

            z.mapName = blockFindStr("mapName");
            z.biome = blockFindStr("biome");
            z.description = blockFindStr("description");

            auto txPos = block.find("\"tileX\":");
            if (txPos != std::string::npos) z.tileX = std::stoi(block.substr(txPos + 8));
            auto tyPos = block.find("\"tileY\":");
            if (tyPos != std::string::npos) z.tileY = std::stoi(block.substr(tyPos + 8));

            if (!z.mapName.empty()) zones.push_back(z);
            start = end;
        }
    }

    LOG_INFO("Project loaded: ", path, " (", name, ", ", zones.size(), " zones)");
    return true;
}

std::string EditorProject::getZoneOutputDir(int zoneIdx) const {
    if (zoneIdx < 0 || zoneIdx >= static_cast<int>(zones.size())) return "";
    return projectDir + "/" + zones[zoneIdx].mapName;
}

} // namespace editor
} // namespace wowee
