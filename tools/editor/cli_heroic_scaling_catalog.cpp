#include "cli_heroic_scaling_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_heroic_scaling.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWhrdExt(std::string base) {
    stripExt(base, ".whrd");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeHeroicScaling& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeHeroicScalingLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.whrd\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeHeroicScaling& c,
                     const std::string& base) {
    std::printf("Wrote %s.whrd\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  scalings : %zu\n", c.entries.size());
}

int handleGen5man(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WotLK5manHeroicScaling";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWhrdExt(base);
    auto c = wowee::pipeline::WoweeHeroicScalingLoader::makeWotLK5manHeroic(name);
    if (!saveOrError(c, base, "gen-hrd")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRaid25(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "Raid25HeroicScaling";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWhrdExt(base);
    auto c = wowee::pipeline::WoweeHeroicScalingLoader::makeRaid25Heroic(name);
    if (!saveOrError(c, base, "gen-hrd-raid25")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenChallenge(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ChallengeModeScaling";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWhrdExt(base);
    auto c = wowee::pipeline::WoweeHeroicScalingLoader::makeChallengeMode(name);
    if (!saveOrError(c, base, "gen-hrd-cm")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWhrdExt(base);
    if (!wowee::pipeline::WoweeHeroicScalingLoader::exists(base)) {
        std::fprintf(stderr, "WHRD not found: %s.whrd\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeHeroicScalingLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["whrd"] = base + ".whrd";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"scalingId", e.scalingId},
                {"name", e.name},
                {"description", e.description},
                {"mapId", e.mapId},
                {"difficultyId", e.difficultyId},
                {"itemLevelDelta", e.itemLevelDelta},
                {"bonusQualityChance", e.bonusQualityChance},
                {"bonusQualityPct",
                    e.bonusQualityChance / 100.0},
                {"dropChanceMultiplier", e.dropChanceMultiplier},
                {"heroicTokenItemId", e.heroicTokenItemId},
                {"bonusEmblemCount", e.bonusEmblemCount},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WHRD: %s.whrd\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  scalings : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   map   diff   ilvl  bonusQ%%  dropMult  token   emblems  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %4u   %4u   %+4d    %5.2f    %5.2fx  %6u   %3u    %s\n",
                    e.scalingId, e.mapId, e.difficultyId,
                    e.itemLevelDelta,
                    e.bonusQualityChance / 100.0,
                    e.dropChanceMultiplier,
                    e.heroicTokenItemId,
                    e.bonusEmblemCount, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWhrdExt(base);
    if (!wowee::pipeline::WoweeHeroicScalingLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-whrd: WHRD not found: %s.whrd\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeHeroicScalingLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    // (mapId, difficultyId) tuple uniqueness — two
    // scalings binding the same instance+difficulty
    // would make the loot-roll lookup ambiguous.
    std::set<uint64_t> instanceComboSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.scalingId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.scalingId == 0)
            errors.push_back(ctx + ": scalingId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.difficultyId == 0) {
            errors.push_back(ctx +
                ": difficultyId is 0 — Heroic scaling "
                "must specify a non-default difficulty "
                "(Normal mode is difficultyId=0 by "
                "convention)");
        }
        // itemLevelDelta sanity. 5-man Heroic typically
        // +13, raid Heroic +13 to +26. Negative is
        // unusual (Heroic shouldn't be WORSE than Normal).
        if (e.itemLevelDelta < 0) {
            warnings.push_back(ctx +
                ": itemLevelDelta " +
                std::to_string(e.itemLevelDelta) +
                " < 0 — Heroic loot is worse than Normal? "
                "Verify if intentional");
        }
        if (e.itemLevelDelta > 50) {
            warnings.push_back(ctx +
                ": itemLevelDelta " +
                std::to_string(e.itemLevelDelta) +
                " > 50 — exceeds typical Heroic-scaling "
                "delta range (max canonical is +26 for "
                "raid Heroic)");
        }
        if (e.bonusQualityChance > 10000) {
            errors.push_back(ctx +
                ": bonusQualityChance " +
                std::to_string(e.bonusQualityChance) +
                " > 10000 (basis points cap) — would "
                "guarantee multiple bonus drops");
        }
        if (e.dropChanceMultiplier <= 0.0f) {
            errors.push_back(ctx +
                ": dropChanceMultiplier <= 0 — would "
                "block all loot drops on Heroic");
        }
        if (e.dropChanceMultiplier > 10.0f) {
            warnings.push_back(ctx +
                ": dropChanceMultiplier " +
                std::to_string(e.dropChanceMultiplier) +
                " > 10x — extreme drop boost; verify if "
                "intentional");
        }
        // (mapId, difficultyId) uniqueness — but mapId=0
        // is the wildcard (any map at the given
        // difficulty), which is allowed multiple times.
        if (e.mapId != 0) {
            uint64_t key = (static_cast<uint64_t>(e.mapId) << 32)
                          | e.difficultyId;
            if (!instanceComboSeen.insert(key).second) {
                errors.push_back(ctx +
                    ": (mapId=" + std::to_string(e.mapId) +
                    ", difficultyId=" +
                    std::to_string(e.difficultyId) +
                    ") combo already bound by another "
                    "scaling — loot-roll lookup would be "
                    "ambiguous");
            }
        }
        if (!idsSeen.insert(e.scalingId).second) {
            errors.push_back(ctx + ": duplicate scalingId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["whrd"] = base + ".whrd";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-whrd: %s.whrd\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu scalings, all scalingIds + "
                    "(map,difficulty) tuples unique\n",
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

bool handleHeroicScalingCatalog(int& i, int argc, char** argv,
                                 int& outRc) {
    if (std::strcmp(argv[i], "--gen-hrd") == 0 && i + 1 < argc) {
        outRc = handleGen5man(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-hrd-raid25") == 0 &&
        i + 1 < argc) {
        outRc = handleGenRaid25(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-hrd-cm") == 0 && i + 1 < argc) {
        outRc = handleGenChallenge(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-whrd") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-whrd") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
