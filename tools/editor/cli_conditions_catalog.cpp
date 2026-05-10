#include "cli_conditions_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_conditions.hpp"
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

std::string stripWpcdExt(std::string base) {
    stripExt(base, ".wpcd");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeCondition& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeConditionLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wpcd\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeCondition& c,
                     const std::string& base) {
    std::printf("Wrote %s.wpcd\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  conditions : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterConditions";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpcdExt(base);
    auto c = wowee::pipeline::WoweeConditionLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-conditions")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenGated(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "GatedConditions";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpcdExt(base);
    auto c = wowee::pipeline::WoweeConditionLoader::makeGated(name);
    if (!saveOrError(c, base, "gen-conditions-gated")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenEvent(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "EventConditions";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpcdExt(base);
    auto c = wowee::pipeline::WoweeConditionLoader::makeEvent(name);
    if (!saveOrError(c, base, "gen-conditions-event")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWpcdExt(base);
    if (!wowee::pipeline::WoweeConditionLoader::exists(base)) {
        std::fprintf(stderr, "WPCD not found: %s.wpcd\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeConditionLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wpcd"] = base + ".wpcd";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"conditionId", e.conditionId},
                {"groupId", e.groupId},
                {"name", e.name},
                {"description", e.description},
                {"kind", e.kind},
                {"kindName", wowee::pipeline::WoweeCondition::kindName(e.kind)},
                {"aggregator", e.aggregator},
                {"aggregatorName", wowee::pipeline::WoweeCondition::aggregatorName(e.aggregator)},
                {"negated", e.negated},
                {"targetId", e.targetId},
                {"minValue", e.minValue},
                {"maxValue", e.maxValue},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WPCD: %s.wpcd\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  conditions : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    grp   kind          agg  neg  target   min/max         name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %4u  %-12s  %-3s  %u    %5u    %5d/%-5d  %s\n",
                    e.conditionId, e.groupId,
                    wowee::pipeline::WoweeCondition::kindName(e.kind),
                    wowee::pipeline::WoweeCondition::aggregatorName(e.aggregator),
                    e.negated, e.targetId,
                    e.minValue, e.maxValue, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWpcdExt(base);
    if (!wowee::pipeline::WoweeConditionLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wpcd: WPCD not found: %s.wpcd\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeConditionLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.conditionId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.conditionId == 0) {
            errors.push_back(ctx + ": conditionId is 0");
        }
        if (e.kind > wowee::pipeline::WoweeCondition::HasTitle) {
            errors.push_back(ctx + ": kind " +
                std::to_string(e.kind) + " not in 0..16");
        }
        if (e.aggregator > wowee::pipeline::WoweeCondition::Or) {
            errors.push_back(ctx + ": aggregator " +
                std::to_string(e.aggregator) + " not in 0..1");
        }
        // Most kinds need a non-zero targetId. Exceptions:
        // AlwaysTrue / AlwaysFalse (no target), MinLevel /
        // MaxLevel (use minValue), TeamSize (uses min/max),
        // GuildLevel (uses minValue).
        bool needsTarget =
            e.kind != wowee::pipeline::WoweeCondition::AlwaysTrue &&
            e.kind != wowee::pipeline::WoweeCondition::AlwaysFalse &&
            e.kind != wowee::pipeline::WoweeCondition::MinLevel &&
            e.kind != wowee::pipeline::WoweeCondition::MaxLevel &&
            e.kind != wowee::pipeline::WoweeCondition::TeamSize &&
            e.kind != wowee::pipeline::WoweeCondition::GuildLevel;
        if (needsTarget && e.targetId == 0) {
            errors.push_back(ctx +
                ": kind needs a non-zero targetId");
        }
        if (e.kind == wowee::pipeline::WoweeCondition::TeamSize &&
            e.minValue > 0 && e.maxValue > 0 &&
            e.minValue > e.maxValue) {
            errors.push_back(ctx + ": team-size minValue > maxValue");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.conditionId) {
                errors.push_back(ctx + ": duplicate conditionId");
                break;
            }
        }
        idsSeen.push_back(e.conditionId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wpcd"] = base + ".wpcd";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wpcd: %s.wpcd\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu conditions, all conditionIds unique\n",
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

bool handleConditionsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-conditions") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-conditions-gated") == 0 && i + 1 < argc) {
        outRc = handleGenGated(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-conditions-event") == 0 && i + 1 < argc) {
        outRc = handleGenEvent(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wpcd") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wpcd") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
