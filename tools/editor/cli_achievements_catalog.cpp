#include "cli_achievements_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_achievements.hpp"
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

std::string stripWachExt(std::string base) {
    stripExt(base, ".wach");
    return base;
}

void appendAchFlagsStr(std::string& s, uint32_t flags) {
    if (flags & wowee::pipeline::WoweeAchievement::HiddenUntilEarned) s += "hidden ";
    if (flags & wowee::pipeline::WoweeAchievement::ServerFirst)       s += "server-first ";
    if (flags & wowee::pipeline::WoweeAchievement::RealmFirst)        s += "realm-first ";
    if (flags & wowee::pipeline::WoweeAchievement::Tracking)          s += "tracking ";
    if (flags & wowee::pipeline::WoweeAchievement::Counter)           s += "counter ";
    if (flags & wowee::pipeline::WoweeAchievement::Account)           s += "account ";
    if (s.empty()) s = "-";
    else if (s.back() == ' ') s.pop_back();
}

bool saveOrError(const wowee::pipeline::WoweeAchievement& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeAchievementLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wach\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

uint32_t totalCriteria(const wowee::pipeline::WoweeAchievement& c) {
    uint32_t n = 0;
    for (const auto& e : c.entries) n += static_cast<uint32_t>(e.criteria.size());
    return n;
}

void printGenSummary(const wowee::pipeline::WoweeAchievement& c,
                     const std::string& base) {
    std::printf("Wrote %s.wach\n", base.c_str());
    std::printf("  catalog      : %s\n", c.name.c_str());
    std::printf("  achievements : %zu (%u criteria total)\n",
                c.entries.size(), totalCriteria(c));
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterAchievements";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWachExt(base);
    auto c = wowee::pipeline::WoweeAchievementLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-achievements")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBandit(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BanditAchievements";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWachExt(base);
    auto c = wowee::pipeline::WoweeAchievementLoader::makeBandit(name);
    if (!saveOrError(c, base, "gen-achievements-bandit")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMeta(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MetaAchievements";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWachExt(base);
    auto c = wowee::pipeline::WoweeAchievementLoader::makeMeta(name);
    if (!saveOrError(c, base, "gen-achievements-meta")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWachExt(base);
    if (!wowee::pipeline::WoweeAchievementLoader::exists(base)) {
        std::fprintf(stderr, "WACH not found: %s.wach\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeAchievementLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wach"] = base + ".wach";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        j["totalCriteria"] = totalCriteria(c);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            std::string fs;
            appendAchFlagsStr(fs, e.flags);
            nlohmann::json je;
            je["achievementId"] = e.achievementId;
            je["categoryId"] = e.categoryId;
            je["name"] = e.name;
            je["description"] = e.description;
            je["iconPath"] = e.iconPath;
            je["titleReward"] = e.titleReward;
            je["points"] = e.points;
            je["minLevel"] = e.minLevel;
            je["faction"] = e.faction;
            je["factionName"] = wowee::pipeline::WoweeAchievement::factionName(e.faction);
            je["flags"] = e.flags;
            je["flagsStr"] = fs;
            nlohmann::json ca = nlohmann::json::array();
            for (const auto& cr : e.criteria) {
                ca.push_back({
                    {"criteriaId", cr.criteriaId},
                    {"kind", cr.kind},
                    {"kindName", wowee::pipeline::WoweeAchievement::criteriaKindName(cr.kind)},
                    {"targetId", cr.targetId},
                    {"quantity", cr.quantity},
                    {"description", cr.description},
                });
            }
            je["criteria"] = ca;
            arr.push_back(je);
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WACH: %s.wach\n", base.c_str());
    std::printf("  catalog      : %s\n", c.name.c_str());
    std::printf("  achievements : %zu (%u criteria total)\n",
                c.entries.size(), totalCriteria(c));
    if (c.entries.empty()) return 0;
    for (const auto& e : c.entries) {
        std::string fs;
        appendAchFlagsStr(fs, e.flags);
        std::printf("\n  achievementId=%u  points=%u  faction=%s  flags=%s%s%s\n",
                    e.achievementId, e.points,
                    wowee::pipeline::WoweeAchievement::factionName(e.faction),
                    fs.c_str(),
                    e.titleReward.empty() ? "" : "  title=",
                    e.titleReward.c_str());
        std::printf("    name : %s\n", e.name.c_str());
        if (e.criteria.empty()) {
            std::printf("    *no criteria*\n");
            continue;
        }
        for (const auto& cr : e.criteria) {
            std::printf("    [%-5s] target=%-5u  qty=%u   %s\n",
                        wowee::pipeline::WoweeAchievement::criteriaKindName(cr.kind),
                        cr.targetId, cr.quantity,
                        cr.description.c_str());
        }
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWachExt(base);
    if (!wowee::pipeline::WoweeAchievementLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wach: WACH not found: %s.wach\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeAchievementLoader::load(base);
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
                          " (id=" + std::to_string(e.achievementId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.achievementId == 0) {
            errors.push_back(ctx + ": achievementId is 0");
        }
        if (e.name.empty()) {
            errors.push_back(ctx + ": name is empty");
        }
        if (e.faction > wowee::pipeline::WoweeAchievement::FactionHorde) {
            errors.push_back(ctx + ": faction " +
                std::to_string(e.faction) + " not in 0..2");
        }
        if (e.criteria.empty()) {
            warnings.push_back(ctx +
                ": no criteria (achievement can never be earned)");
        }
        for (size_t ci = 0; ci < e.criteria.size(); ++ci) {
            const auto& cr = e.criteria[ci];
            std::string cctx = ctx + " criterion " + std::to_string(ci);
            if (cr.kind > wowee::pipeline::WoweeAchievement::CompleteAchievement) {
                errors.push_back(cctx + ": kind " +
                    std::to_string(cr.kind) + " not in 0..8");
            }
            if (cr.quantity == 0) {
                errors.push_back(cctx + ": quantity is 0");
            }
            // ReachLevel and Counter-style criteria can have
            // targetId=0; everything else needs a real target.
            bool needsTarget =
                cr.kind != wowee::pipeline::WoweeAchievement::ReachLevel;
            if (needsTarget && cr.targetId == 0) {
                errors.push_back(cctx + ": targetId is 0 (no resource referenced)");
            }
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.achievementId) {
                errors.push_back(ctx + ": duplicate achievementId");
                break;
            }
        }
        idsSeen.push_back(e.achievementId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wach"] = base + ".wach";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wach: %s.wach\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu achievements (%u criteria), all IDs unique\n",
                    c.entries.size(), totalCriteria(c));
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

bool handleAchievementsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-achievements") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-achievements-bandit") == 0 && i + 1 < argc) {
        outRc = handleGenBandit(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-achievements-meta") == 0 && i + 1 < argc) {
        outRc = handleGenMeta(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wach") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wach") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
