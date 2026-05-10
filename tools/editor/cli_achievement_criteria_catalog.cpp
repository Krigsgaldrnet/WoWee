#include "cli_achievement_criteria_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_achievement_criteria.hpp"
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

std::string stripWacrExt(std::string base) {
    stripExt(base, ".wacr");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeAchievementCriteria& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeAchievementCriteriaLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wacr\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeAchievementCriteria& c,
                     const std::string& base) {
    std::printf("Wrote %s.wacr\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  criteria : %zu\n", c.entries.size());
}

int handleGenKill(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "KillCriteria";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWacrExt(base);
    auto c = wowee::pipeline::WoweeAchievementCriteriaLoader::makeKill(name);
    if (!saveOrError(c, base, "gen-acr")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenQuest(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "QuestCriteria";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWacrExt(base);
    auto c = wowee::pipeline::WoweeAchievementCriteriaLoader::makeQuest(name);
    if (!saveOrError(c, base, "gen-acr-quest")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMixed(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MixedCriteria";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWacrExt(base);
    auto c = wowee::pipeline::WoweeAchievementCriteriaLoader::makeMixed(name);
    if (!saveOrError(c, base, "gen-acr-mixed")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWacrExt(base);
    if (!wowee::pipeline::WoweeAchievementCriteriaLoader::exists(base)) {
        std::fprintf(stderr, "WACR not found: %s.wacr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeAchievementCriteriaLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wacr"] = base + ".wacr";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"criteriaId", e.criteriaId},
                {"name", e.name},
                {"description", e.description},
                {"achievementId", e.achievementId},
                {"targetId", e.targetId},
                {"requiredCount", e.requiredCount},
                {"timeLimitMs", e.timeLimitMs},
                {"criteriaType", e.criteriaType},
                {"criteriaTypeName", wowee::pipeline::WoweeAchievementCriteria::criteriaTypeName(e.criteriaType)},
                {"progressOrder", e.progressOrder},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WACR: %s.wacr\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  criteria : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    ach     type             targetId    count   timeMs   ord   name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %5u    %-15s  %8u   %5u    %5u   %u    %s\n",
                    e.criteriaId, e.achievementId,
                    wowee::pipeline::WoweeAchievementCriteria::criteriaTypeName(e.criteriaType),
                    e.targetId, e.requiredCount, e.timeLimitMs,
                    e.progressOrder, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWacrExt(base);
    if (!wowee::pipeline::WoweeAchievementCriteriaLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wacr: WACR not found: %s.wacr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeAchievementCriteriaLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.criteriaId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.criteriaId == 0)
            errors.push_back(ctx + ": criteriaId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.achievementId == 0)
            errors.push_back(ctx +
                ": achievementId is 0 — missing WACH cross-ref");
        if (e.criteriaType > wowee::pipeline::WoweeAchievementCriteria::Misc) {
            errors.push_back(ctx + ": criteriaType " +
                std::to_string(e.criteriaType) + " not in 0..12");
        }
        if (e.requiredCount == 0)
            warnings.push_back(ctx +
                ": requiredCount is 0 — criteria completes "
                "instantly on first progress event");
        // Type-specific cross-ref checks.
        switch (e.criteriaType) {
            case wowee::pipeline::WoweeAchievementCriteria::KillCreature:
            case wowee::pipeline::WoweeAchievementCriteria::CompleteQuest:
            case wowee::pipeline::WoweeAchievementCriteria::EarnReputation:
            case wowee::pipeline::WoweeAchievementCriteria::ExploreZone:
            case wowee::pipeline::WoweeAchievementCriteria::LootItem:
            case wowee::pipeline::WoweeAchievementCriteria::UseItem:
            case wowee::pipeline::WoweeAchievementCriteria::CastSpell:
            case wowee::pipeline::WoweeAchievementCriteria::DungeonRun:
                if (e.targetId == 0) {
                    warnings.push_back(ctx +
                        ": " +
                        wowee::pipeline::WoweeAchievementCriteria::criteriaTypeName(e.criteriaType) +
                        " kind requires targetId — engine cannot "
                        "track progression without it");
                }
                break;
            case wowee::pipeline::WoweeAchievementCriteria::ReachLevel:
                if (e.requiredCount > 80) {
                    warnings.push_back(ctx +
                        ": ReachLevel with requiredCount=" +
                        std::to_string(e.requiredCount) +
                        " > 80 — character cap is 80 in WotLK");
                }
                break;
            case wowee::pipeline::WoweeAchievementCriteria::EarnGold:
            case wowee::pipeline::WoweeAchievementCriteria::GainHonor:
            case wowee::pipeline::WoweeAchievementCriteria::PvPKill:
            case wowee::pipeline::WoweeAchievementCriteria::Misc:
                break;     // no specific cross-ref required
        }
        // timeLimitMs > 0 with non-time-sensitive criteria
        // is suspicious — the engine ignores it for kinds
        // like ReachLevel.
        if (e.timeLimitMs != 0 &&
            (e.criteriaType == wowee::pipeline::WoweeAchievementCriteria::ReachLevel ||
             e.criteriaType == wowee::pipeline::WoweeAchievementCriteria::EarnGold)) {
            warnings.push_back(ctx +
                ": timeLimitMs " + std::to_string(e.timeLimitMs) +
                " set on a non-time-sensitive criteria type — "
                "engine will ignore");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.criteriaId) {
                errors.push_back(ctx + ": duplicate criteriaId");
                break;
            }
        }
        idsSeen.push_back(e.criteriaId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wacr"] = base + ".wacr";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wacr: %s.wacr\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu criteria, all criteriaIds unique\n",
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

bool handleAchievementCriteriaCatalog(int& i, int argc, char** argv,
                                      int& outRc) {
    if (std::strcmp(argv[i], "--gen-acr") == 0 && i + 1 < argc) {
        outRc = handleGenKill(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-acr-quest") == 0 && i + 1 < argc) {
        outRc = handleGenQuest(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-acr-mixed") == 0 && i + 1 < argc) {
        outRc = handleGenMixed(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wacr") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wacr") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
