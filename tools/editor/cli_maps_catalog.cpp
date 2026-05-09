#include "cli_maps_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_maps.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWmsExt(std::string base) {
    stripExt(base, ".wms");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeMaps& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeMapsLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wms\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeMaps& c,
                     const std::string& base) {
    std::printf("Wrote %s.wms\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  maps    : %zu  areas : %zu\n",
                c.maps.size(), c.areas.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterMaps";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmsExt(base);
    auto c = wowee::pipeline::WoweeMapsLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-maps")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenClassic(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ClassicMaps";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmsExt(base);
    auto c = wowee::pipeline::WoweeMapsLoader::makeClassic(name);
    if (!saveOrError(c, base, "gen-maps-classic")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBgArena(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BgArenaMaps";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmsExt(base);
    auto c = wowee::pipeline::WoweeMapsLoader::makeBgArena(name);
    if (!saveOrError(c, base, "gen-maps-bgarena")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWmsExt(base);
    if (!wowee::pipeline::WoweeMapsLoader::exists(base)) {
        std::fprintf(stderr, "WMS not found: %s.wms\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeMapsLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wms"] = base + ".wms";
        j["name"] = c.name;
        j["mapCount"] = c.maps.size();
        j["areaCount"] = c.areas.size();
        nlohmann::json ma = nlohmann::json::array();
        for (const auto& m : c.maps) {
            ma.push_back({
                {"mapId", m.mapId},
                {"name", m.name},
                {"shortName", m.shortName},
                {"mapType", m.mapType},
                {"mapTypeName", wowee::pipeline::WoweeMaps::mapTypeName(m.mapType)},
                {"expansionId", m.expansionId},
                {"expansionName", wowee::pipeline::WoweeMaps::expansionName(m.expansionId)},
                {"maxPlayers", m.maxPlayers},
            });
        }
        j["maps"] = ma;
        nlohmann::json aa = nlohmann::json::array();
        for (const auto& a : c.areas) {
            aa.push_back({
                {"areaId", a.areaId},
                {"mapId", a.mapId},
                {"parentAreaId", a.parentAreaId},
                {"name", a.name},
                {"minLevel", a.minLevel},
                {"maxLevel", a.maxLevel},
                {"factionGroup", a.factionGroup},
                {"factionGroupName", wowee::pipeline::WoweeMaps::factionGroupName(a.factionGroup)},
                {"explorationXP", a.explorationXP},
                {"ambienceSoundId", a.ambienceSoundId},
            });
        }
        j["areas"] = aa;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WMS: %s.wms\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  maps    : %zu  areas : %zu\n",
                c.maps.size(), c.areas.size());
    if (!c.maps.empty()) {
        std::printf("\n  Maps:\n");
        std::printf("    id    type          expansion  max  short  name\n");
        for (const auto& m : c.maps) {
            std::printf("  %4u  %-12s  %-9s  %3u  %-5s  %s\n",
                        m.mapId,
                        wowee::pipeline::WoweeMaps::mapTypeName(m.mapType),
                        wowee::pipeline::WoweeMaps::expansionName(m.expansionId),
                        m.maxPlayers, m.shortName.c_str(),
                        m.name.c_str());
        }
    }
    if (!c.areas.empty()) {
        std::printf("\n  Areas:\n");
        std::printf("    id     map  parent  level    faction      xp    sound  name\n");
        for (const auto& a : c.areas) {
            std::printf("  %5u   %3u  %5u   %2u-%-2u  %-10s  %4u   %4u   %s\n",
                        a.areaId, a.mapId, a.parentAreaId,
                        a.minLevel, a.maxLevel,
                        wowee::pipeline::WoweeMaps::factionGroupName(a.factionGroup),
                        a.explorationXP, a.ambienceSoundId,
                        a.name.c_str());
        }
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWmsExt(base);
    if (!wowee::pipeline::WoweeMapsLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wms: WMS not found: %s.wms\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeMapsLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.maps.empty()) {
        warnings.push_back("catalog has zero maps");
    }
    std::vector<uint32_t> mapIdsSeen;
    for (size_t k = 0; k < c.maps.size(); ++k) {
        const auto& m = c.maps[k];
        std::string ctx = "map " + std::to_string(k) +
                          " (id=" + std::to_string(m.mapId);
        if (!m.name.empty()) ctx += " " + m.name;
        ctx += ")";
        if (m.name.empty()) {
            errors.push_back(ctx + ": name is empty");
        }
        if (m.mapType > wowee::pipeline::WoweeMaps::Arena) {
            errors.push_back(ctx + ": mapType " +
                std::to_string(m.mapType) + " not in 0..4");
        }
        if (m.expansionId > wowee::pipeline::WoweeMaps::Mop) {
            errors.push_back(ctx + ": expansionId " +
                std::to_string(m.expansionId) + " not in 0..4");
        }
        // Battleground / Arena need a player cap; continent/instance
        // can leave it 0 (unlimited / set by instance template).
        if ((m.mapType == wowee::pipeline::WoweeMaps::Battleground ||
             m.mapType == wowee::pipeline::WoweeMaps::Arena) &&
            m.maxPlayers == 0) {
            warnings.push_back(ctx +
                ": Battleground/Arena with maxPlayers=0 (no participant cap)");
        }
        for (uint32_t prev : mapIdsSeen) {
            if (prev == m.mapId) {
                errors.push_back(ctx + ": duplicate mapId");
                break;
            }
        }
        mapIdsSeen.push_back(m.mapId);
    }
    std::vector<uint32_t> areaIdsSeen;
    for (size_t k = 0; k < c.areas.size(); ++k) {
        const auto& a = c.areas[k];
        std::string ctx = "area " + std::to_string(k) +
                          " (id=" + std::to_string(a.areaId);
        if (!a.name.empty()) ctx += " " + a.name;
        ctx += ")";
        if (a.areaId == 0) {
            errors.push_back(ctx + ": areaId is 0");
        }
        if (a.name.empty()) {
            errors.push_back(ctx + ": name is empty");
        }
        if (a.maxLevel < a.minLevel) {
            errors.push_back(ctx + ": maxLevel < minLevel");
        }
        if (a.factionGroup > wowee::pipeline::WoweeMaps::FactionContested) {
            errors.push_back(ctx + ": factionGroup " +
                std::to_string(a.factionGroup) + " not in 0..3");
        }
        // Area must reference a real map.
        if (!c.findMap(a.mapId)) {
            errors.push_back(ctx + ": mapId " +
                std::to_string(a.mapId) +
                " does not exist in this catalog");
        }
        // parentAreaId (if non-zero) must reference a real area
        // and must be on the same map.
        if (a.parentAreaId != 0) {
            const auto* parent = c.findArea(a.parentAreaId);
            if (!parent) {
                errors.push_back(ctx + ": parentAreaId " +
                    std::to_string(a.parentAreaId) +
                    " does not exist");
            } else if (parent->mapId != a.mapId) {
                errors.push_back(ctx + ": parent area " +
                    std::to_string(a.parentAreaId) +
                    " is on a different map");
            } else if (a.parentAreaId == a.areaId) {
                errors.push_back(ctx + ": area lists itself as parent");
            }
        }
        for (uint32_t prev : areaIdsSeen) {
            if (prev == a.areaId) {
                errors.push_back(ctx + ": duplicate areaId");
                break;
            }
        }
        areaIdsSeen.push_back(a.areaId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wms"] = base + ".wms";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wms: %s.wms\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu maps, %zu areas, all IDs unique\n",
                    c.maps.size(), c.areas.size());
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

bool handleMapsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-maps") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-maps-classic") == 0 && i + 1 < argc) {
        outRc = handleGenClassic(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-maps-bgarena") == 0 && i + 1 < argc) {
        outRc = handleGenBgArena(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wms") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wms") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
