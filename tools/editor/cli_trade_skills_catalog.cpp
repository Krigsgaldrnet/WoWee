#include "cli_trade_skills_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_trade_skills.hpp"
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

std::string stripWtskExt(std::string base) {
    stripExt(base, ".wtsk");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeTradeSkill& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeTradeSkillLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wtsk\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeTradeSkill& c,
                     const std::string& base) {
    std::printf("Wrote %s.wtsk\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  recipes : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterRecipes";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtskExt(base);
    auto c = wowee::pipeline::WoweeTradeSkillLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-tsk")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBlacksmithing(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BlacksmithingRecipes";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtskExt(base);
    auto c = wowee::pipeline::WoweeTradeSkillLoader::makeBlacksmithing(name);
    if (!saveOrError(c, base, "gen-tsk-blacksmithing")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenAlchemy(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AlchemyRecipes";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtskExt(base);
    auto c = wowee::pipeline::WoweeTradeSkillLoader::makeAlchemy(name);
    if (!saveOrError(c, base, "gen-tsk-alchemy")) return 1;
    printGenSummary(c, base);
    return 0;
}

void appendEntryJson(nlohmann::json& arr,
                     const wowee::pipeline::WoweeTradeSkill::Entry& e) {
    nlohmann::json reagents = nlohmann::json::array();
    for (size_t k = 0;
         k < wowee::pipeline::WoweeTradeSkill::kMaxReagents; ++k) {
        if (e.reagentItemId[k] == 0 && e.reagentCount[k] == 0) continue;
        reagents.push_back({
            {"itemId", e.reagentItemId[k]},
            {"count", e.reagentCount[k]},
        });
    }
    arr.push_back({
        {"recipeId", e.recipeId},
        {"name", e.name},
        {"description", e.description},
        {"iconPath", e.iconPath},
        {"profession", e.profession},
        {"professionName", wowee::pipeline::WoweeTradeSkill::professionName(e.profession)},
        {"skillId", e.skillId},
        {"orangeRank", e.orangeRank},
        {"yellowRank", e.yellowRank},
        {"greenRank", e.greenRank},
        {"grayRank", e.grayRank},
        {"craftSpellId", e.craftSpellId},
        {"producedItemId", e.producedItemId},
        {"producedMinCount", e.producedMinCount},
        {"producedMaxCount", e.producedMaxCount},
        {"toolItemId", e.toolItemId},
        {"reagents", reagents},
    });
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWtskExt(base);
    if (!wowee::pipeline::WoweeTradeSkillLoader::exists(base)) {
        std::fprintf(stderr, "WTSK not found: %s.wtsk\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeTradeSkillLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wtsk"] = base + ".wtsk";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) appendEntryJson(arr, e);
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WTSK: %s.wtsk\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  recipes : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    profession      ranks(O/Y/G/Gr)         spell    item    qty   tool  rgts  name\n");
    for (const auto& e : c.entries) {
        size_t reagentCount = 0;
        for (size_t k = 0;
             k < wowee::pipeline::WoweeTradeSkill::kMaxReagents; ++k) {
            if (e.reagentItemId[k] != 0 || e.reagentCount[k] != 0)
                ++reagentCount;
        }
        std::printf("  %4u   %-13s  %3u/%3u/%3u/%3u    %5u   %5u   %u-%u   %5u  %4zu  %s\n",
                    e.recipeId,
                    wowee::pipeline::WoweeTradeSkill::professionName(e.profession),
                    e.orangeRank, e.yellowRank, e.greenRank, e.grayRank,
                    e.craftSpellId, e.producedItemId,
                    e.producedMinCount, e.producedMaxCount,
                    e.toolItemId, reagentCount, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWtskExt(base);
    if (!wowee::pipeline::WoweeTradeSkillLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wtsk: WTSK not found: %s.wtsk\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeTradeSkillLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.recipeId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.recipeId == 0)
            errors.push_back(ctx + ": recipeId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.profession > wowee::pipeline::WoweeTradeSkill::Fishing) {
            errors.push_back(ctx + ": profession " +
                std::to_string(e.profession) + " not in 0..13");
        }
        if (e.craftSpellId == 0)
            errors.push_back(ctx +
                ": craftSpellId is 0 (recipe has no craft action)");
        if (e.producedItemId == 0)
            errors.push_back(ctx +
                ": producedItemId is 0 (recipe produces nothing)");
        if (e.producedMinCount == 0 || e.producedMaxCount == 0) {
            errors.push_back(ctx +
                ": producedMin/MaxCount must be >= 1");
        }
        if (e.producedMinCount > e.producedMaxCount) {
            errors.push_back(ctx + ": producedMinCount " +
                std::to_string(e.producedMinCount) +
                " > producedMaxCount " +
                std::to_string(e.producedMaxCount));
        }
        // Skill-up bracket thresholds must be monotonic:
        // orange < yellow < green < gray.
        if (!(e.orangeRank <= e.yellowRank &&
              e.yellowRank <= e.greenRank &&
              e.greenRank  <= e.grayRank)) {
            errors.push_back(ctx +
                ": skill brackets non-monotonic (require "
                "orange <= yellow <= green <= gray, got " +
                std::to_string(e.orangeRank) + "/" +
                std::to_string(e.yellowRank) + "/" +
                std::to_string(e.greenRank)  + "/" +
                std::to_string(e.grayRank)   + ")");
        }
        if (e.skillId == 0)
            warnings.push_back(ctx +
                ": skillId=0 (recipe not bound to a WSKL skill line)");
        // A recipe with zero reagents and no tool is suspicious
        // — most crafts need at least one of the two.
        bool anyReagent = false;
        for (size_t r = 0;
             r < wowee::pipeline::WoweeTradeSkill::kMaxReagents; ++r) {
            if (e.reagentItemId[r] != 0 && e.reagentCount[r] > 0) {
                anyReagent = true; break;
            }
            if (e.reagentItemId[r] != 0 && e.reagentCount[r] == 0) {
                errors.push_back(ctx + ": reagent slot " +
                    std::to_string(r) + " has itemId=" +
                    std::to_string(e.reagentItemId[r]) +
                    " but count=0 (set count or clear itemId)");
            }
        }
        if (!anyReagent && e.toolItemId == 0) {
            warnings.push_back(ctx +
                ": no reagents and no tool — recipe is free");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.recipeId) {
                errors.push_back(ctx + ": duplicate recipeId");
                break;
            }
        }
        idsSeen.push_back(e.recipeId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wtsk"] = base + ".wtsk";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wtsk: %s.wtsk\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu recipes, all recipeIds unique, all skill brackets monotonic\n",
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

bool handleTradeSkillsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-tsk") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-tsk-blacksmithing") == 0 &&
        i + 1 < argc) {
        outRc = handleGenBlacksmithing(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-tsk-alchemy") == 0 && i + 1 < argc) {
        outRc = handleGenAlchemy(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wtsk") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wtsk") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
