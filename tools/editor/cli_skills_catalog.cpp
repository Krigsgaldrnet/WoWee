#include "cli_skills_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_skills.hpp"
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

std::string stripWsklExt(std::string base) {
    stripExt(base, ".wskl");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeSkill& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSkillLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wskl\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSkill& c,
                     const std::string& base) {
    std::printf("Wrote %s.wskl\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  skills  : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterSkills";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsklExt(base);
    auto c = wowee::pipeline::WoweeSkillLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-skills")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenProfessions(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ProfessionSkills";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsklExt(base);
    auto c = wowee::pipeline::WoweeSkillLoader::makeProfessions(name);
    if (!saveOrError(c, base, "gen-skills-professions")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenWeapons(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WeaponSkills";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsklExt(base);
    auto c = wowee::pipeline::WoweeSkillLoader::makeWeapons(name);
    if (!saveOrError(c, base, "gen-skills-weapons")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsklExt(base);
    if (!wowee::pipeline::WoweeSkillLoader::exists(base)) {
        std::fprintf(stderr, "WSKL not found: %s.wskl\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSkillLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wskl"] = base + ".wskl";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"skillId", e.skillId},
                {"name", e.name},
                {"description", e.description},
                {"categoryId", e.categoryId},
                {"categoryName", wowee::pipeline::WoweeSkill::categoryName(e.categoryId)},
                {"canTrain", e.canTrain},
                {"maxRank", e.maxRank},
                {"rankPerLevel", e.rankPerLevel},
                {"iconPath", e.iconPath},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSKL: %s.wskl\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  skills  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   category     max  /lvl  train   name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-11s  %3u   %2u    %s     %s\n",
                    e.skillId,
                    wowee::pipeline::WoweeSkill::categoryName(e.categoryId),
                    e.maxRank, e.rankPerLevel,
                    e.canTrain ? "yes" : "no ",
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsklExt(base);
    if (!wowee::pipeline::WoweeSkillLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wskl: WSKL not found: %s.wskl\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSkillLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    idsSeen.reserve(c.entries.size());
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.skillId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.skillId == 0) {
            errors.push_back(ctx + ": skillId is 0");
        }
        if (e.name.empty()) {
            errors.push_back(ctx + ": name is empty");
        }
        if (e.maxRank == 0) {
            errors.push_back(ctx + ": maxRank is 0");
        }
        if (e.categoryId > wowee::pipeline::WoweeSkill::WeaponSpec) {
            errors.push_back(ctx + ": categoryId " +
                std::to_string(e.categoryId) + " not in 0..7");
        }
        // Languages have maxRank=1 (you either know it or you don't);
        // anything else with maxRank=1 is suspicious.
        if (e.maxRank == 1 &&
            e.categoryId != wowee::pipeline::WoweeSkill::Language) {
            warnings.push_back(ctx +
                ": maxRank=1 on non-Language skill (only languages cap at 1)");
        }
        // Weapon skills should auto-grow (rankPerLevel > 0).
        if (e.categoryId == wowee::pipeline::WoweeSkill::Weapon &&
            e.rankPerLevel == 0) {
            warnings.push_back(ctx +
                ": weapon skill with rankPerLevel=0 (won't auto-grow on use)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.skillId) {
                errors.push_back(ctx + ": duplicate skillId");
                break;
            }
        }
        idsSeen.push_back(e.skillId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wskl"] = base + ".wskl";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wskl: %s.wskl\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu skills, all skillIds unique\n",
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

bool handleSkillsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-skills") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-skills-professions") == 0 && i + 1 < argc) {
        outRc = handleGenProfessions(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-skills-weapons") == 0 && i + 1 < argc) {
        outRc = handleGenWeapons(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wskl") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wskl") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
