#include "cli_pets_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_pets.hpp"
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

std::string stripWpetExt(std::string base) {
    stripExt(base, ".wpet");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweePet& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweePetLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wpet\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweePet& c,
                     const std::string& base) {
    std::printf("Wrote %s.wpet\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  families : %zu  minions : %zu\n",
                c.families.size(), c.minions.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterPets";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpetExt(base);
    auto c = wowee::pipeline::WoweePetLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-pets")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenHunter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "HunterPets";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpetExt(base);
    auto c = wowee::pipeline::WoweePetLoader::makeHunter(name);
    if (!saveOrError(c, base, "gen-pets-hunter")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenWarlock(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WarlockMinions";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpetExt(base);
    auto c = wowee::pipeline::WoweePetLoader::makeWarlock(name);
    if (!saveOrError(c, base, "gen-pets-warlock")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWpetExt(base);
    if (!wowee::pipeline::WoweePetLoader::exists(base)) {
        std::fprintf(stderr, "WPET not found: %s.wpet\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweePetLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wpet"] = base + ".wpet";
        j["name"] = c.name;
        j["familyCount"] = c.families.size();
        j["minionCount"] = c.minions.size();
        nlohmann::json fa = nlohmann::json::array();
        for (const auto& f : c.families) {
            nlohmann::json jf;
            jf["familyId"] = f.familyId;
            jf["name"] = f.name;
            jf["description"] = f.description;
            jf["iconPath"] = f.iconPath;
            jf["petType"] = f.petType;
            jf["petTypeName"] = wowee::pipeline::WoweePet::petTypeName(f.petType);
            jf["baseAttackSpeed"] = f.baseAttackSpeed;
            jf["damageMultiplier"] = f.damageMultiplier;
            jf["armorMultiplier"] = f.armorMultiplier;
            jf["dietMask"] = f.dietMask;
            jf["dietMaskName"] = wowee::pipeline::WoweePet::dietMaskName(f.dietMask);
            nlohmann::json abs = nlohmann::json::array();
            for (const auto& a : f.abilities) {
                abs.push_back({
                    {"spellId", a.spellId},
                    {"learnedAtLevel", a.learnedAtLevel},
                    {"rank", a.rank},
                });
            }
            jf["abilities"] = abs;
            fa.push_back(jf);
        }
        j["families"] = fa;
        nlohmann::json ma = nlohmann::json::array();
        for (const auto& m : c.minions) {
            nlohmann::json jm;
            jm["minionId"] = m.minionId;
            jm["name"] = m.name;
            jm["summonSpellId"] = m.summonSpellId;
            jm["creatureId"] = m.creatureId;
            nlohmann::json abs = nlohmann::json::array();
            for (const auto& a : m.abilities) {
                abs.push_back({
                    {"spellId", a.spellId},
                    {"rank", a.rank},
                    {"autocastDefault", a.autocastDefault},
                });
            }
            jm["abilities"] = abs;
            ma.push_back(jm);
        }
        j["minions"] = ma;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WPET: %s.wpet\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  families : %zu  minions : %zu\n",
                c.families.size(), c.minions.size());
    if (!c.families.empty()) {
        std::printf("\n  Families:\n");
        std::printf("    id   type      atkSpd  dmg    arm    diet           name\n");
        for (const auto& f : c.families) {
            std::printf("  %4u   %-8s  %4.1f   %4.2f  %4.2f  %-13s  %s (%zu abilities)\n",
                        f.familyId,
                        wowee::pipeline::WoweePet::petTypeName(f.petType),
                        f.baseAttackSpeed, f.damageMultiplier,
                        f.armorMultiplier,
                        wowee::pipeline::WoweePet::dietMaskName(f.dietMask).c_str(),
                        f.name.c_str(), f.abilities.size());
        }
    }
    if (!c.minions.empty()) {
        std::printf("\n  Minions:\n");
        std::printf("    id   summonSpell  creatureId  name\n");
        for (const auto& m : c.minions) {
            std::printf("  %4u   %5u        %5u      %s (%zu abilities)\n",
                        m.minionId, m.summonSpellId, m.creatureId,
                        m.name.c_str(), m.abilities.size());
        }
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWpetExt(base);
    if (!wowee::pipeline::WoweePetLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wpet: WPET not found: %s.wpet\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweePetLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.families.empty() && c.minions.empty()) {
        warnings.push_back("catalog has zero families and zero minions");
    }
    std::vector<uint32_t> famIds;
    for (size_t k = 0; k < c.families.size(); ++k) {
        const auto& f = c.families[k];
        std::string ctx = "family " + std::to_string(k) +
                          " (id=" + std::to_string(f.familyId);
        if (!f.name.empty()) ctx += " " + f.name;
        ctx += ")";
        if (f.familyId == 0) errors.push_back(ctx + ": familyId is 0");
        if (f.name.empty()) errors.push_back(ctx + ": name is empty");
        if (f.petType > wowee::pipeline::WoweePet::Tenacity) {
            errors.push_back(ctx + ": petType " +
                std::to_string(f.petType) + " not in 0..2");
        }
        if (f.baseAttackSpeed <= 0) {
            errors.push_back(ctx + ": baseAttackSpeed must be > 0");
        }
        if (f.dietMask == 0) {
            warnings.push_back(ctx +
                ": dietMask=0 (pet cannot be fed for happiness)");
        }
        for (uint32_t prev : famIds) {
            if (prev == f.familyId) {
                errors.push_back(ctx + ": duplicate familyId");
                break;
            }
        }
        famIds.push_back(f.familyId);
    }
    std::vector<uint32_t> minIds;
    for (size_t k = 0; k < c.minions.size(); ++k) {
        const auto& m = c.minions[k];
        std::string ctx = "minion " + std::to_string(k) +
                          " (id=" + std::to_string(m.minionId);
        if (!m.name.empty()) ctx += " " + m.name;
        ctx += ")";
        if (m.minionId == 0) errors.push_back(ctx + ": minionId is 0");
        if (m.name.empty()) errors.push_back(ctx + ": name is empty");
        if (m.summonSpellId == 0) {
            errors.push_back(ctx + ": summonSpellId is 0 (cannot summon)");
        }
        if (m.creatureId == 0) {
            errors.push_back(ctx +
                ": creatureId is 0 (no WCRT template for stats)");
        }
        for (uint32_t prev : minIds) {
            if (prev == m.minionId) {
                errors.push_back(ctx + ": duplicate minionId");
                break;
            }
        }
        minIds.push_back(m.minionId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wpet"] = base + ".wpet";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wpet: %s.wpet\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu families, %zu minions, all IDs unique\n",
                    c.families.size(), c.minions.size());
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

bool handlePetsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-pets") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-pets-hunter") == 0 && i + 1 < argc) {
        outRc = handleGenHunter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-pets-warlock") == 0 && i + 1 < argc) {
        outRc = handleGenWarlock(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wpet") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wpet") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
