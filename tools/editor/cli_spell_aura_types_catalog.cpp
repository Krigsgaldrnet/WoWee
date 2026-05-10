#include "cli_spell_aura_types_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_spell_aura_types.hpp"
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

std::string stripWaurExt(std::string base) {
    stripExt(base, ".waur");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeSpellAuraType& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSpellAuraTypeLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.waur\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSpellAuraType& c,
                     const std::string& base) {
    std::printf("Wrote %s.waur\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  auras   : %zu\n", c.entries.size());
}

int handleGenPeriodic(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "PeriodicAuras";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWaurExt(base);
    auto c = wowee::pipeline::WoweeSpellAuraTypeLoader::makePeriodic(name);
    if (!saveOrError(c, base, "gen-aur")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenStatMod(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StatModAuras";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWaurExt(base);
    auto c = wowee::pipeline::WoweeSpellAuraTypeLoader::makeStatMod(name);
    if (!saveOrError(c, base, "gen-aur-stats")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMovement(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MovementCCAuras";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWaurExt(base);
    auto c = wowee::pipeline::WoweeSpellAuraTypeLoader::makeMovement(name);
    if (!saveOrError(c, base, "gen-aur-movement")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWaurExt(base);
    if (!wowee::pipeline::WoweeSpellAuraTypeLoader::exists(base)) {
        std::fprintf(stderr, "WAUR not found: %s.waur\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellAuraTypeLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["waur"] = base + ".waur";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"auraTypeId", e.auraTypeId},
                {"name", e.name},
                {"description", e.description},
                {"auraKind", e.auraKind},
                {"auraKindName", wowee::pipeline::WoweeSpellAuraType::auraKindName(e.auraKind)},
                {"targetingHint", e.targetingHint},
                {"targetingHintName", wowee::pipeline::WoweeSpellAuraType::targetingHintName(e.targetingHint)},
                {"isStackable", e.isStackable != 0},
                {"maxStackCount", e.maxStackCount},
                {"updateFrequencyMs", e.updateFrequencyMs},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WAUR: %s.waur\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  auras   : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind         target       stack   max    tickMs   name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-10s   %-10s    %s     %3u   %6u    %s\n",
                    e.auraTypeId,
                    wowee::pipeline::WoweeSpellAuraType::auraKindName(e.auraKind),
                    wowee::pipeline::WoweeSpellAuraType::targetingHintName(e.targetingHint),
                    e.isStackable ? "yes" : "no ",
                    e.maxStackCount, e.updateFrequencyMs,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWaurExt(base);
    if (!wowee::pipeline::WoweeSpellAuraTypeLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-waur: WAUR not found: %s.waur\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellAuraTypeLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.auraTypeId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.auraKind > wowee::pipeline::WoweeSpellAuraType::Misc) {
            errors.push_back(ctx + ": auraKind " +
                std::to_string(e.auraKind) + " not in 0..8");
        }
        if (e.targetingHint > wowee::pipeline::WoweeSpellAuraType::BeneficialOnly) {
            errors.push_back(ctx + ": targetingHint " +
                std::to_string(e.targetingHint) + " not in 0..3");
        }
        // Periodic kind requires updateFrequencyMs > 0.
        if (e.auraKind == wowee::pipeline::WoweeSpellAuraType::Periodic &&
            e.updateFrequencyMs == 0) {
            errors.push_back(ctx +
                ": Periodic kind requires updateFrequencyMs > 0 "
                "— aura would never tick");
        }
        // Non-periodic with updateFrequencyMs > 0 is suspicious.
        if (e.auraKind != wowee::pipeline::WoweeSpellAuraType::Periodic &&
            e.auraKind != wowee::pipeline::WoweeSpellAuraType::Trigger &&
            e.updateFrequencyMs != 0) {
            warnings.push_back(ctx +
                ": " +
                wowee::pipeline::WoweeSpellAuraType::auraKindName(e.auraKind) +
                " kind with updateFrequencyMs=" +
                std::to_string(e.updateFrequencyMs) +
                " — non-periodic auras don't tick, engine ignores");
        }
        // maxStackCount > 0 with isStackable=0 is contradictory.
        if (e.isStackable == 0 && e.maxStackCount > 0) {
            warnings.push_back(ctx +
                ": maxStackCount=" + std::to_string(e.maxStackCount) +
                " set but isStackable=false — stack cap is "
                "unreachable");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.auraTypeId) {
                errors.push_back(ctx + ": duplicate auraTypeId");
                break;
            }
        }
        idsSeen.push_back(e.auraTypeId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["waur"] = base + ".waur";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-waur: %s.waur\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu auras, all auraTypeIds unique\n",
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

bool handleSpellAuraTypesCatalog(int& i, int argc, char** argv,
                                 int& outRc) {
    if (std::strcmp(argv[i], "--gen-aur") == 0 && i + 1 < argc) {
        outRc = handleGenPeriodic(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-aur-stats") == 0 && i + 1 < argc) {
        outRc = handleGenStatMod(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-aur-movement") == 0 && i + 1 < argc) {
        outRc = handleGenMovement(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-waur") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-waur") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
