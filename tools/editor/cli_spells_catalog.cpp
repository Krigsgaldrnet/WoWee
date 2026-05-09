#include "cli_spells_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_spells.hpp"
#include <nlohmann/json.hpp>

#include <cmath>
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

std::string stripWsplExt(std::string base) {
    stripExt(base, ".wspl");
    return base;
}

void appendSpellFlagsStr(std::string& s, uint32_t flags) {
    if (flags & wowee::pipeline::WoweeSpell::Passive)        s += "passive ";
    if (flags & wowee::pipeline::WoweeSpell::Hidden)         s += "hidden ";
    if (flags & wowee::pipeline::WoweeSpell::Channeled)      s += "channeled ";
    if (flags & wowee::pipeline::WoweeSpell::Ranged)         s += "ranged ";
    if (flags & wowee::pipeline::WoweeSpell::AreaOfEffect)   s += "aoe ";
    if (flags & wowee::pipeline::WoweeSpell::Triggered)      s += "triggered ";
    if (flags & wowee::pipeline::WoweeSpell::UnitTargetOnly) s += "unit-only ";
    if (flags & wowee::pipeline::WoweeSpell::FriendlyOnly)   s += "friendly ";
    if (flags & wowee::pipeline::WoweeSpell::HostileOnly)    s += "hostile ";
    if (s.empty()) s = "-";
    else if (s.back() == ' ') s.pop_back();
}

bool saveOrError(const wowee::pipeline::WoweeSpell& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSpellLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wspl\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSpell& c,
                     const std::string& base) {
    std::printf("Wrote %s.wspl\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  spells  : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterSpells";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsplExt(base);
    auto c = wowee::pipeline::WoweeSpellLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-spells")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMage(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MageSpells";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsplExt(base);
    auto c = wowee::pipeline::WoweeSpellLoader::makeMage(name);
    if (!saveOrError(c, base, "gen-spells-mage")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenWarrior(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WarriorSpells";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsplExt(base);
    auto c = wowee::pipeline::WoweeSpellLoader::makeWarrior(name);
    if (!saveOrError(c, base, "gen-spells-warrior")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsplExt(base);
    if (!wowee::pipeline::WoweeSpellLoader::exists(base)) {
        std::fprintf(stderr, "WSPL not found: %s.wspl\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wspl"] = base + ".wspl";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            std::string fs;
            appendSpellFlagsStr(fs, e.flags);
            arr.push_back({
                {"spellId", e.spellId},
                {"name", e.name},
                {"description", e.description},
                {"iconPath", e.iconPath},
                {"school", e.school},
                {"schoolName", wowee::pipeline::WoweeSpell::schoolName(e.school)},
                {"targetType", e.targetType},
                {"targetTypeName", wowee::pipeline::WoweeSpell::targetTypeName(e.targetType)},
                {"effectKind", e.effectKind},
                {"effectKindName", wowee::pipeline::WoweeSpell::effectKindName(e.effectKind)},
                {"castTimeMs", e.castTimeMs},
                {"cooldownMs", e.cooldownMs},
                {"gcdMs", e.gcdMs},
                {"manaCost", e.manaCost},
                {"rangeMin", e.rangeMin},
                {"rangeMax", e.rangeMax},
                {"minLevel", e.minLevel},
                {"maxStacks", e.maxStacks},
                {"durationMs", e.durationMs},
                {"effectValueMin", e.effectValueMin},
                {"effectValueMax", e.effectValueMax},
                {"effectMisc", e.effectMisc},
                {"flags", e.flags},
                {"flagsStr", fs},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSPL: %s.wspl\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  spells  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id      school    effect     cast  cd   mana  range   value     name\n");
    for (const auto& e : c.entries) {
        std::printf("  %5u   %-8s  %-9s  %4ums  %4us  %3u  %4.0f-%-4.0f  %4d-%-4d  %s\n",
                    e.spellId,
                    wowee::pipeline::WoweeSpell::schoolName(e.school),
                    wowee::pipeline::WoweeSpell::effectKindName(e.effectKind),
                    e.castTimeMs, e.cooldownMs / 1000, e.manaCost,
                    e.rangeMin, e.rangeMax,
                    e.effectValueMin, e.effectValueMax,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsplExt(base);
    if (!wowee::pipeline::WoweeSpellLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wspl: WSPL not found: %s.wspl\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    idsSeen.reserve(c.entries.size());
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.spellId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.spellId == 0) {
            errors.push_back(ctx + ": spellId is 0");
        }
        if (e.name.empty()) {
            errors.push_back(ctx + ": name is empty");
        }
        if (e.school > wowee::pipeline::WoweeSpell::SchoolArcane) {
            errors.push_back(ctx + ": school " +
                std::to_string(e.school) + " not in 0..6");
        }
        if (e.effectKind > wowee::pipeline::WoweeSpell::EffectDispel) {
            errors.push_back(ctx + ": effectKind " +
                std::to_string(e.effectKind) + " not in 0..6");
        }
        if (!std::isfinite(e.rangeMin) || !std::isfinite(e.rangeMax)) {
            errors.push_back(ctx + ": rangeMin/Max not finite");
        }
        if (e.rangeMin > e.rangeMax) {
            errors.push_back(ctx + ": rangeMin > rangeMax");
        }
        if (e.effectValueMin > e.effectValueMax) {
            errors.push_back(ctx + ": effectValueMin > effectValueMax");
        }
        // Friendly + Hostile target restrictions are mutually exclusive.
        if ((e.flags & wowee::pipeline::WoweeSpell::FriendlyOnly) &&
            (e.flags & wowee::pipeline::WoweeSpell::HostileOnly)) {
            errors.push_back(ctx +
                ": FriendlyOnly and HostileOnly both set (incoherent)");
        }
        // Damage / debuff effects on a friendly-only spell don't make sense.
        if ((e.flags & wowee::pipeline::WoweeSpell::FriendlyOnly) &&
            (e.effectKind == wowee::pipeline::WoweeSpell::EffectDamage ||
             e.effectKind == wowee::pipeline::WoweeSpell::EffectDebuff)) {
            warnings.push_back(ctx +
                ": friendly-only spell with damage/debuff effect");
        }
        // Heal / buff on a hostile-only spell is incoherent.
        if ((e.flags & wowee::pipeline::WoweeSpell::HostileOnly) &&
            (e.effectKind == wowee::pipeline::WoweeSpell::EffectHeal ||
             e.effectKind == wowee::pipeline::WoweeSpell::EffectBuff)) {
            warnings.push_back(ctx +
                ": hostile-only spell with heal/buff effect");
        }
        // Buff / debuff effects need a non-zero duration to mean anything.
        if ((e.effectKind == wowee::pipeline::WoweeSpell::EffectBuff ||
             e.effectKind == wowee::pipeline::WoweeSpell::EffectDebuff) &&
            e.durationMs == 0) {
            warnings.push_back(ctx +
                ": buff/debuff effect with durationMs=0 (instant fade)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.spellId) {
                errors.push_back(ctx + ": duplicate spellId");
                break;
            }
        }
        idsSeen.push_back(e.spellId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wspl"] = base + ".wspl";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wspl: %s.wspl\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu spells, all spellIds unique\n",
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

bool handleSpellsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-spells") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-spells-mage") == 0 && i + 1 < argc) {
        outRc = handleGenMage(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-spells-warrior") == 0 && i + 1 < argc) {
        outRc = handleGenWarrior(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wspl") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wspl") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
