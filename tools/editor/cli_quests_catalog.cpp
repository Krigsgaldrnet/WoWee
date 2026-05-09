#include "cli_quests_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_quests.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWqtExt(std::string base) {
    stripExt(base, ".wqt");
    return base;
}

void appendQuestFlagsStr(std::string& s, uint32_t flags) {
    if (flags & wowee::pipeline::WoweeQuest::Daily)        s += "daily ";
    if (flags & wowee::pipeline::WoweeQuest::Weekly)       s += "weekly ";
    if (flags & wowee::pipeline::WoweeQuest::Raid)         s += "raid ";
    if (flags & wowee::pipeline::WoweeQuest::Group)        s += "group ";
    if (flags & wowee::pipeline::WoweeQuest::AutoComplete) s += "auto-complete ";
    if (flags & wowee::pipeline::WoweeQuest::AutoAccept)   s += "auto-accept ";
    if (flags & wowee::pipeline::WoweeQuest::Repeatable)   s += "repeatable ";
    if (flags & wowee::pipeline::WoweeQuest::ClassQuest)   s += "class ";
    if (flags & wowee::pipeline::WoweeQuest::Pvp)          s += "pvp ";
    if (s.empty()) s = "-";
    else if (s.back() == ' ') s.pop_back();
}

bool saveOrError(const wowee::pipeline::WoweeQuest& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeQuestLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wqt\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeQuest& c,
                     const std::string& base) {
    std::printf("Wrote %s.wqt\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  quests  : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterQuests";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWqtExt(base);
    auto c = wowee::pipeline::WoweeQuestLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-quests")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenChain(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "QuestChain";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWqtExt(base);
    auto c = wowee::pipeline::WoweeQuestLoader::makeChain(name);
    if (!saveOrError(c, base, "gen-quests-chain")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenDaily(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "DailyQuests";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWqtExt(base);
    auto c = wowee::pipeline::WoweeQuestLoader::makeDaily(name);
    if (!saveOrError(c, base, "gen-quests-daily")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWqtExt(base);
    if (!wowee::pipeline::WoweeQuestLoader::exists(base)) {
        std::fprintf(stderr, "WQT not found: %s.wqt\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeQuestLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wqt"] = base + ".wqt";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            std::string fs;
            appendQuestFlagsStr(fs, e.flags);
            nlohmann::json je;
            je["questId"] = e.questId;
            je["title"] = e.title;
            je["objective"] = e.objective;
            je["description"] = e.description;
            je["minLevel"] = e.minLevel;
            je["questLevel"] = e.questLevel;
            je["maxLevel"] = e.maxLevel;
            je["requiredClassMask"] = e.requiredClassMask;
            je["requiredRaceMask"] = e.requiredRaceMask;
            je["prevQuestId"] = e.prevQuestId;
            je["nextQuestId"] = e.nextQuestId;
            je["giverCreatureId"] = e.giverCreatureId;
            je["turninCreatureId"] = e.turninCreatureId;
            nlohmann::json oa = nlohmann::json::array();
            for (const auto& o : e.objectives) {
                oa.push_back({
                    {"kind", o.kind},
                    {"kindName", wowee::pipeline::WoweeQuest::objectiveKindName(o.kind)},
                    {"targetId", o.targetId},
                    {"quantity", o.quantity},
                });
            }
            je["objectives"] = oa;
            je["xpReward"] = e.xpReward;
            je["moneyCopperReward"] = e.moneyCopperReward;
            nlohmann::json ra = nlohmann::json::array();
            for (const auto& r : e.rewardItems) {
                ra.push_back({
                    {"itemId", r.itemId},
                    {"qty", r.qty},
                    {"pickFlags", r.pickFlags},
                });
            }
            je["rewardItems"] = ra;
            je["flags"] = e.flags;
            je["flagsStr"] = fs;
            arr.push_back(je);
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WQT: %s.wqt\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  quests  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    for (const auto& e : c.entries) {
        std::string fs;
        appendQuestFlagsStr(fs, e.flags);
        std::printf("\n  questId=%u  level=%u  giver=%u  flags=%s\n",
                    e.questId, e.questLevel, e.giverCreatureId, fs.c_str());
        std::printf("    title    : %s\n", e.title.c_str());
        std::printf("    objective: %s\n", e.objective.c_str());
        if (!e.objectives.empty()) {
            std::printf("    targets  : ");
            for (size_t k = 0; k < e.objectives.size(); ++k) {
                const auto& o = e.objectives[k];
                std::printf("%s%s id=%u x%u",
                            k > 0 ? ", " : "",
                            wowee::pipeline::WoweeQuest::objectiveKindName(o.kind),
                            o.targetId, o.quantity);
            }
            std::printf("\n");
        }
        std::printf("    reward   : %u xp + %u copper",
                    e.xpReward, e.moneyCopperReward);
        if (!e.rewardItems.empty()) {
            std::printf(" + items: ");
            for (size_t k = 0; k < e.rewardItems.size(); ++k) {
                const auto& r = e.rewardItems[k];
                std::printf("%sitem %u x%u%s",
                            k > 0 ? ", " : "",
                            r.itemId, r.qty,
                            (r.pickFlags & wowee::pipeline::WoweeQuest::PlayerChoice) ?
                                " (choice)" : "");
            }
        }
        std::printf("\n");
        if (e.prevQuestId != 0 || e.nextQuestId != 0) {
            std::printf("    chain    : prev=%u, next=%u\n",
                        e.prevQuestId, e.nextQuestId);
        }
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWqtExt(base);
    if (!wowee::pipeline::WoweeQuestLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wqt: WQT not found: %s.wqt\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeQuestLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    idsSeen.reserve(c.entries.size());
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "quest " + std::to_string(e.questId);
        if (!e.title.empty()) ctx += " (" + e.title + ")";
        if (e.questId == 0) {
            errors.push_back(ctx + ": questId is 0");
        }
        if (e.minLevel == 0) {
            errors.push_back(ctx + ": minLevel is 0");
        }
        if (e.maxLevel != 0 && e.maxLevel < e.minLevel) {
            errors.push_back(ctx + ": maxLevel < minLevel");
        }
        if (e.title.empty()) {
            errors.push_back(ctx + ": title is empty");
        }
        // A quest with no objectives only makes sense if it's
        // a chain-bridge (auto-complete on dialogue).
        if (e.objectives.empty() &&
            !(e.flags & wowee::pipeline::WoweeQuest::AutoComplete)) {
            warnings.push_back(ctx +
                ": no objectives and not AutoComplete (player can't finish)");
        }
        // No reward at all is technically valid for chain bridges
        // but is usually a mistake.
        if (e.xpReward == 0 && e.moneyCopperReward == 0 &&
            e.rewardItems.empty()) {
            warnings.push_back(ctx + ": no rewards (xp / money / items)");
        }
        // Daily without Repeatable is contradictory.
        if ((e.flags & wowee::pipeline::WoweeQuest::Daily) &&
            !(e.flags & wowee::pipeline::WoweeQuest::Repeatable)) {
            warnings.push_back(ctx +
                ": Daily quest is not flagged Repeatable");
        }
        for (size_t oi = 0; oi < e.objectives.size(); ++oi) {
            const auto& o = e.objectives[oi];
            std::string octx = ctx + " obj " + std::to_string(oi);
            if (o.targetId == 0) {
                errors.push_back(octx + ": targetId is 0");
            }
            if (o.quantity == 0) {
                errors.push_back(octx + ": quantity is 0");
            }
            if (o.kind > wowee::pipeline::WoweeQuest::SpellCast) {
                errors.push_back(octx + ": kind " +
                    std::to_string(o.kind) + " not in known range 0..5");
            }
        }
        for (size_t ri = 0; ri < e.rewardItems.size(); ++ri) {
            const auto& r = e.rewardItems[ri];
            std::string rctx = ctx + " reward " + std::to_string(ri);
            if (r.itemId == 0) {
                errors.push_back(rctx + ": itemId is 0");
            }
            if (r.qty == 0) {
                errors.push_back(rctx + ": qty is 0");
            }
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.questId) {
                errors.push_back(ctx + ": duplicate questId");
                break;
            }
        }
        idsSeen.push_back(e.questId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wqt"] = base + ".wqt";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wqt: %s.wqt\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu quests, all questIds unique\n",
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

bool handleQuestsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-quests") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-quests-chain") == 0 && i + 1 < argc) {
        outRc = handleGenChain(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-quests-daily") == 0 && i + 1 < argc) {
        outRc = handleGenDaily(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wqt") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wqt") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
