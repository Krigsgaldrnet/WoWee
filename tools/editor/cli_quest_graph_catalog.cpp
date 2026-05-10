#include "cli_quest_graph_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_quest_graph.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWqgrExt(std::string base) {
    stripExt(base, ".wqgr");
    return base;
}

const char* questTypeName(uint8_t t) {
    using G = wowee::pipeline::WoweeQuestGraph;
    switch (t) {
        case G::Normal:     return "normal";
        case G::Daily:      return "daily";
        case G::Repeatable: return "repeatable";
        case G::Group:      return "group";
        case G::Raid:       return "raid";
        default:            return "?";
    }
}

const char* factionAccessName(uint8_t f) {
    using G = wowee::pipeline::WoweeQuestGraph;
    switch (f) {
        case G::Both:     return "both";
        case G::Alliance: return "alliance";
        case G::Horde:    return "horde";
        case G::Neutral:  return "neutral";
        default:          return "?";
    }
}

bool saveOrError(const wowee::pipeline::WoweeQuestGraph& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeQuestGraphLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wqgr\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeQuestGraph& c,
                     const std::string& base) {
    std::printf("Wrote %s.wqgr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  quests  : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "NorthshireStarterChain";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWqgrExt(base);
    auto c = wowee::pipeline::WoweeQuestGraphLoader::
        makeStarterChain(name);
    if (!saveOrError(c, base, "gen-qgr-starter")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBranched(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BranchedConvergingChain";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWqgrExt(base);
    auto c = wowee::pipeline::WoweeQuestGraphLoader::
        makeBranchedChain(name);
    if (!saveOrError(c, base, "gen-qgr-branched")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenDailies(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "DailyQuests";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWqgrExt(base);
    auto c = wowee::pipeline::WoweeQuestGraphLoader::
        makeDailies(name);
    if (!saveOrError(c, base, "gen-qgr-dailies")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWqgrExt(base);
    if (!wowee::pipeline::WoweeQuestGraphLoader::exists(base)) {
        std::fprintf(stderr, "WQGR not found: %s.wqgr\n",
                     base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeQuestGraphLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wqgr"] = base + ".wqgr";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"questId", e.questId},
                {"name", e.name},
                {"minLevel", e.minLevel},
                {"maxLevel", e.maxLevel},
                {"questType", e.questType},
                {"questTypeName", questTypeName(e.questType)},
                {"factionAccess", e.factionAccess},
                {"factionAccessName",
                    factionAccessName(e.factionAccess)},
                {"classRestriction", e.classRestriction},
                {"raceRestriction", e.raceRestriction},
                {"zoneId", e.zoneId},
                {"chainHeadHint", e.chainHeadHint != 0},
                {"prevQuestIds", e.prevQuestIds},
                {"followupQuestIds", e.followupQuestIds},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WQGR: %s.wqgr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  quests  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   minL maxL  type        fact      zone  head  prev  fup  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %3u  %3u  %-10s  %-8s  %4u  %s    %4zu  %3zu  %s\n",
                    e.questId, e.minLevel, e.maxLevel,
                    questTypeName(e.questType),
                    factionAccessName(e.factionAccess),
                    e.zoneId,
                    e.chainHeadHint ? "Y" : "n",
                    e.prevQuestIds.size(),
                    e.followupQuestIds.size(),
                    e.name.c_str());
    }
    return 0;
}

// DFS cycle detection over prevQuestIds — same
// stack-based pattern as WMOD addon manifest. A
// cycle in quest prereqs means the quest is
// unreachable (player would need to complete Q1 to
// start Q2, but Q1 prereq is Q2).
std::vector<uint32_t> findFirstCycle(
    const wowee::pipeline::WoweeQuestGraph& c) {
    std::map<uint32_t, std::vector<uint32_t>> graph;
    std::set<uint32_t> known;
    for (const auto& e : c.entries) {
        graph[e.questId] = e.prevQuestIds;
        known.insert(e.questId);
    }
    enum Color : uint8_t { White = 0, Gray = 1, Black = 2 };
    std::map<uint32_t, Color> color;
    for (uint32_t id : known) color[id] = White;
    std::vector<uint32_t> path;
    std::vector<uint32_t> cycle;
    std::function<bool(uint32_t)> dfs = [&](uint32_t node) -> bool {
        color[node] = Gray;
        path.push_back(node);
        for (uint32_t prev : graph[node]) {
            if (!known.count(prev)) continue;
            if (color[prev] == Gray) {
                auto it = std::find(path.begin(), path.end(), prev);
                cycle.assign(it, path.end());
                cycle.push_back(prev);
                return true;
            }
            if (color[prev] == White) {
                if (dfs(prev)) return true;
            }
        }
        color[node] = Black;
        path.pop_back();
        return false;
    };
    for (uint32_t id : known) {
        if (color[id] == White && dfs(id)) return cycle;
    }
    return {};
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWqgrExt(base);
    if (!wowee::pipeline::WoweeQuestGraphLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wqgr: WQGR not found: %s.wqgr\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeQuestGraphLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    std::set<uint32_t> knownIds;
    for (const auto& e : c.entries) knownIds.insert(e.questId);
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (questId=" + std::to_string(e.questId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.questId == 0)
            errors.push_back(ctx + ": questId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.questType > 4) {
            errors.push_back(ctx + ": questType " +
                std::to_string(e.questType) +
                " out of range (0..4)");
        }
        if (e.factionAccess > 3) {
            errors.push_back(ctx + ": factionAccess " +
                std::to_string(e.factionAccess) +
                " out of range (0..3)");
        }
        if (e.maxLevel != 0 && e.maxLevel < e.minLevel) {
            errors.push_back(ctx + ": maxLevel " +
                std::to_string(e.maxLevel) +
                " < minLevel " +
                std::to_string(e.minLevel));
        }
        // Self-prereq: a quest listing itself in
        // its own prereqs is unreachable (catch-22).
        for (uint32_t prev : e.prevQuestIds) {
            if (prev == e.questId) {
                errors.push_back(ctx +
                    ": quest depends on itself "
                    "(unreachable — catch-22)");
            }
            if (!knownIds.count(prev)) {
                errors.push_back(ctx +
                    ": prereq questId=" +
                    std::to_string(prev) +
                    " not found in catalog");
            }
        }
        // Followup hints to unknown ids: NOT an
        // error — followups are advisory hints,
        // the missing target may live in a sibling
        // catalog. Just warn.
        for (uint32_t fol : e.followupQuestIds) {
            if (fol == e.questId) {
                warnings.push_back(ctx +
                    ": followup hint points to self "
                    "(no-op — prune)");
            }
            if (!knownIds.count(fol)) {
                warnings.push_back(ctx +
                    ": followup hint points to "
                    "questId=" + std::to_string(fol) +
                    " not in this catalog (may live "
                    "in a sibling catalog)");
            }
        }
        // chainHeadHint=1 with non-empty prereqs is a
        // contradiction — a chain head BY DEFINITION
        // has no prereqs. Warn.
        if (e.chainHeadHint && !e.prevQuestIds.empty()) {
            warnings.push_back(ctx +
                ": chainHeadHint=1 but quest has " +
                std::to_string(e.prevQuestIds.size()) +
                " prereq(s) — chain heads should have "
                "no prereqs");
        }
        if (!idsSeen.insert(e.questId).second) {
            errors.push_back(ctx + ": duplicate questId");
        }
    }
    // DFS cycle on prevQuestIds — same pattern as WMOD.
    auto cycle = findFirstCycle(c);
    if (!cycle.empty()) {
        std::string trail;
        for (size_t k = 0; k < cycle.size(); ++k) {
            if (k > 0) trail += " -> ";
            trail += std::to_string(cycle[k]);
        }
        errors.push_back("prereq cycle detected: " + trail +
                          " — quests would be unreachable "
                          "(progression deadlock)");
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wqgr"] = base + ".wqgr";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wqgr: %s.wqgr\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu quests, all questIds unique, "
                    "questType 0..4, factionAccess 0..3, no "
                    "self-prereq, no missing prereq questId, "
                    "no DFS cycle (no progression deadlock)\n",
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

bool handleQuestGraphCatalog(int& i, int argc, char** argv,
                               int& outRc) {
    if (std::strcmp(argv[i], "--gen-qgr-starter") == 0 &&
        i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-qgr-branched") == 0 &&
        i + 1 < argc) {
        outRc = handleGenBranched(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-qgr-dailies") == 0 &&
        i + 1 < argc) {
        outRc = handleGenDailies(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wqgr") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wqgr") == 0 &&
        i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
