#include "cli_combat_formulas_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_combat_formulas.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWcfrExt(std::string base) {
    stripExt(base, ".wcfr");
    return base;
}

const char* outputStatKindName(uint8_t k) {
    using F = wowee::pipeline::WoweeCombatFormulas;
    switch (k) {
        case F::AttackPower:  return "ap";
        case F::SpellPower:   return "sp";
        case F::CritPct:      return "crit";
        case F::DodgePct:     return "dodge";
        case F::ParryPct:     return "parry";
        case F::HitPct:       return "hit";
        case F::SpellCritPct: return "spellcrit";
        case F::HastePct:     return "haste";
        default:              return "?";
    }
}

const char* inputStatKindName(uint8_t k) {
    using F = wowee::pipeline::WoweeCombatFormulas;
    switch (k) {
        case F::Strength:  return "str";
        case F::Agility:   return "agi";
        case F::Stamina:   return "sta";
        case F::Intellect: return "int";
        case F::Spirit:    return "spi";
        default:           return "?";
    }
}

bool saveOrError(const wowee::pipeline::WoweeCombatFormulas& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeCombatFormulasLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wcfr\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeCombatFormulas& c,
                     const std::string& base) {
    std::printf("Wrote %s.wcfr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  formulas: %zu\n", c.entries.size());
}

int handleGenWarrior(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WarriorCombatFormulas";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcfrExt(base);
    auto c = wowee::pipeline::WoweeCombatFormulasLoader::
        makeWarriorFormulas(name);
    if (!saveOrError(c, base, "gen-cfr-warrior")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMage(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MageCombatFormulas";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcfrExt(base);
    auto c = wowee::pipeline::WoweeCombatFormulasLoader::
        makeMageFormulas(name);
    if (!saveOrError(c, base, "gen-cfr-mage")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRogue(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RogueCombatFormulas";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcfrExt(base);
    auto c = wowee::pipeline::WoweeCombatFormulasLoader::
        makeRogueFormulas(name);
    if (!saveOrError(c, base, "gen-cfr-rogue")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcfrExt(base);
    if (!wowee::pipeline::WoweeCombatFormulasLoader::exists(base)) {
        std::fprintf(stderr, "WCFR not found: %s.wcfr\n",
                     base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCombatFormulasLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wcfr"] = base + ".wcfr";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"formulaId", e.formulaId},
                {"name", e.name},
                {"outputStatKind", e.outputStatKind},
                {"outputStatKindName",
                    outputStatKindName(e.outputStatKind)},
                {"inputStatKind", e.inputStatKind},
                {"inputStatKindName",
                    inputStatKindName(e.inputStatKind)},
                {"levelMin", e.levelMin},
                {"levelMax", e.levelMax},
                {"classRestriction", e.classRestriction},
                {"conversionRatioFp_x100",
                    e.conversionRatioFp_x100},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WCFR: %s.wcfr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  formulas: %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id  output     input  classBits  lvlRange  ratio (x100)  name\n");
    for (const auto& e : c.entries) {
        char lvlBuf[16];
        if (e.levelMax == 0) {
            std::snprintf(lvlBuf, sizeof(lvlBuf),
                          "%3u..*", e.levelMin);
        } else {
            std::snprintf(lvlBuf, sizeof(lvlBuf),
                          "%3u..%-2u", e.levelMin,
                          e.levelMax);
        }
        std::printf("  %4u  %-9s  %-3s    0x%04X     %-8s  %12u  %s\n",
                    e.formulaId,
                    outputStatKindName(e.outputStatKind),
                    inputStatKindName(e.inputStatKind),
                    e.classRestriction,
                    lvlBuf,
                    e.conversionRatioFp_x100,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcfrExt(base);
    if (!wowee::pipeline::WoweeCombatFormulasLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wcfr: WCFR not found: %s.wcfr\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCombatFormulasLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    using Quad = std::tuple<uint8_t, uint8_t, uint16_t, uint8_t>;
    std::set<Quad> applicabilityKeys;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.formulaId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.formulaId == 0)
            errors.push_back(ctx + ": formulaId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.outputStatKind > 7) {
            errors.push_back(ctx + ": outputStatKind " +
                std::to_string(e.outputStatKind) +
                " out of range (0..7)");
        }
        if (e.inputStatKind > 4) {
            errors.push_back(ctx + ": inputStatKind " +
                std::to_string(e.inputStatKind) +
                " out of range (0..4)");
        }
        // Conversion ratio = 0 means input stat
        // never produces any output — a no-op
        // formula. Almost certainly a typo.
        if (e.conversionRatioFp_x100 == 0) {
            errors.push_back(ctx +
                ": conversionRatioFp_x100 is 0 — "
                "input stat never produces output "
                "(no-op formula)");
        }
        if (e.levelMax > 0 && e.levelMax < e.levelMin) {
            errors.push_back(ctx + ": levelMax=" +
                std::to_string(e.levelMax) +
                " < levelMin=" +
                std::to_string(e.levelMin));
        }
        // Ratio > 100x (= 10000 fp_x100) is almost
        // certainly a units-mismatch typo (e.g.
        // forgot to divide by 100 when porting from
        // a percentage table).
        if (e.conversionRatioFp_x100 > 10000) {
            warnings.push_back(ctx +
                ": conversionRatioFp_x100=" +
                std::to_string(e.conversionRatioFp_x100) +
                " (= " +
                std::to_string(e.conversionRatioFp_x100 /
                                100.0) +
                "x) — exceeds 100x ratio; likely a "
                "units-mismatch typo (forgot to divide "
                "by 100?)");
        }
        // Per-(output, input, class, level-band)
        // applicability: walk class bitmask to detect
        // overlapping coverage. A simpler (and more
        // useful) check: same (output, input,
        // classRestriction, levelMin) quad seen twice
        // = duplicate at same scope. When two
        // formulas overlap at the same scope, the
        // runtime stat-compute would apply BOTH (or
        // pick one ambiguously).
        Quad key{e.outputStatKind, e.inputStatKind,
                  e.classRestriction, e.levelMin};
        if (!applicabilityKeys.insert(key).second) {
            errors.push_back(ctx +
                ": duplicate (output=" +
                std::string(outputStatKindName(e.outputStatKind))
                + ", input=" +
                std::string(inputStatKindName(e.inputStatKind))
                + ", classMask=0x" +
                std::to_string(e.classRestriction) +
                ", levelMin=" +
                std::to_string(e.levelMin) +
                ") — runtime stat-compute would apply "
                "both formulas, doubling the contribution");
        }
        if (!idsSeen.insert(e.formulaId).second) {
            errors.push_back(ctx + ": duplicate formulaId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wcfr"] = base + ".wcfr";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wcfr: %s.wcfr\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu formulas, all formulaIds + "
                    "(output,input,classMask,levelMin) quads "
                    "unique, outputStatKind 0..7, "
                    "inputStatKind 0..4, conversionRatio > 0, "
                    "valid level range\n",
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

bool handleCombatFormulasCatalog(int& i, int argc, char** argv,
                                   int& outRc) {
    if (std::strcmp(argv[i], "--gen-cfr-warrior") == 0 &&
        i + 1 < argc) {
        outRc = handleGenWarrior(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cfr-mage") == 0 &&
        i + 1 < argc) {
        outRc = handleGenMage(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cfr-rogue") == 0 &&
        i + 1 < argc) {
        outRc = handleGenRogue(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wcfr") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wcfr") == 0 &&
        i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
