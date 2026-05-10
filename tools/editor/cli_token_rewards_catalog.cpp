#include "cli_token_rewards_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_token_rewards.hpp"
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

std::string stripWtbrExt(std::string base) {
    stripExt(base, ".wtbr");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeTokenReward& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeTokenRewardLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wtbr\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeTokenReward& c,
                     const std::string& base) {
    std::printf("Wrote %s.wtbr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  rewards : %zu\n", c.entries.size());
}

int handleGenRaid(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RaidTokenRewards";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtbrExt(base);
    auto c = wowee::pipeline::WoweeTokenRewardLoader::makeRaidTokens(name);
    if (!saveOrError(c, base, "gen-tbr")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenPvP(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "PvPTokenRewards";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtbrExt(base);
    auto c = wowee::pipeline::WoweeTokenRewardLoader::makePvP(name);
    if (!saveOrError(c, base, "gen-tbr-pvp")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenFaction(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FactionTokenRewards";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtbrExt(base);
    auto c = wowee::pipeline::WoweeTokenRewardLoader::makeFaction(name);
    if (!saveOrError(c, base, "gen-tbr-faction")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWtbrExt(base);
    if (!wowee::pipeline::WoweeTokenRewardLoader::exists(base)) {
        std::fprintf(stderr, "WTBR not found: %s.wtbr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeTokenRewardLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wtbr"] = base + ".wtbr";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"tokenRewardId", e.tokenRewardId},
                {"name", e.name},
                {"description", e.description},
                {"spentTokenItemId", e.spentTokenItemId},
                {"spentTokenCount", e.spentTokenCount},
                {"rewardKind", e.rewardKind},
                {"rewardKindName", wowee::pipeline::WoweeTokenReward::rewardKindName(e.rewardKind)},
                {"rewardId", e.rewardId},
                {"rewardCount", e.rewardCount},
                {"requiredFactionId", e.requiredFactionId},
                {"requiredFactionStanding", e.requiredFactionStanding},
                {"requiredFactionStandingName", wowee::pipeline::WoweeTokenReward::factionStandingName(e.requiredFactionStanding)},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WTBR: %s.wtbr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  rewards : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    spent(item x count)   kind        rewardId   faction@standing      name\n");
    for (const auto& e : c.entries) {
        char gateBuf[64] = "-";
        if (e.requiredFactionId != 0) {
            std::snprintf(gateBuf, sizeof(gateBuf), "%u@%s",
                          e.requiredFactionId,
                          wowee::pipeline::WoweeTokenReward::factionStandingName(e.requiredFactionStanding));
        }
        std::printf("  %4u   %5u x %5u        %-9s   %5u      %-20s  %s\n",
                    e.tokenRewardId,
                    e.spentTokenItemId, e.spentTokenCount,
                    wowee::pipeline::WoweeTokenReward::rewardKindName(e.rewardKind),
                    e.rewardId, gateBuf,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWtbrExt(base);
    if (!wowee::pipeline::WoweeTokenRewardLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wtbr: WTBR not found: %s.wtbr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeTokenRewardLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.tokenRewardId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.tokenRewardId == 0)
            errors.push_back(ctx + ": tokenRewardId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.spentTokenItemId == 0)
            errors.push_back(ctx +
                ": spentTokenItemId is 0 — missing token currency");
        if (e.spentTokenCount == 0)
            errors.push_back(ctx +
                ": spentTokenCount is 0 — would grant reward for free");
        if (e.rewardKind > wowee::pipeline::WoweeTokenReward::Cosmetic) {
            errors.push_back(ctx + ": rewardKind " +
                std::to_string(e.rewardKind) + " not in 0..7");
        }
        if (e.requiredFactionStanding > wowee::pipeline::WoweeTokenReward::Exalted) {
            errors.push_back(ctx + ": requiredFactionStanding " +
                std::to_string(e.requiredFactionStanding) +
                " not in 0..7");
        }
        if (e.rewardId == 0)
            warnings.push_back(ctx +
                ": rewardId is 0 — no actual reward target, "
                "vendor will offer the entry but grant nothing");
        // requiredFactionStanding > Neutral with no
        // requiredFactionId is contradictory — the gate
        // can't apply.
        if (e.requiredFactionStanding > wowee::pipeline::WoweeTokenReward::Neutral &&
            e.requiredFactionId == 0) {
            warnings.push_back(ctx +
                ": requiredFactionStanding=" +
                wowee::pipeline::WoweeTokenReward::factionStandingName(e.requiredFactionStanding) +
                " set but requiredFactionId=0 — rep gate "
                "has no faction to check, gate will be ignored");
        }
        // Currency conversion to same item is suspicious
        // (1 X -> N X is usually a config bug).
        if (e.rewardKind == wowee::pipeline::WoweeTokenReward::Currency &&
            e.rewardId == e.spentTokenItemId) {
            warnings.push_back(ctx +
                ": Currency conversion from item " +
                std::to_string(e.spentTokenItemId) +
                " to itself — usually a typo");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.tokenRewardId) {
                errors.push_back(ctx + ": duplicate tokenRewardId");
                break;
            }
        }
        idsSeen.push_back(e.tokenRewardId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wtbr"] = base + ".wtbr";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wtbr: %s.wtbr\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu rewards, all tokenRewardIds unique\n",
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

bool handleTokenRewardsCatalog(int& i, int argc, char** argv,
                               int& outRc) {
    if (std::strcmp(argv[i], "--gen-tbr") == 0 && i + 1 < argc) {
        outRc = handleGenRaid(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-tbr-pvp") == 0 && i + 1 < argc) {
        outRc = handleGenPvP(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-tbr-faction") == 0 && i + 1 < argc) {
        outRc = handleGenFaction(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wtbr") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wtbr") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
