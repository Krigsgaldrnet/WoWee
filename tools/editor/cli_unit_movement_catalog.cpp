#include "cli_unit_movement_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_unit_movement.hpp"
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

std::string stripWumvExt(std::string base) {
    stripExt(base, ".wumv");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeUnitMovement& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeUnitMovementLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wumv\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeUnitMovement& c,
                     const std::string& base) {
    std::printf("Wrote %s.wumv\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  types   : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterMovementTypes";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWumvExt(base);
    auto c = wowee::pipeline::WoweeUnitMovementLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-umv")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenFlight(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FlightMovementTypes";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWumvExt(base);
    auto c = wowee::pipeline::WoweeUnitMovementLoader::makeFlight(name);
    if (!saveOrError(c, base, "gen-umv-flight")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBuffs(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MovementBuffs";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWumvExt(base);
    auto c = wowee::pipeline::WoweeUnitMovementLoader::makeBuffs(name);
    if (!saveOrError(c, base, "gen-umv-buffs")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWumvExt(base);
    if (!wowee::pipeline::WoweeUnitMovementLoader::exists(base)) {
        std::fprintf(stderr, "WUMV not found: %s.wumv\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeUnitMovementLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wumv"] = base + ".wumv";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"moveTypeId", e.moveTypeId},
                {"name", e.name},
                {"description", e.description},
                {"iconPath", e.iconPath},
                {"movementCategory", e.movementCategory},
                {"movementCategoryName", wowee::pipeline::WoweeUnitMovement::movementCategoryName(e.movementCategory)},
                {"requiresFlight", e.requiresFlight},
                {"canStackBuffs", e.canStackBuffs},
                {"baseSpeed", e.baseSpeed},
                {"baseMultiplier", e.baseMultiplier},
                {"maxMultiplier", e.maxMultiplier},
                {"defaultDurationMs", e.defaultDurationMs},
                {"stackingPriority", e.stackingPriority},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WUMV: %s.wumv\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  types   : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    category     baseSpeed  baseMul  maxMul  dur(ms)  prio  flight  stack  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-11s   %7.2f    %5.2f    %5.2f   %5u   %3u    %u       %u    %s\n",
                    e.moveTypeId,
                    wowee::pipeline::WoweeUnitMovement::movementCategoryName(e.movementCategory),
                    e.baseSpeed, e.baseMultiplier, e.maxMultiplier,
                    e.defaultDurationMs, e.stackingPriority,
                    e.requiresFlight, e.canStackBuffs,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWumvExt(base);
    if (!wowee::pipeline::WoweeUnitMovementLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wumv: WUMV not found: %s.wumv\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeUnitMovementLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.moveTypeId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.moveTypeId == 0)
            errors.push_back(ctx + ": moveTypeId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.movementCategory > wowee::pipeline::WoweeUnitMovement::TempBuff) {
            errors.push_back(ctx + ": movementCategory " +
                std::to_string(e.movementCategory) + " not in 0..11");
        }
        if (e.baseMultiplier <= 0.0f) {
            errors.push_back(ctx +
                ": baseMultiplier " +
                std::to_string(e.baseMultiplier) +
                " <= 0 (would freeze unit in place)");
        }
        if (e.maxMultiplier < e.baseMultiplier) {
            errors.push_back(ctx + ": maxMultiplier " +
                std::to_string(e.maxMultiplier) +
                " < baseMultiplier " +
                std::to_string(e.baseMultiplier) +
                " (cap below floor — base would be clamped down)");
        }
        // Baseline movement (Walk/Run/Swim/Turn) should have
        // a positive baseSpeed — TempBuff entries are
        // multiplier-only and may have baseSpeed=0.
        bool isBaseline =
            e.movementCategory >= wowee::pipeline::WoweeUnitMovement::Walk &&
            e.movementCategory <= wowee::pipeline::WoweeUnitMovement::FlyBack;
        if (isBaseline && e.baseSpeed <= 0.0f) {
            errors.push_back(ctx +
                ": baseline movement category with baseSpeed " +
                std::to_string(e.baseSpeed) + " <= 0 — unit "
                "won't move on this category");
        }
        // Run speed below walk speed is suspicious — flag.
        // Run is ~7.0y/s, walk is ~2.5y/s in canonical WoW.
        if (e.movementCategory == wowee::pipeline::WoweeUnitMovement::Run &&
            e.baseSpeed < 3.0f) {
            warnings.push_back(ctx +
                ": Run baseSpeed " +
                std::to_string(e.baseSpeed) +
                " unusually slow (canonical 7.0y/s) — verify intent");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.moveTypeId) {
                errors.push_back(ctx + ": duplicate moveTypeId");
                break;
            }
        }
        idsSeen.push_back(e.moveTypeId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wumv"] = base + ".wumv";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wumv: %s.wumv\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu movement types, all moveTypeIds unique, all multipliers consistent\n",
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

bool handleUnitMovementCatalog(int& i, int argc, char** argv,
                               int& outRc) {
    if (std::strcmp(argv[i], "--gen-umv") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-umv-flight") == 0 && i + 1 < argc) {
        outRc = handleGenFlight(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-umv-buffs") == 0 && i + 1 < argc) {
        outRc = handleGenBuffs(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wumv") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wumv") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
