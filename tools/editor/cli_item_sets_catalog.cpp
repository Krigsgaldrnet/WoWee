#include "cli_item_sets_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_item_sets.hpp"
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

std::string stripWsetExt(std::string base) {
    stripExt(base, ".wset");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeItemSet& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeItemSetLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wset\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeItemSet& c,
                     const std::string& base) {
    std::printf("Wrote %s.wset\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  sets    : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterItemSets";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsetExt(base);
    auto c = wowee::pipeline::WoweeItemSetLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-itset")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenTier(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "TierItemSets";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsetExt(base);
    auto c = wowee::pipeline::WoweeItemSetLoader::makeTier(name);
    if (!saveOrError(c, base, "gen-itset-tier")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenPvP(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "PvPItemSets";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsetExt(base);
    auto c = wowee::pipeline::WoweeItemSetLoader::makePvP(name);
    if (!saveOrError(c, base, "gen-itset-pvp")) return 1;
    printGenSummary(c, base);
    return 0;
}

void appendEntryJson(nlohmann::json& arr,
                     const wowee::pipeline::WoweeItemSet::Entry& e) {
    nlohmann::json items = nlohmann::json::array();
    for (size_t k = 0; k < e.pieceCount &&
         k < wowee::pipeline::WoweeItemSet::kMaxPieces; ++k) {
        items.push_back(e.itemIds[k]);
    }
    nlohmann::json bonuses = nlohmann::json::array();
    for (size_t k = 0; k < e.bonusCount &&
         k < wowee::pipeline::WoweeItemSet::kMaxBonuses; ++k) {
        bonuses.push_back({
            {"threshold", e.bonusThresholds[k]},
            {"spellId", e.bonusSpellIds[k]},
        });
    }
    arr.push_back({
        {"setId", e.setId},
        {"name", e.name},
        {"description", e.description},
        {"pieceCount", e.pieceCount},
        {"bonusCount", e.bonusCount},
        {"requiredClassMask", e.requiredClassMask},
        {"requiredSkillId", e.requiredSkillId},
        {"requiredSkillRank", e.requiredSkillRank},
        {"itemIds", items},
        {"bonuses", bonuses},
    });
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsetExt(base);
    if (!wowee::pipeline::WoweeItemSetLoader::exists(base)) {
        std::fprintf(stderr, "WSET not found: %s.wset\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeItemSetLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wset"] = base + ".wset";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) appendEntryJson(arr, e);
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSET: %s.wset\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  sets    : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    pieces  bonuses  classMask  skill  rank   first-itemId  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u    %3u     %3u      0x%02x      %5u  %5u   %12u  %s\n",
                    e.setId, e.pieceCount, e.bonusCount,
                    e.requiredClassMask, e.requiredSkillId,
                    e.requiredSkillRank, e.itemIds[0],
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsetExt(base);
    if (!wowee::pipeline::WoweeItemSetLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wset: WSET not found: %s.wset\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeItemSetLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.setId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.setId == 0)
            errors.push_back(ctx + ": setId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.pieceCount == 0)
            errors.push_back(ctx +
                ": pieceCount=0 (set has no items)");
        if (e.pieceCount > wowee::pipeline::WoweeItemSet::kMaxPieces) {
            errors.push_back(ctx + ": pieceCount " +
                std::to_string(e.pieceCount) + " exceeds kMaxPieces (8)");
        }
        if (e.bonusCount > wowee::pipeline::WoweeItemSet::kMaxBonuses) {
            errors.push_back(ctx + ": bonusCount " +
                std::to_string(e.bonusCount) + " exceeds kMaxBonuses (4)");
        }
        // Verify the populated piece slots are non-zero and
        // the unpopulated tail is zero — drift between
        // pieceCount and the actual slot data confuses the
        // runtime resolver.
        for (size_t p = 0; p < wowee::pipeline::WoweeItemSet::kMaxPieces;
             ++p) {
            if (p < e.pieceCount) {
                if (e.itemIds[p] == 0) {
                    errors.push_back(ctx + ": piece slot " +
                        std::to_string(p) + " is 0 but pieceCount=" +
                        std::to_string(e.pieceCount));
                }
            } else {
                if (e.itemIds[p] != 0) {
                    warnings.push_back(ctx + ": piece slot " +
                        std::to_string(p) + " has itemId=" +
                        std::to_string(e.itemIds[p]) +
                        " but is past pieceCount " +
                        std::to_string(e.pieceCount) +
                        " (silently ignored at runtime)");
                }
            }
        }
        // Bonus thresholds must be ascending and within
        // pieceCount — a bonus that requires more pieces
        // than the set has can never trigger.
        uint8_t prevThreshold = 0;
        for (size_t b = 0; b < e.bonusCount; ++b) {
            uint8_t t = e.bonusThresholds[b];
            uint32_t s = e.bonusSpellIds[b];
            if (t == 0) {
                errors.push_back(ctx + ": bonus " +
                    std::to_string(b) + " has threshold=0");
            }
            if (t > e.pieceCount) {
                errors.push_back(ctx + ": bonus " +
                    std::to_string(b) + " threshold " +
                    std::to_string(t) + " > pieceCount " +
                    std::to_string(e.pieceCount) +
                    " (bonus can never trigger)");
            }
            if (s == 0) {
                errors.push_back(ctx + ": bonus " +
                    std::to_string(b) +
                    " threshold set but spellId=0 "
                    "(bonus has no aura)");
            }
            if (b > 0 && t <= prevThreshold) {
                errors.push_back(ctx + ": bonus " +
                    std::to_string(b) + " threshold " +
                    std::to_string(t) +
                    " not strictly greater than previous " +
                    std::to_string(prevThreshold));
            }
            prevThreshold = t;
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.setId) {
                errors.push_back(ctx + ": duplicate setId");
                break;
            }
        }
        idsSeen.push_back(e.setId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wset"] = base + ".wset";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wset: %s.wset\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu sets, all setIds unique, all bonus thresholds reachable\n",
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

bool handleItemSetsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-itset") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-itset-tier") == 0 && i + 1 < argc) {
        outRc = handleGenTier(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-itset-pvp") == 0 && i + 1 < argc) {
        outRc = handleGenPvP(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wset") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wset") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
