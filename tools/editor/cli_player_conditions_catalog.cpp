#include "cli_player_conditions_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_player_conditions.hpp"
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

std::string stripWpcnExt(std::string base) {
    stripExt(base, ".wpcn");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweePlayerCondition& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweePlayerConditionLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wpcn\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweePlayerCondition& c,
                     const std::string& base) {
    std::printf("Wrote %s.wpcn\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  conditions : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterConditions";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpcnExt(base);
    auto c = wowee::pipeline::WoweePlayerConditionLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-pcn")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenQuestGates(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "QuestGateConditions";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpcnExt(base);
    auto c = wowee::pipeline::WoweePlayerConditionLoader::makeQuestGates(name);
    if (!saveOrError(c, base, "gen-pcn-quest-gates")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenComposite(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "CompositeConditions";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpcnExt(base);
    auto c = wowee::pipeline::WoweePlayerConditionLoader::makeComposite(name);
    if (!saveOrError(c, base, "gen-pcn-composite")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWpcnExt(base);
    if (!wowee::pipeline::WoweePlayerConditionLoader::exists(base)) {
        std::fprintf(stderr, "WPCN not found: %s.wpcn\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweePlayerConditionLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wpcn"] = base + ".wpcn";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"conditionId", e.conditionId},
                {"name", e.name},
                {"description", e.description},
                {"conditionKind", e.conditionKind},
                {"conditionKindName", wowee::pipeline::WoweePlayerCondition::conditionKindName(e.conditionKind)},
                {"comparisonOp", e.comparisonOp},
                {"comparisonOpName", wowee::pipeline::WoweePlayerCondition::comparisonOpName(e.comparisonOp)},
                {"chainOp", e.chainOp},
                {"chainOpName", wowee::pipeline::WoweePlayerCondition::chainOpName(e.chainOp)},
                {"targetIdA", e.targetIdA},
                {"targetIdB", e.targetIdB},
                {"intValueA", e.intValueA},
                {"intValueB", e.intValueB},
                {"chainNextId", e.chainNextId},
                {"failMessage", e.failMessage},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WPCN: %s.wpcn\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  conditions : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id     kind             op          tgtA   tgtB   intA   intB   chain  next   name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u    %-14s   %-10s   %4u   %4u  %5d  %5d   %-5s  %4u   %s\n",
                    e.conditionId,
                    wowee::pipeline::WoweePlayerCondition::conditionKindName(e.conditionKind),
                    wowee::pipeline::WoweePlayerCondition::comparisonOpName(e.comparisonOp),
                    e.targetIdA, e.targetIdB,
                    e.intValueA, e.intValueB,
                    wowee::pipeline::WoweePlayerCondition::chainOpName(e.chainOp),
                    e.chainNextId, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWpcnExt(base);
    if (!wowee::pipeline::WoweePlayerConditionLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wpcn: WPCN not found: %s.wpcn\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweePlayerConditionLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (const auto& e : c.entries) idsSeen.push_back(e.conditionId);
    auto idExists = [&](uint32_t id) {
        for (uint32_t a : idsSeen) if (a == id) return true;
        return false;
    };
    std::vector<uint32_t> dupCheck;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.conditionId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.conditionId == 0)
            errors.push_back(ctx + ": conditionId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.conditionKind > wowee::pipeline::WoweePlayerCondition::Resting) {
            errors.push_back(ctx + ": conditionKind " +
                std::to_string(e.conditionKind) + " not in 0..15");
        }
        if (e.comparisonOp > wowee::pipeline::WoweePlayerCondition::NotInSet) {
            errors.push_back(ctx + ": comparisonOp " +
                std::to_string(e.comparisonOp) + " not in 0..7");
        }
        if (e.chainOp > wowee::pipeline::WoweePlayerCondition::ChainNot) {
            errors.push_back(ctx + ": chainOp " +
                std::to_string(e.chainOp) + " not in 0..3");
        }
        // chainOp != ChainNone requires a non-zero chainNextId
        // — and that ID must point at another condition in
        // this catalog.
        if (e.chainOp != wowee::pipeline::WoweePlayerCondition::ChainNone) {
            if (e.chainNextId == 0) {
                errors.push_back(ctx + ": chainOp '" +
                    wowee::pipeline::WoweePlayerCondition::chainOpName(e.chainOp) +
                    "' set but chainNextId=0 (chain has no tail)");
            } else if (e.chainNextId == e.conditionId) {
                errors.push_back(ctx +
                    ": chainNextId equals conditionId "
                    "(infinite loop)");
            } else if (!idExists(e.chainNextId)) {
                warnings.push_back(ctx + ": chainNextId=" +
                    std::to_string(e.chainNextId) +
                    " not found in this catalog (resolved at runtime)");
            }
        }
        // chainOp == ChainNone and chainNextId != 0 is dead
        // pointer — chainNextId is silently unused.
        if (e.chainOp == wowee::pipeline::WoweePlayerCondition::ChainNone &&
            e.chainNextId != 0) {
            warnings.push_back(ctx +
                ": chainNextId set but chainOp=none "
                "(silently ignored at runtime)");
        }
        // duplicates
        for (size_t m = 0; m < k; ++m) {
            if (c.entries[m].conditionId == e.conditionId) {
                errors.push_back(ctx + ": duplicate conditionId");
                break;
            }
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wpcn"] = base + ".wpcn";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wpcn: %s.wpcn\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu conditions, all conditionIds unique, all chains resolved\n",
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

bool handlePlayerConditionsCatalog(int& i, int argc, char** argv,
                                   int& outRc) {
    if (std::strcmp(argv[i], "--gen-pcn") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-pcn-quest-gates") == 0 &&
        i + 1 < argc) {
        outRc = handleGenQuestGates(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-pcn-composite") == 0 &&
        i + 1 < argc) {
        outRc = handleGenComposite(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wpcn") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wpcn") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
