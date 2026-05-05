#include "editor_app.hpp"
#include "pipeline/wowee_model.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/custom_zone_discovery.hpp"
#include "core/logger.hpp"
#include <string>
#include <cstring>

static void printUsage(const char* argv0) {
    LOG_INFO("Usage: ", argv0, " --data <path> [options]");
    LOG_INFO("");
    LOG_INFO("Options:");
    LOG_INFO("  --data <path>          Path to extracted WoW data (manifest.json)");
    LOG_INFO("  --adt <map> <x> <y>    Load an ADT tile on startup");
    LOG_INFO("  --convert-m2 <path>    Convert M2 model to WOM open format (no GUI)");
    LOG_INFO("  --list-zones           List discovered custom zones and exit");
    LOG_INFO("  --version              Show version and format info");
    LOG_INFO("");
    LOG_INFO("Wowee World Editor v1.0.0 — by Kelsi Davis");
    LOG_INFO("Novel open formats: WOT/WHM/WOM/WOB/WCP");
}

int main(int argc, char* argv[]) {
    std::string dataPath;
    std::string adtMap;
    int adtX = -1, adtY = -1;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            dataPath = argv[++i];
        } else if (std::strcmp(argv[i], "--adt") == 0 && i + 3 < argc) {
            adtMap = argv[++i];
            adtX = std::atoi(argv[++i]);
            adtY = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--list-zones") == 0) {
            auto zones = wowee::pipeline::CustomZoneDiscovery::scan({"custom_zones", "output"});
            if (zones.empty()) {
                LOG_INFO("No custom zones found in custom_zones/ or output/");
            } else {
                LOG_INFO("Custom zones found:");
                for (const auto& z : zones) {
                    LOG_INFO("  ", z.name, " — ", z.directory,
                             z.hasCreatures ? " [NPCs]" : "",
                             z.hasQuests ? " [Quests]" : "");
                }
            }
            return 0;
        } else if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            LOG_INFO("Wowee World Editor v1.0.0");
            LOG_INFO("Open formats: WOT/WHM/WOM/WOB/WCP (all novel, no Blizzard IP)");
            LOG_INFO("By Kelsi Davis");
            return 0;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    // Batch convert mode: --convert <m2path> converts M2 to WOM
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--convert-m2") == 0 && i + 1 < argc) {
            std::string m2Path = argv[++i];
            LOG_INFO("Batch convert mode: M2→WOM for ", m2Path);
            // Need data path for asset loading
            if (dataPath.empty()) dataPath = "Data";
            wowee::pipeline::AssetManager am;
            if (am.initialize(dataPath)) {
                auto wom = wowee::pipeline::WoweeModelLoader::fromM2(m2Path, &am);
                if (wom.isValid()) {
                    std::string outPath = m2Path;
                    auto dot = outPath.rfind('.');
                    if (dot != std::string::npos) outPath = outPath.substr(0, dot);
                    wowee::pipeline::WoweeModelLoader::save(wom, "output/models/" + outPath);
                    LOG_INFO("Converted: ", m2Path, " → output/models/", outPath, ".wom");
                } else {
                    LOG_ERROR("Failed to convert: ", m2Path);
                }
                am.shutdown();
            }
            return 0;
        }
    }

    if (dataPath.empty()) {
        dataPath = "Data";
        LOG_INFO("No --data path specified, using default: ", dataPath);
    }

    wowee::editor::EditorApp app;
    if (!app.initialize(dataPath)) {
        LOG_ERROR("Failed to initialize editor");
        return 1;
    }

    if (!adtMap.empty()) {
        app.loadADT(adtMap, adtX, adtY);
    }

    app.run();
    app.shutdown();

    return 0;
}
