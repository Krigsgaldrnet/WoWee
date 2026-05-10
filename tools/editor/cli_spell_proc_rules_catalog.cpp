#include "cli_spell_proc_rules_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_spell_proc_rules.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWprcExt(std::string base) {
    stripExt(base, ".wprc");
    return base;
}

const char* triggerEventName(uint8_t e) {
    using P = wowee::pipeline::WoweeSpellProcRules;
    switch (e) {
        case P::OnHit:        return "onhit";
        case P::OnCrit:       return "oncrit";
        case P::OnCast:       return "oncast";
        case P::OnTakeDamage: return "ontakedamage";
        case P::OnHeal:       return "onheal";
        case P::OnDodge:      return "ondodge";
        case P::OnParry:      return "onparry";
        case P::OnBlock:      return "onblock";
        case P::OnKill:       return "onkill";
        default:              return "?";
    }
}

bool saveOrError(const wowee::pipeline::WoweeSpellProcRules& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSpellProcRulesLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wprc\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSpellProcRules& c,
                     const std::string& base) {
    std::printf("Wrote %s.wprc\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  procs   : %zu\n", c.entries.size());
}

int handleGenWeapon(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WeaponEnchantProcs";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWprcExt(base);
    auto c = wowee::pipeline::WoweeSpellProcRulesLoader::
        makeWeaponProcs(name);
    if (!saveOrError(c, base, "gen-prc-weapon")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRet(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RetributionPaladinProcs";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWprcExt(base);
    auto c = wowee::pipeline::WoweeSpellProcRulesLoader::
        makeRetPaladin(name);
    if (!saveOrError(c, base, "gen-prc-ret")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRage(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RageGenerationProcs";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWprcExt(base);
    auto c = wowee::pipeline::WoweeSpellProcRulesLoader::
        makeRageGen(name);
    if (!saveOrError(c, base, "gen-prc-rage")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWprcExt(base);
    if (!wowee::pipeline::WoweeSpellProcRulesLoader::exists(base)) {
        std::fprintf(stderr, "WPRC not found: %s.wprc\n",
                     base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellProcRulesLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wprc"] = base + ".wprc";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"procRuleId", e.procRuleId},
                {"name", e.name},
                {"sourceSpellId", e.sourceSpellId},
                {"procEffectSpellId", e.procEffectSpellId},
                {"triggerEvent", e.triggerEvent},
                {"triggerEventName",
                    triggerEventName(e.triggerEvent)},
                {"maxStacksOnTarget", e.maxStacksOnTarget},
                {"procChancePct", e.procChancePct},
                {"internalCooldownMs", e.internalCooldownMs},
                {"procFlagsMask", e.procFlagsMask},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WPRC: %s.wprc\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  procs   : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   srcSpell  effSpell  event           chance  ICDms     stacks  flags  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %7u  %8u  %-13s   %5u   %6u   %5u  0x%04X  %s\n",
                    e.procRuleId, e.sourceSpellId,
                    e.procEffectSpellId,
                    triggerEventName(e.triggerEvent),
                    e.procChancePct, e.internalCooldownMs,
                    e.maxStacksOnTarget,
                    e.procFlagsMask, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWprcExt(base);
    if (!wowee::pipeline::WoweeSpellProcRulesLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wprc: WPRC not found: %s.wprc\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellProcRulesLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.procRuleId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.procRuleId == 0)
            errors.push_back(ctx + ": procRuleId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.sourceSpellId == 0)
            errors.push_back(ctx +
                ": sourceSpellId is 0 — proc has no "
                "owning aura");
        if (e.procEffectSpellId == 0)
            errors.push_back(ctx +
                ": procEffectSpellId is 0 — proc has "
                "nothing to trigger");
        if (e.triggerEvent > 8) {
            errors.push_back(ctx + ": triggerEvent " +
                std::to_string(e.triggerEvent) +
                " out of range (0..8)");
        }
        if (e.procChancePct == 0) {
            errors.push_back(ctx +
                ": procChancePct is 0 — proc never "
                "fires");
        }
        if (e.procChancePct > 10000) {
            errors.push_back(ctx +
                ": procChancePct " +
                std::to_string(e.procChancePct) +
                " exceeds 10000 (100% in basis points)");
        }
        // Self-proc on OnCast is the most dangerous
        // case — sourceSpellId == procEffectSpellId
        // with OnCast trigger could create an infinite
        // proc loop where the effect spell triggers
        // its own re-cast. Other triggers are usually
        // safe (OnHit/OnCrit don't fire on the proc
        // spell itself unless that spell hits).
        using P = wowee::pipeline::WoweeSpellProcRules;
        if (e.sourceSpellId != 0 &&
            e.sourceSpellId == e.procEffectSpellId &&
            e.triggerEvent == P::OnCast) {
            errors.push_back(ctx +
                ": sourceSpellId == procEffectSpellId="
                + std::to_string(e.sourceSpellId) +
                " on OnCast trigger — infinite proc "
                "loop (effect re-casts itself)");
        }
        // 100% proc chance with 0 ICD on OnHit/OnCrit
        // = every-melee-swing spam — almost certainly
        // unintended performance footgun. Warn unless
        // it's an OnCast bookkeeping rule (those are
        // intentional).
        if (e.procChancePct == 10000 &&
            e.internalCooldownMs == 0 &&
            (e.triggerEvent == P::OnHit ||
             e.triggerEvent == P::OnCrit ||
             e.triggerEvent == P::OnTakeDamage)) {
            warnings.push_back(ctx +
                ": 100% proc chance + 0ms ICD on "
                "high-frequency event (" +
                std::string(triggerEventName(e.triggerEvent)) +
                ") — would spam every swing; verify "
                "intentional or add an ICD");
        }
        if (!idsSeen.insert(e.procRuleId).second) {
            errors.push_back(ctx + ": duplicate procRuleId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wprc"] = base + ".wprc";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wprc: %s.wprc\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu procs, all procRuleIds "
                    "unique, sourceSpellId+procEffectSpellId "
                    "non-zero, triggerEvent 0..8, "
                    "procChancePct 1..10000, no infinite "
                    "self-proc loop on OnCast\n",
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

bool handleSpellProcRulesCatalog(int& i, int argc, char** argv,
                                   int& outRc) {
    if (std::strcmp(argv[i], "--gen-prc-weapon") == 0 &&
        i + 1 < argc) {
        outRc = handleGenWeapon(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-prc-ret") == 0 &&
        i + 1 < argc) {
        outRc = handleGenRet(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-prc-rage") == 0 &&
        i + 1 < argc) {
        outRc = handleGenRage(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wprc") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wprc") == 0 &&
        i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
