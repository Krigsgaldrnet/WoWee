#include "cli_player_spawn_profiles_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_player_spawn_profiles.hpp"
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

std::string stripWpspExt(std::string base) {
    stripExt(base, ".wpsp");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweePlayerSpawnProfile& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweePlayerSpawnProfileLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wpsp\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweePlayerSpawnProfile& c,
                     const std::string& base) {
    std::printf("Wrote %s.wpsp\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  profiles : %zu\n", c.entries.size());
}

int handleGenAlliance(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AllianceStartingProfiles";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpspExt(base);
    auto c = wowee::pipeline::WoweePlayerSpawnProfileLoader::makeAlliance(name);
    if (!saveOrError(c, base, "gen-psp")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenHorde(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "HordeStartingProfiles";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpspExt(base);
    auto c = wowee::pipeline::WoweePlayerSpawnProfileLoader::makeHorde(name);
    if (!saveOrError(c, base, "gen-psp-horde")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenDeathKnight(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "DeathKnightProfiles";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpspExt(base);
    auto c = wowee::pipeline::WoweePlayerSpawnProfileLoader::makeDeathKnight(name);
    if (!saveOrError(c, base, "gen-psp-dk")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWpspExt(base);
    if (!wowee::pipeline::WoweePlayerSpawnProfileLoader::exists(base)) {
        std::fprintf(stderr, "WPSP not found: %s.wpsp\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweePlayerSpawnProfileLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wpsp"] = base + ".wpsp";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"profileId", e.profileId},
                {"name", e.name},
                {"description", e.description},
                {"raceMask", e.raceMask},
                {"classMask", e.classMask},
                {"mapId", e.mapId},
                {"zoneId", e.zoneId},
                {"spawnX", e.spawnX},
                {"spawnY", e.spawnY},
                {"spawnZ", e.spawnZ},
                {"spawnFacing", e.spawnFacing},
                {"bindMapId", e.bindMapId},
                {"bindZoneId", e.bindZoneId},
                {"startingItem1Id", e.startingItem1Id},
                {"startingItem1Count", e.startingItem1Count},
                {"startingItem2Id", e.startingItem2Id},
                {"startingItem2Count", e.startingItem2Count},
                {"startingItem3Id", e.startingItem3Id},
                {"startingItem3Count", e.startingItem3Count},
                {"startingItem4Id", e.startingItem4Id},
                {"startingItem4Count", e.startingItem4Count},
                {"startingSpell1Id", e.startingSpell1Id},
                {"startingSpell2Id", e.startingSpell2Id},
                {"startingSpell3Id", e.startingSpell3Id},
                {"startingSpell4Id", e.startingSpell4Id},
                {"startingLevel", e.startingLevel},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WPSP: %s.wpsp\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  profiles : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   raceMask    classMask   map  zone  lvl   spawn (x,y,z)              name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   0x%08x  0x%08x  %4u  %4u  %3u   (%8.1f,%8.1f,%6.1f)  %s\n",
                    e.profileId,
                    e.raceMask, e.classMask,
                    e.mapId, e.zoneId,
                    e.startingLevel,
                    e.spawnX, e.spawnY, e.spawnZ,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWpspExt(base);
    if (!wowee::pipeline::WoweePlayerSpawnProfileLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wpsp: WPSP not found: %s.wpsp\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweePlayerSpawnProfileLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.profileId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.profileId == 0)
            errors.push_back(ctx + ": profileId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.raceMask == 0)
            errors.push_back(ctx +
                ": raceMask is 0 — no race can spawn here");
        if (e.classMask == 0)
            errors.push_back(ctx +
                ": classMask is 0 — no class can spawn here");
        if (e.startingLevel == 0)
            errors.push_back(ctx + ": startingLevel is 0");
        if (e.startingLevel > 80)
            warnings.push_back(ctx +
                ": startingLevel " +
                std::to_string(e.startingLevel) +
                " > 80 — character will spawn above WotLK level cap");
        // (0,0,0) spawn position is suspicious — usually a
        // forgotten coord on a hand-edited entry.
        if (e.spawnX == 0.0f && e.spawnY == 0.0f && e.spawnZ == 0.0f) {
            warnings.push_back(ctx +
                ": spawn position is (0,0,0) — likely an "
                "uninitialized entry");
        }
        // Item count without item id (or vice versa) is a
        // misconfiguration.
        auto checkItem = [&](uint32_t id, uint32_t count, int slot) {
            if (id == 0 && count != 0) {
                warnings.push_back(ctx +
                    ": startingItem" + std::to_string(slot) +
                    " has count=" + std::to_string(count) +
                    " but id=0 — item will not be granted");
            } else if (id != 0 && count == 0) {
                warnings.push_back(ctx +
                    ": startingItem" + std::to_string(slot) +
                    " has id=" + std::to_string(id) +
                    " but count=0 — item will not be granted");
            }
        };
        checkItem(e.startingItem1Id, e.startingItem1Count, 1);
        checkItem(e.startingItem2Id, e.startingItem2Count, 2);
        checkItem(e.startingItem3Id, e.startingItem3Count, 3);
        checkItem(e.startingItem4Id, e.startingItem4Count, 4);
        // DK spawning at lvl < 55 is misconfigured (DKs
        // always start at 55).
        constexpr uint32_t CLS_DK_BIT = 1u << 5;
        if ((e.classMask & CLS_DK_BIT) && e.startingLevel < 55) {
            warnings.push_back(ctx +
                ": Death Knight class with startingLevel=" +
                std::to_string(e.startingLevel) +
                " — DKs canonically start at level 55");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.profileId) {
                errors.push_back(ctx + ": duplicate profileId");
                break;
            }
        }
        idsSeen.push_back(e.profileId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wpsp"] = base + ".wpsp";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wpsp: %s.wpsp\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu profiles, all profileIds unique, all masks set\n",
                    c.entries.size());
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

bool handlePlayerSpawnProfilesCatalog(int& i, int argc, char** argv,
                                      int& outRc) {
    if (std::strcmp(argv[i], "--gen-psp") == 0 && i + 1 < argc) {
        outRc = handleGenAlliance(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-psp-horde") == 0 && i + 1 < argc) {
        outRc = handleGenHorde(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-psp-dk") == 0 && i + 1 < argc) {
        outRc = handleGenDeathKnight(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wpsp") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wpsp") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
