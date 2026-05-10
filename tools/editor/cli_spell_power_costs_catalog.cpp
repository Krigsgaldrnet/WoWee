#include "cli_spell_power_costs_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_spell_power_costs.hpp"
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

std::string stripWspcExt(std::string base) {
    stripExt(base, ".wspc");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeSpellPowerCost& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSpellPowerCostLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wspc\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSpellPowerCost& c,
                     const std::string& base) {
    std::printf("Wrote %s.wspc\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  buckets : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterPowerCosts";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWspcExt(base);
    auto c = wowee::pipeline::WoweeSpellPowerCostLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-spc")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRage(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WarriorRageCosts";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWspcExt(base);
    auto c = wowee::pipeline::WoweeSpellPowerCostLoader::makeRage(name);
    if (!saveOrError(c, base, "gen-spc-rage")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMixed(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MixedClassPowerCosts";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWspcExt(base);
    auto c = wowee::pipeline::WoweeSpellPowerCostLoader::makeMixed(name);
    if (!saveOrError(c, base, "gen-spc-mixed")) return 1;
    printGenSummary(c, base);
    return 0;
}

void appendCostFlagNames(uint32_t flags, std::string& out) {
    using F = wowee::pipeline::WoweeSpellPowerCost;
    auto add = [&](const char* n) {
        if (!out.empty()) out += "|";
        out += n;
    };
    if (flags & F::RequiresCombatStance) add("RequiresCombatStance");
    if (flags & F::RefundOnMiss)         add("RefundOnMiss");
    if (flags & F::DoublesInForm)        add("DoublesInForm");
    if (flags & F::ScalesWithMastery)    add("ScalesWithMastery");
    if (out.empty()) out = "-";
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWspcExt(base);
    if (!wowee::pipeline::WoweeSpellPowerCostLoader::exists(base)) {
        std::fprintf(stderr, "WSPC not found: %s.wspc\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellPowerCostLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wspc"] = base + ".wspc";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            std::string flagNames;
            appendCostFlagNames(e.costFlags, flagNames);
            arr.push_back({
                {"powerCostId", e.powerCostId},
                {"name", e.name},
                {"description", e.description},
                {"powerType", e.powerType},
                {"powerTypeName", wowee::pipeline::WoweeSpellPowerCost::powerTypeName(e.powerType)},
                {"baseCost", e.baseCost},
                {"perLevelCost", e.perLevelCost},
                {"percentOfBase", e.percentOfBase},
                {"costFlags", e.costFlags},
                {"costFlagsLabels", flagNames},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSPC: %s.wspc\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  buckets : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    powerType    base   /lvl    %%max   flags                                  name\n");
    for (const auto& e : c.entries) {
        std::string flagNames;
        appendCostFlagNames(e.costFlags, flagNames);
        std::printf("  %4u    %-11s  %5d  %5d  %5.2f   %-36s   %s\n",
                    e.powerCostId,
                    wowee::pipeline::WoweeSpellPowerCost::powerTypeName(e.powerType),
                    e.baseCost, e.perLevelCost,
                    e.percentOfBase,
                    flagNames.c_str(),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWspcExt(base);
    if (!wowee::pipeline::WoweeSpellPowerCostLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wspc: WSPC not found: %s.wspc\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellPowerCostLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    constexpr uint32_t kKnownFlagMask =
        wowee::pipeline::WoweeSpellPowerCost::RequiresCombatStance |
        wowee::pipeline::WoweeSpellPowerCost::RefundOnMiss |
        wowee::pipeline::WoweeSpellPowerCost::DoublesInForm |
        wowee::pipeline::WoweeSpellPowerCost::ScalesWithMastery;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.powerCostId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.powerCostId == 0)
            errors.push_back(ctx + ": powerCostId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.powerType > wowee::pipeline::WoweeSpellPowerCost::NoCost) {
            errors.push_back(ctx + ": powerType " +
                std::to_string(e.powerType) + " not in 0..11");
        }
        if (e.percentOfBase < 0.0f || e.percentOfBase > 1.0f) {
            warnings.push_back(ctx +
                ": percentOfBase " + std::to_string(e.percentOfBase) +
                " is outside [0..1] — may overflow caster's max power");
        }
        if (e.costFlags & ~kKnownFlagMask) {
            warnings.push_back(ctx +
                ": costFlags has bits outside known mask " +
                "(0x" + std::to_string(e.costFlags & ~kKnownFlagMask) +
                ") — engine will ignore unknown flags");
        }
        // NoCost type with non-zero cost values is
        // contradictory.
        if (e.powerType == wowee::pipeline::WoweeSpellPowerCost::NoCost &&
            (e.baseCost != 0 || e.perLevelCost != 0 ||
             e.percentOfBase != 0.0f)) {
            warnings.push_back(ctx +
                ": NoCost type with non-zero cost fields — "
                "engine treats this as free regardless");
        }
        // Spell with no cost fields set when not NoCost — is
        // probably misconfigured (would be free).
        if (e.powerType != wowee::pipeline::WoweeSpellPowerCost::NoCost &&
            e.baseCost == 0 && e.perLevelCost == 0 &&
            e.percentOfBase == 0.0f) {
            warnings.push_back(ctx +
                ": no cost fields set but powerType is " +
                wowee::pipeline::WoweeSpellPowerCost::powerTypeName(e.powerType) +
                " — spell will cast for free, switch to NoCost type if intended");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.powerCostId) {
                errors.push_back(ctx + ": duplicate powerCostId");
                break;
            }
        }
        idsSeen.push_back(e.powerCostId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wspc"] = base + ".wspc";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wspc: %s.wspc\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu buckets, all powerCostIds unique\n",
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

bool handleSpellPowerCostsCatalog(int& i, int argc, char** argv,
                                  int& outRc) {
    if (std::strcmp(argv[i], "--gen-spc") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-spc-rage") == 0 && i + 1 < argc) {
        outRc = handleGenRage(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-spc-mixed") == 0 && i + 1 < argc) {
        outRc = handleGenMixed(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wspc") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wspc") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
