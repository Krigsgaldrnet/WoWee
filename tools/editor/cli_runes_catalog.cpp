#include "cli_runes_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_runes.hpp"
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

std::string stripWrunExt(std::string base) {
    stripExt(base, ".wrun");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeRuneCost& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeRuneCostLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wrun\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeRuneCost& c,
                     const std::string& base) {
    std::printf("Wrote %s.wrun\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  costs   : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterRuneCosts";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWrunExt(base);
    auto c = wowee::pipeline::WoweeRuneCostLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-rune")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBlood(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BloodTreeRuneCosts";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWrunExt(base);
    auto c = wowee::pipeline::WoweeRuneCostLoader::makeBlood(name);
    if (!saveOrError(c, base, "gen-rune-blood")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenFrost(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FrostTreeRuneCosts";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWrunExt(base);
    auto c = wowee::pipeline::WoweeRuneCostLoader::makeFrost(name);
    if (!saveOrError(c, base, "gen-rune-frost")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWrunExt(base);
    if (!wowee::pipeline::WoweeRuneCostLoader::exists(base)) {
        std::fprintf(stderr, "WRUN not found: %s.wrun\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeRuneCostLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wrun"] = base + ".wrun";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"runeCostId", e.runeCostId},
                {"spellId", e.spellId},
                {"name", e.name},
                {"description", e.description},
                {"bloodCost", e.bloodCost},
                {"frostCost", e.frostCost},
                {"unholyCost", e.unholyCost},
                {"anyDeathConvertCost", e.anyDeathConvertCost},
                {"runicPowerCost", e.runicPowerCost},
                {"spellTreeBranch", e.spellTreeBranch},
                {"spellTreeBranchName", wowee::pipeline::WoweeRuneCost::spellTreeBranchName(e.spellTreeBranch)},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WRUN: %s.wrun\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  costs   : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    spell    B  F  U  Any   RP    branch    name\n");
    for (const auto& e : c.entries) {
        const char* rpKind = e.runicPowerCost == 0
            ? "  "
            : (e.runicPowerCost > 0 ? "→" : "←");
        std::printf("  %4u    %5u   %u  %u  %u   %u   %s%4d  %-8s  %s\n",
                    e.runeCostId, e.spellId,
                    e.bloodCost, e.frostCost, e.unholyCost,
                    e.anyDeathConvertCost,
                    rpKind, e.runicPowerCost,
                    wowee::pipeline::WoweeRuneCost::spellTreeBranchName(e.spellTreeBranch),
                    e.name.c_str());
    }
    std::printf("  legend: B/F/U/Any = blood/frost/unholy/death-convertible costs; "
                "RP arrow shows generator (←) vs spender (→)\n");
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWrunExt(base);
    if (!wowee::pipeline::WoweeRuneCostLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wrun: WRUN not found: %s.wrun\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeRuneCostLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.runeCostId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.runeCostId == 0)
            errors.push_back(ctx + ": runeCostId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.spellId == 0)
            errors.push_back(ctx +
                ": spellId is 0 (rune cost not bound to a WSPL spell)");
        if (e.spellTreeBranch > wowee::pipeline::WoweeRuneCost::Generic) {
            errors.push_back(ctx + ": spellTreeBranch " +
                std::to_string(e.spellTreeBranch) + " not in 0..3");
        }
        // A DK has 6 runes total (2 of each kind) — a single
        // ability cost shouldn't exceed 2 of any one type
        // because the system can't satisfy it.
        if (e.bloodCost > 2)
            errors.push_back(ctx + ": bloodCost " +
                std::to_string(e.bloodCost) + " exceeds 2 (DK only "
                "has 2 blood runes)");
        if (e.frostCost > 2)
            errors.push_back(ctx + ": frostCost " +
                std::to_string(e.frostCost) + " exceeds 2");
        if (e.unholyCost > 2)
            errors.push_back(ctx + ": unholyCost " +
                std::to_string(e.unholyCost) + " exceeds 2");
        // A spell with no rune cost AND no runic-power cost
        // is weird — every DK ability either consumes
        // resources, generates them, or applies a stance.
        // We don't have stance info here, so warn.
        bool noResourceCost = e.bloodCost == 0 && e.frostCost == 0 &&
                              e.unholyCost == 0 &&
                              e.anyDeathConvertCost == 0 &&
                              e.runicPowerCost == 0;
        if (noResourceCost) {
            warnings.push_back(ctx +
                ": no rune or runic-power cost — verify this is "
                "intentional (passive / stance / forms only)");
        }
        // RP cost > 100 isn't possible — max RP cap is 100.
        if (e.runicPowerCost > 100) {
            errors.push_back(ctx +
                ": runicPowerCost " +
                std::to_string(e.runicPowerCost) +
                " exceeds 100 (DK runic power max)");
        }
        if (e.runicPowerCost < -25) {
            warnings.push_back(ctx +
                ": runicPowerCost " +
                std::to_string(e.runicPowerCost) +
                " generates more than 25 RP per cast — unusual");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.runeCostId) {
                errors.push_back(ctx + ": duplicate runeCostId");
                break;
            }
        }
        idsSeen.push_back(e.runeCostId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wrun"] = base + ".wrun";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wrun: %s.wrun\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu rune costs, all runeCostIds unique, all costs within DK budget\n",
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

bool handleRunesCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-rune") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-rune-blood") == 0 && i + 1 < argc) {
        outRc = handleGenBlood(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-rune-frost") == 0 && i + 1 < argc) {
        outRc = handleGenFrost(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wrun") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wrun") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
