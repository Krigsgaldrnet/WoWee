#include "cli_pvp_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_pvp.hpp"
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

std::string stripWpvpExt(std::string base) {
    stripExt(base, ".wpvp");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweePVPRank& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweePVPRankLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wpvp\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweePVPRank& c,
                     const std::string& base) {
    std::printf("Wrote %s.wpvp\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  ranks   : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterPvPRanks";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpvpExt(base);
    auto c = wowee::pipeline::WoweePVPRankLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-pvp")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenAllianceFull(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AllianceVanillaRanks";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpvpExt(base);
    auto c = wowee::pipeline::WoweePVPRankLoader::makeAllianceFull(name);
    if (!saveOrError(c, base, "gen-pvp-alliance")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenArena(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ArenaTiers";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpvpExt(base);
    auto c = wowee::pipeline::WoweePVPRankLoader::makeArenaTiers(name);
    if (!saveOrError(c, base, "gen-pvp-arena")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWpvpExt(base);
    if (!wowee::pipeline::WoweePVPRankLoader::exists(base)) {
        std::fprintf(stderr, "WPVP not found: %s.wpvp\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweePVPRankLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wpvp"] = base + ".wpvp";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"rankId", e.rankId},
                {"name", e.name},
                {"factionAllianceName", e.factionAllianceName},
                {"factionHordeName", e.factionHordeName},
                {"description", e.description},
                {"rankKind", e.rankKind},
                {"rankKindName", wowee::pipeline::WoweePVPRank::rankKindName(e.rankKind)},
                {"minBracketLevel", e.minBracketLevel},
                {"maxBracketLevel", e.maxBracketLevel},
                {"minHonorOrRating", e.minHonorOrRating},
                {"rewardEmblems", e.rewardEmblems},
                {"titleId", e.titleId},
                {"chestItemId", e.chestItemId},
                {"glovesItemId", e.glovesItemId},
                {"shouldersItemId", e.shouldersItemId},
                {"bracketBgId", e.bracketBgId},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WPVP: %s.wpvp\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  ranks   : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind             threshold  emblem  title  chest  alliance / horde            \n");
    for (const auto& e : c.entries) {
        std::string both = e.factionAllianceName +
                           (e.factionAllianceName != e.factionHordeName
                              ? std::string(" / ") + e.factionHordeName
                              : std::string());
        std::printf("  %4u   %-15s  %8u   %3u    %3u   %5u  %s\n",
                    e.rankId,
                    wowee::pipeline::WoweePVPRank::rankKindName(e.rankKind),
                    e.minHonorOrRating, e.rewardEmblems,
                    e.titleId, e.chestItemId, both.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWpvpExt(base);
    if (!wowee::pipeline::WoweePVPRankLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wpvp: WPVP not found: %s.wpvp\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweePVPRankLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    // Track threshold monotonicity within a single rankKind —
    // arena ratings should ascend (1500 < 1750 < ...), so a
    // catalog with two arena entries at the same rating or
    // a higher-id entry below a lower-id entry is suspicious.
    uint32_t prevHonorByKind[5] = {0, 0, 0, 0, 0};
    bool prevHonorSeen[5] = {false, false, false, false, false};
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.rankId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.rankId == 0)
            errors.push_back(ctx + ": rankId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.rankKind > wowee::pipeline::WoweePVPRank::ConquestPoint) {
            errors.push_back(ctx + ": rankKind " +
                std::to_string(e.rankKind) + " not in 0..4");
        }
        if (e.minBracketLevel > e.maxBracketLevel) {
            errors.push_back(ctx + ": minBracketLevel " +
                std::to_string(e.minBracketLevel) +
                " > maxBracketLevel " +
                std::to_string(e.maxBracketLevel));
        }
        // Vanilla honor ranks must use VanillaHonor kind and
        // have minHonor > 0 (rank 1 is the implicit baseline).
        if (e.rankKind == wowee::pipeline::WoweePVPRank::VanillaHonor &&
            e.minHonorOrRating == 0) {
            warnings.push_back(ctx +
                ": VanillaHonor kind with minHonor=0 "
                "(rank 1 baseline — verify intentional)");
        }
        // Arena ratings below 1500 don't unlock any reward —
        // 1500 is the WoW arena floor.
        if (e.rankKind == wowee::pipeline::WoweePVPRank::ArenaRating &&
            e.minHonorOrRating < 1500) {
            warnings.push_back(ctx +
                ": ArenaRating with rating " +
                std::to_string(e.minHonorOrRating) +
                " below 1500 floor");
        }
        // Faction alternate names — vanilla ranks have
        // distinct alliance / horde names; arena tiers share
        // the same name on both factions. Either is valid; an
        // empty alliance name + non-empty horde (or vice
        // versa) is a typo signal.
        if (e.factionAllianceName.empty() !=
            e.factionHordeName.empty()) {
            warnings.push_back(ctx +
                ": only one faction-alternate name set "
                "(alliance='" + e.factionAllianceName +
                "', horde='" + e.factionHordeName + "')");
        }
        // Threshold monotonicity within rankKind.
        if (e.rankKind < 5) {
            if (prevHonorSeen[e.rankKind] &&
                e.minHonorOrRating < prevHonorByKind[e.rankKind]) {
                warnings.push_back(ctx +
                    ": threshold " +
                    std::to_string(e.minHonorOrRating) +
                    " below previous " +
                    std::to_string(prevHonorByKind[e.rankKind]) +
                    " in same rankKind (non-monotonic)");
            }
            prevHonorByKind[e.rankKind] = e.minHonorOrRating;
            prevHonorSeen[e.rankKind] = true;
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.rankId) {
                errors.push_back(ctx + ": duplicate rankId");
                break;
            }
        }
        idsSeen.push_back(e.rankId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wpvp"] = base + ".wpvp";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wpvp: %s.wpvp\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu ranks, all rankIds unique, all thresholds monotonic\n",
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

bool handlePVPCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-pvp") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-pvp-alliance") == 0 && i + 1 < argc) {
        outRc = handleGenAllianceFull(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-pvp-arena") == 0 && i + 1 < argc) {
        outRc = handleGenArena(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wpvp") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wpvp") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
