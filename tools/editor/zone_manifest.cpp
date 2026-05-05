#include "zone_manifest.hpp"
#include "core/logger.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <ctime>

namespace wowee {
namespace editor {

bool ZoneManifest::save(const std::string& path) const {
    auto dir = std::filesystem::path(path).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir);

    nlohmann::json j;
    j["mapName"] = mapName;
    j["displayName"] = displayName;
    j["mapId"] = mapId;
    j["biome"] = biome;
    j["baseHeight"] = baseHeight;
    j["hasCreatures"] = hasCreatures;
    j["description"] = description;
    j["editorVersion"] = "1.0.0";

    {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        char timeBuf[32];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", std::localtime(&time));
        j["exportTime"] = timeBuf;
    }

    nlohmann::json tilesArr = nlohmann::json::array();
    for (const auto& t : tiles) tilesArr.push_back({t.first, t.second});
    j["tiles"] = tilesArr;

    nlohmann::json files;
    files["wdt"] = mapName + ".wdt";
    for (const auto& t : tiles) {
        std::string key = "adt_" + std::to_string(t.first) + "_" + std::to_string(t.second);
        files[key] = mapName + "_" + std::to_string(t.first) + "_" + std::to_string(t.second) + ".adt";
    }
    if (hasCreatures) files["creatures"] = "creatures.json";
    j["files"] = files;

    // Zone gameplay flags
    nlohmann::json flags;
    flags["allowFlying"] = allowFlying;
    flags["pvpEnabled"] = pvpEnabled;
    flags["isIndoor"] = isIndoor;
    flags["isSanctuary"] = isSanctuary;
    j["flags"] = flags;

    // Audio configuration
    if (!musicTrack.empty() || !ambienceDay.empty()) {
        nlohmann::json audio;
        if (!musicTrack.empty()) audio["music"] = musicTrack;
        if (!ambienceDay.empty()) audio["ambienceDay"] = ambienceDay;
        if (!ambienceNight.empty()) audio["ambienceNight"] = ambienceNight;
        audio["musicVolume"] = musicVolume;
        audio["ambienceVolume"] = ambienceVolume;
        j["audio"] = audio;
    }

    std::ofstream f(path);
    if (!f) { LOG_ERROR("Failed to write zone manifest: ", path); return false; }
    f << j.dump(2) << "\n";

    LOG_INFO("Zone manifest saved: ", path);
    return true;
}

bool ZoneManifest::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    try {
        auto j = nlohmann::json::parse(f);

        mapName = j.value("mapName", "");
        if (mapName.empty()) mapName = j.value("name", "");
        displayName = j.value("displayName", mapName);
        biome = j.value("biome", "");
        description = j.value("description", "");
        mapId = j.value("mapId", 9000u);
        baseHeight = j.value("baseHeight", 100.0f);
        hasCreatures = j.value("hasCreatures", false);

        tiles.clear();
        if (j.contains("tiles") && j["tiles"].is_array()) {
            for (const auto& t : j["tiles"]) {
                if (t.is_array() && t.size() >= 2)
                    tiles.push_back({t[0].get<int>(), t[1].get<int>()});
            }
        }

        // Zone gameplay flags
        if (j.contains("flags")) {
            const auto& fl = j["flags"];
            allowFlying = fl.value("allowFlying", false);
            pvpEnabled = fl.value("pvpEnabled", false);
            isIndoor = fl.value("isIndoor", false);
            isSanctuary = fl.value("isSanctuary", false);
        }

        // Audio configuration
        if (j.contains("audio")) {
            const auto& a = j["audio"];
            musicTrack = a.value("music", "");
            ambienceDay = a.value("ambienceDay", "");
            ambienceNight = a.value("ambienceNight", "");
            musicVolume = a.value("musicVolume", 0.7f);
            ambienceVolume = a.value("ambienceVolume", 0.5f);
        }

        return !mapName.empty();
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse zone manifest: ", e.what());
        return false;
    }
}

} // namespace editor
} // namespace wowee
