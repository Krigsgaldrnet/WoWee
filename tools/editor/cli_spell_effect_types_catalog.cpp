#include "cli_spell_effect_types_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_spell_effect_types.hpp"
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

std::string stripWsefExt(std::string base) {
    stripExt(base, ".wsef");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeSpellEffectType& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSpellEffectTypeLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wsef\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSpellEffectType& c,
                     const std::string& base) {
    std::printf("Wrote %s.wsef\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  effects : %zu\n", c.entries.size());
}

int handleGenDamage(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "DamageEffects";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsefExt(base);
    auto c = wowee::pipeline::WoweeSpellEffectTypeLoader::makeDamage(name);
    if (!saveOrError(c, base, "gen-sef")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenHealing(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "HealingEffects";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsefExt(base);
    auto c = wowee::pipeline::WoweeSpellEffectTypeLoader::makeHealing(name);
    if (!saveOrError(c, base, "gen-sef-healing")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenAura(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AuraEffects";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsefExt(base);
    auto c = wowee::pipeline::WoweeSpellEffectTypeLoader::makeAura(name);
    if (!saveOrError(c, base, "gen-sef-aura")) return 1;
    printGenSummary(c, base);
    return 0;
}

void appendBehaviorFlagNames(uint8_t flags, std::string& out) {
    using F = wowee::pipeline::WoweeSpellEffectType;
    auto add = [&](const char* n) {
        if (!out.empty()) out += "|";
        out += n;
    };
    if (flags & F::RequiresTarget)      add("RequiresTarget");
    if (flags & F::RequiresLineOfSight) add("RequiresLineOfSight");
    if (flags & F::IsHostileEffect)     add("IsHostileEffect");
    if (flags & F::IsBeneficialEffect)  add("IsBeneficialEffect");
    if (flags & F::IgnoresImmunities)   add("IgnoresImmunities");
    if (flags & F::TriggersGCD)         add("TriggersGCD");
    if (out.empty()) out = "-";
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsefExt(base);
    if (!wowee::pipeline::WoweeSpellEffectTypeLoader::exists(base)) {
        std::fprintf(stderr, "WSEF not found: %s.wsef\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellEffectTypeLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wsef"] = base + ".wsef";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            std::string flagNames;
            appendBehaviorFlagNames(e.behaviorFlags, flagNames);
            arr.push_back({
                {"effectId", e.effectId},
                {"name", e.name},
                {"description", e.description},
                {"effectKind", e.effectKind},
                {"effectKindName", wowee::pipeline::WoweeSpellEffectType::effectKindName(e.effectKind)},
                {"behaviorFlags", e.behaviorFlags},
                {"behaviorFlagsLabels", flagNames},
                {"baseAmount", e.baseAmount},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSEF: %s.wsef\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  effects : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind       baseAmt   flags                                                name\n");
    for (const auto& e : c.entries) {
        std::string flagNames;
        appendBehaviorFlagNames(e.behaviorFlags, flagNames);
        std::printf("  %4u   %-9s  %7d   %-50s   %s\n",
                    e.effectId,
                    wowee::pipeline::WoweeSpellEffectType::effectKindName(e.effectKind),
                    e.baseAmount,
                    flagNames.c_str(),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsefExt(base);
    if (!wowee::pipeline::WoweeSpellEffectTypeLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wsef: WSEF not found: %s.wsef\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellEffectTypeLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    constexpr uint8_t kKnownFlagMask =
        wowee::pipeline::WoweeSpellEffectType::RequiresTarget |
        wowee::pipeline::WoweeSpellEffectType::RequiresLineOfSight |
        wowee::pipeline::WoweeSpellEffectType::IsHostileEffect |
        wowee::pipeline::WoweeSpellEffectType::IsBeneficialEffect |
        wowee::pipeline::WoweeSpellEffectType::IgnoresImmunities |
        wowee::pipeline::WoweeSpellEffectType::TriggersGCD;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.effectId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.effectKind > wowee::pipeline::WoweeSpellEffectType::Misc) {
            errors.push_back(ctx + ": effectKind " +
                std::to_string(e.effectKind) + " not in 0..9");
        }
        if (e.behaviorFlags & ~kKnownFlagMask) {
            warnings.push_back(ctx +
                ": behaviorFlags has bits outside known mask " +
                "(0x" + std::to_string(e.behaviorFlags & ~kKnownFlagMask) +
                ") — engine will ignore unknown flags");
        }
        // Both Hostile and Beneficial set is contradictory.
        if ((e.behaviorFlags & wowee::pipeline::WoweeSpellEffectType::IsHostileEffect) &&
            (e.behaviorFlags & wowee::pipeline::WoweeSpellEffectType::IsBeneficialEffect)) {
            warnings.push_back(ctx +
                ": both IsHostileEffect and IsBeneficialEffect "
                "flags set — engine treats this as Hostile (flag "
                "wins) but the contradiction suggests a config bug");
        }
        // Damage kind without TriggersGCD is unusual.
        if (e.effectKind == wowee::pipeline::WoweeSpellEffectType::Damage &&
            !(e.behaviorFlags & wowee::pipeline::WoweeSpellEffectType::TriggersGCD) &&
            e.effectId != 13) {  // EnvironmentalDamage doesn't trigger GCD
            warnings.push_back(ctx +
                ": Damage kind without TriggersGCD — most damage "
                "effects should be on the GCD; double-check this "
                "is intentional");
        }
        // Heal kind without IsBeneficialEffect is suspicious.
        if (e.effectKind == wowee::pipeline::WoweeSpellEffectType::Heal &&
            !(e.behaviorFlags & wowee::pipeline::WoweeSpellEffectType::IsBeneficialEffect)) {
            warnings.push_back(ctx +
                ": Heal kind without IsBeneficialEffect — "
                "engine treats heals as ungated, may damage enemies");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.effectId) {
                errors.push_back(ctx + ": duplicate effectId");
                break;
            }
        }
        idsSeen.push_back(e.effectId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wsef"] = base + ".wsef";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wsef: %s.wsef\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu effects, all effectIds unique\n",
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

bool handleSpellEffectTypesCatalog(int& i, int argc, char** argv,
                                   int& outRc) {
    if (std::strcmp(argv[i], "--gen-sef") == 0 && i + 1 < argc) {
        outRc = handleGenDamage(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-sef-healing") == 0 && i + 1 < argc) {
        outRc = handleGenHealing(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-sef-aura") == 0 && i + 1 < argc) {
        outRc = handleGenAura(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wsef") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wsef") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
