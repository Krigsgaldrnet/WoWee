#include "cli_world_map.hpp"

#include "pipeline/wowee_world_map.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWomxExt(std::string base) {
    if (base.size() >= 5 && base.substr(base.size() - 5) == ".womx")
        base = base.substr(0, base.size() - 5);
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeWorldMap& m,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeWorldMapLoader::save(m, base)) {
        std::fprintf(stderr, "%s: failed to save %s.womx\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeWorldMap& m,
                     const std::string& base) {
    std::printf("Wrote %s.womx\n", base.c_str());
    std::printf("  name        : %s\n", m.name.c_str());
    std::printf("  worldType   : %u (%s)\n", m.worldType,
                wowee::pipeline::WoweeWorldMap::worldTypeName(m.worldType));
    std::printf("  gridSize    : %ux%u tiles\n", m.gridSize, m.gridSize);
    std::printf("  tilesPresent: %u / %u\n",
                m.countTiles(),
                static_cast<uint32_t>(m.gridSize) * m.gridSize);
}

int handleGenContinent(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string mapName = "Continent";
    if (i + 1 < argc && argv[i + 1][0] != '-') mapName = argv[++i];
    base = stripWomxExt(base);
    auto m = wowee::pipeline::WoweeWorldMapLoader::makeContinent(mapName);
    if (!saveOrError(m, base, "gen-world-map")) return 1;
    printGenSummary(m, base);
    return 0;
}

int handleGenInstance(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string mapName = "Instance";
    if (i + 1 < argc && argv[i + 1][0] != '-') mapName = argv[++i];
    base = stripWomxExt(base);
    auto m = wowee::pipeline::WoweeWorldMapLoader::makeInstance(mapName);
    if (!saveOrError(m, base, "gen-world-map-instance")) return 1;
    printGenSummary(m, base);
    return 0;
}

int handleGenArena(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string mapName = "Arena";
    if (i + 1 < argc && argv[i + 1][0] != '-') mapName = argv[++i];
    base = stripWomxExt(base);
    auto m = wowee::pipeline::WoweeWorldMapLoader::makeArena(mapName);
    if (!saveOrError(m, base, "gen-world-map-arena")) return 1;
    printGenSummary(m, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = (i + 1 < argc &&
                    std::strcmp(argv[i + 1], "--json") == 0);
    if (jsonOut) i++;
    base = stripWomxExt(base);
    if (!wowee::pipeline::WoweeWorldMapLoader::exists(base)) {
        std::fprintf(stderr, "WOMX not found: %s.womx\n", base.c_str());
        return 1;
    }
    auto m = wowee::pipeline::WoweeWorldMapLoader::load(base);
    uint32_t total = static_cast<uint32_t>(m.gridSize) * m.gridSize;
    uint32_t present = m.countTiles();
    if (jsonOut) {
        nlohmann::json j;
        j["womx"] = base + ".womx";
        j["name"] = m.name;
        j["worldType"] = m.worldType;
        j["worldTypeName"] =
            wowee::pipeline::WoweeWorldMap::worldTypeName(m.worldType);
        j["gridSize"] = m.gridSize;
        j["totalTiles"] = total;
        j["tilesPresent"] = present;
        j["defaultLightId"] = m.defaultLightId;
        j["defaultWeatherId"] = m.defaultWeatherId;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WOMX: %s.womx\n", base.c_str());
    std::printf("  name             : %s\n", m.name.c_str());
    std::printf("  worldType        : %u (%s)\n", m.worldType,
                wowee::pipeline::WoweeWorldMap::worldTypeName(m.worldType));
    std::printf("  gridSize         : %ux%u (%u total tiles)\n",
                m.gridSize, m.gridSize, total);
    std::printf("  tilesPresent     : %u (%.1f%%)\n",
                present,
                total ? (100.0f * present / total) : 0.0f);
    std::printf("  defaultLightId   : %u\n", m.defaultLightId);
    std::printf("  defaultWeatherId : %u\n", m.defaultWeatherId);
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = (i + 1 < argc &&
                    std::strcmp(argv[i + 1], "--json") == 0);
    if (jsonOut) i++;
    base = stripWomxExt(base);
    if (!wowee::pipeline::WoweeWorldMapLoader::exists(base)) {
        std::fprintf(stderr, "validate-womx: WOMX not found: %s.womx\n",
                     base.c_str());
        return 1;
    }
    auto m = wowee::pipeline::WoweeWorldMapLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (m.gridSize == 0 || m.gridSize > 128) {
        errors.push_back("gridSize " + std::to_string(m.gridSize) +
                         " not in range 1..128");
    }
    if (m.worldType > wowee::pipeline::WoweeWorldMap::Arena) {
        errors.push_back("worldType " + std::to_string(m.worldType) +
                         " not in known range 0..3");
    }
    size_t expectedBytes =
        (static_cast<size_t>(m.gridSize) * m.gridSize + 7) / 8;
    if (m.tileBitmap.size() != expectedBytes) {
        errors.push_back("tileBitmap size " +
                         std::to_string(m.tileBitmap.size()) +
                         " != expected " + std::to_string(expectedBytes) +
                         " for grid " + std::to_string(m.gridSize) + "x" +
                         std::to_string(m.gridSize));
    }
    if (m.countTiles() == 0) {
        warnings.push_back("no tiles present in bitmap (empty world)");
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["womx"] = base + ".womx";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-womx: %s.womx\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %ux%u grid, %u/%u tiles present\n",
                    m.gridSize, m.gridSize,
                    m.countTiles(),
                    static_cast<uint32_t>(m.gridSize) * m.gridSize);
        return 0;
    }
    if (!warnings.empty()) {
        std::printf("  warnings (%zu):\n", warnings.size());
        for (const auto& w : warnings)
            std::printf("    - %s\n", w.c_str());
    }
    if (!errors.empty()) {
        std::printf("  ERRORS (%zu):\n", errors.size());
        for (const auto& e : errors)
            std::printf("    - %s\n", e.c_str());
    }
    return ok ? 0 : 1;
}

} // namespace

bool handleWorldMap(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-world-map") == 0 && i + 1 < argc) {
        outRc = handleGenContinent(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-world-map-instance") == 0 && i + 1 < argc) {
        outRc = handleGenInstance(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-world-map-arena") == 0 && i + 1 < argc) {
        outRc = handleGenArena(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-womx") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-womx") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
