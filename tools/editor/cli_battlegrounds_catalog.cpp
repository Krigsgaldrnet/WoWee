#include "cli_battlegrounds_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_battlegrounds.hpp"
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

std::string stripWbgdExt(std::string base) {
    stripExt(base, ".wbgd");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeBattleground& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeBattlegroundLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wbgd\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeBattleground& c,
                     const std::string& base) {
    std::printf("Wrote %s.wbgd\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  bgs     : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterBgs";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbgdExt(base);
    auto c = wowee::pipeline::WoweeBattlegroundLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-bg")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenClassic(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ClassicBgs";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbgdExt(base);
    auto c = wowee::pipeline::WoweeBattlegroundLoader::makeClassic(name);
    if (!saveOrError(c, base, "gen-bg-classic")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenArena(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ArenaSet";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbgdExt(base);
    auto c = wowee::pipeline::WoweeBattlegroundLoader::makeArena(name);
    if (!saveOrError(c, base, "gen-bg-arena")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWbgdExt(base);
    if (!wowee::pipeline::WoweeBattlegroundLoader::exists(base)) {
        std::fprintf(stderr, "WBGD not found: %s.wbgd\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeBattlegroundLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wbgd"] = base + ".wbgd";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"battlegroundId", e.battlegroundId},
                {"mapId", e.mapId},
                {"name", e.name},
                {"description", e.description},
                {"objectiveKind", e.objectiveKind},
                {"objectiveKindName", wowee::pipeline::WoweeBattleground::objectiveKindName(e.objectiveKind)},
                {"minPlayersPerSide", e.minPlayersPerSide},
                {"maxPlayersPerSide", e.maxPlayersPerSide},
                {"minLevel", e.minLevel},
                {"maxLevel", e.maxLevel},
                {"scoreToWin", e.scoreToWin},
                {"timeLimitSeconds", e.timeLimitSeconds},
                {"bracketSize", e.bracketSize},
                {"allianceStart", {e.allianceStart.x, e.allianceStart.y, e.allianceStart.z}},
                {"allianceFacing", e.allianceFacing},
                {"hordeStart", {e.hordeStart.x, e.hordeStart.y, e.hordeStart.z}},
                {"hordeFacing", e.hordeFacing},
                {"respawnTimeSeconds", e.respawnTimeSeconds},
                {"markTokenId", e.markTokenId},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WBGD: %s.wbgd\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  bgs     : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    for (const auto& e : c.entries) {
        std::printf("\n  bgId=%u  map=%u  objective=%s  vs%uv%u  level=%u-%u\n",
                    e.battlegroundId, e.mapId,
                    wowee::pipeline::WoweeBattleground::objectiveKindName(e.objectiveKind),
                    e.minPlayersPerSide, e.maxPlayersPerSide,
                    e.minLevel, e.maxLevel);
        std::printf("    name      : %s\n", e.name.c_str());
        std::printf("    score     : %u to win  /  %us time limit\n",
                    e.scoreToWin, e.timeLimitSeconds);
        std::printf("    respawn   : %us  bracket: %u levels  markToken: %u\n",
                    e.respawnTimeSeconds, e.bracketSize, e.markTokenId);
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWbgdExt(base);
    if (!wowee::pipeline::WoweeBattlegroundLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wbgd: WBGD not found: %s.wbgd\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeBattlegroundLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (bgId=" + std::to_string(e.battlegroundId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.battlegroundId == 0) {
            errors.push_back(ctx + ": battlegroundId is 0");
        }
        if (e.name.empty()) errors.push_back(ctx + ": name is empty");
        if (e.objectiveKind > wowee::pipeline::WoweeBattleground::CarryObject) {
            errors.push_back(ctx + ": objectiveKind " +
                std::to_string(e.objectiveKind) + " not in 0..5");
        }
        if (e.minPlayersPerSide == 0 || e.maxPlayersPerSide == 0) {
            errors.push_back(ctx + ": player count is 0");
        }
        if (e.minPlayersPerSide > e.maxPlayersPerSide) {
            errors.push_back(ctx +
                ": minPlayersPerSide > maxPlayersPerSide");
        }
        if (e.minLevel > e.maxLevel) {
            errors.push_back(ctx + ": minLevel > maxLevel");
        }
        if (e.scoreToWin == 0) {
            errors.push_back(ctx +
                ": scoreToWin is 0 (no win condition)");
        }
        // Annihilation BGs typically have respawnTimeSeconds=0
        // (no respawn until match ends). Other kinds need
        // respawn > 0 or the losing side can't recover.
        if (e.objectiveKind != wowee::pipeline::WoweeBattleground::Annihilation &&
            e.respawnTimeSeconds == 0) {
            warnings.push_back(ctx +
                ": non-annihilation BG with respawnTimeSeconds=0 "
                "(losing side cannot recover)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.battlegroundId) {
                errors.push_back(ctx + ": duplicate battlegroundId");
                break;
            }
        }
        idsSeen.push_back(e.battlegroundId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wbgd"] = base + ".wbgd";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wbgd: %s.wbgd\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu battlegrounds, all bgIds unique\n",
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

bool handleBattlegroundsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-bg") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-bg-classic") == 0 && i + 1 < argc) {
        outRc = handleGenClassic(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-bg-arena") == 0 && i + 1 < argc) {
        outRc = handleGenArena(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wbgd") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wbgd") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
