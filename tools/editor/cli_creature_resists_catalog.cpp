#include "cli_creature_resists_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_creature_resists.hpp"
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

std::string stripWcreExt(std::string base) {
    stripExt(base, ".wcre");
    return base;
}

std::string ccImmunityString(uint16_t mask) {
    using R = wowee::pipeline::WoweeCreatureResists;
    if (mask == 0) return "none";
    if (mask == 0xFFFF) return "all";
    std::string out;
    auto add = [&](const char* tag) {
        if (!out.empty()) out += "+";
        out += tag;
    };
    if (mask & R::ImmuneRoot)      add("root");
    if (mask & R::ImmuneSnare)     add("snare");
    if (mask & R::ImmuneStun)      add("stun");
    if (mask & R::ImmuneFear)      add("fear");
    if (mask & R::ImmuneSleep)     add("sleep");
    if (mask & R::ImmuneSilence)   add("silence");
    if (mask & R::ImmuneCharm)     add("charm");
    if (mask & R::ImmuneDisarm)    add("disarm");
    if (mask & R::ImmunePolymorph) add("polymorph");
    if (mask & R::ImmuneBanish)    add("banish");
    if (mask & R::ImmuneKnockback) add("knockback");
    if (mask & R::ImmuneInterrupt) add("interrupt");
    if (mask & R::ImmuneTaunt)     add("taunt");
    if (mask & R::ImmuneBleed)     add("bleed");
    return out.empty() ? "none" : out;
}

bool saveOrError(const wowee::pipeline::WoweeCreatureResists& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeCreatureResistsLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wcre\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeCreatureResists& c,
                     const std::string& base) {
    std::printf("Wrote %s.wcre\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  resists : %zu\n", c.entries.size());
}

int handleGenBosses(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RaidBossResists";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcreExt(base);
    auto c = wowee::pipeline::WoweeCreatureResistsLoader::makeRaidBosses(name);
    if (!saveOrError(c, base, "gen-cre")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenElites(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "EliteResists";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcreExt(base);
    auto c = wowee::pipeline::WoweeCreatureResistsLoader::makeElites(name);
    if (!saveOrError(c, base, "gen-cre-elites")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenImmunities(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "CCImmunityProfiles";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcreExt(base);
    auto c = wowee::pipeline::WoweeCreatureResistsLoader::makeImmunities(name);
    if (!saveOrError(c, base, "gen-cre-immune")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcreExt(base);
    if (!wowee::pipeline::WoweeCreatureResistsLoader::exists(base)) {
        std::fprintf(stderr, "WCRE not found: %s.wcre\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCreatureResistsLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wcre"] = base + ".wcre";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"resistId", e.resistId},
                {"name", e.name},
                {"description", e.description},
                {"creatureEntry", e.creatureEntry},
                {"holyResist", e.holyResist},
                {"fireResist", e.fireResist},
                {"natureResist", e.natureResist},
                {"frostResist", e.frostResist},
                {"shadowResist", e.shadowResist},
                {"arcaneResist", e.arcaneResist},
                {"physicalResistPct", e.physicalResistPct},
                {"ccImmunityMask", e.ccImmunityMask},
                {"ccImmunityNames", ccImmunityString(e.ccImmunityMask)},
                {"mechanicImmunityMask", e.mechanicImmunityMask},
                {"schoolImmunityMask", e.schoolImmunityMask},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WCRE: %s.wcre\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  resists : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   creature   holy fire natu fros shad arca   phys%%  schoolImm  CC-immune\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u    %5u   %4d %4d %4d %4d %4d %4d   %3u    0x%02X     %s\n",
                    e.resistId, e.creatureEntry,
                    e.holyResist, e.fireResist,
                    e.natureResist, e.frostResist,
                    e.shadowResist, e.arcaneResist,
                    e.physicalResistPct,
                    e.schoolImmunityMask,
                    ccImmunityString(e.ccImmunityMask).c_str());
        std::printf("           %s\n", e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcreExt(base);
    if (!wowee::pipeline::WoweeCreatureResistsLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wcre: WCRE not found: %s.wcre\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCreatureResistsLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    std::set<uint32_t> creaturesSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.resistId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.resistId == 0)
            errors.push_back(ctx + ": resistId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.creatureEntry == 0) {
            errors.push_back(ctx +
                ": creatureEntry is 0 — resist profile is "
                "not bound to any WCRT entry");
        }
        // Resist values: int16 covers -32768 to 32767.
        // 32767 is the special "full immunity" sentinel.
        // Negative values are unusual but legal (some DR
        // mechanics can negative-resist).
        auto checkResist = [&](int16_t v, const char* school) {
            if (v < -100) {
                warnings.push_back(ctx + ": " + school +
                    " resist " + std::to_string(v) +
                    " < -100 — extreme negative resist "
                    "creates >2x damage taken; verify");
            }
        };
        checkResist(e.holyResist,   "holy");
        checkResist(e.fireResist,   "fire");
        checkResist(e.natureResist, "nature");
        checkResist(e.frostResist,  "frost");
        checkResist(e.shadowResist, "shadow");
        checkResist(e.arcaneResist, "arcane");
        // physicalResistPct cap is 75% (game-engine
        // hard cap). Above that, the stored value is
        // clamped at runtime.
        if (e.physicalResistPct > 75) {
            warnings.push_back(ctx +
                ": physicalResistPct " +
                std::to_string(e.physicalResistPct) +
                " > 75%% — clamped at runtime to game-"
                "engine cap (armor mitigation cap)");
        }
        // schoolImmunityMask uses bottom 6 bits (one per
        // magic school). Bit 7 unused.
        if (e.schoolImmunityMask & 0xC0) {
            warnings.push_back(ctx +
                ": schoolImmunityMask has reserved bits "
                "set (0xC0) — only bits 0-5 (Holy/Fire/"
                "Nature/Frost/Shadow/Arcane) are meaningful");
        }
        // Multiple WCRE entries binding the same
        // creature is ambiguous (which profile applies?).
        if (e.creatureEntry != 0 &&
            !creaturesSeen.insert(e.creatureEntry).second) {
            errors.push_back(ctx +
                ": creatureEntry " +
                std::to_string(e.creatureEntry) +
                " is already bound by another resist "
                "profile — damage-calc lookup would be "
                "ambiguous");
        }
        if (!idsSeen.insert(e.resistId).second) {
            errors.push_back(ctx + ": duplicate resistId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wcre"] = base + ".wcre";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wcre: %s.wcre\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu resist profiles, all "
                    "resistIds + creatureEntries unique\n",
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

bool handleCreatureResistsCatalog(int& i, int argc, char** argv,
                                   int& outRc) {
    if (std::strcmp(argv[i], "--gen-cre") == 0 && i + 1 < argc) {
        outRc = handleGenBosses(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cre-elites") == 0 && i + 1 < argc) {
        outRc = handleGenElites(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cre-immune") == 0 && i + 1 < argc) {
        outRc = handleGenImmunities(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wcre") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wcre") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
