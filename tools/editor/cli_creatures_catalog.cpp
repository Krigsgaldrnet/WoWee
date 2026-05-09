#include "cli_creatures_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_creatures.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWcrtExt(std::string base) {
    stripExt(base, ".wcrt");
    return base;
}

void appendNpcFlagsStr(std::string& s, uint32_t flags) {
    if (flags & wowee::pipeline::WoweeCreature::Vendor)       s += "vendor ";
    if (flags & wowee::pipeline::WoweeCreature::QuestGiver)   s += "quest ";
    if (flags & wowee::pipeline::WoweeCreature::Trainer)      s += "trainer ";
    if (flags & wowee::pipeline::WoweeCreature::Banker)       s += "banker ";
    if (flags & wowee::pipeline::WoweeCreature::Innkeeper)    s += "innkeeper ";
    if (flags & wowee::pipeline::WoweeCreature::FlightMaster) s += "flight ";
    if (flags & wowee::pipeline::WoweeCreature::Auctioneer)   s += "auction ";
    if (flags & wowee::pipeline::WoweeCreature::Repair)       s += "repair ";
    if (flags & wowee::pipeline::WoweeCreature::Stable)       s += "stable ";
    if (s.empty()) s = "-";
    else if (s.back() == ' ') s.pop_back();
}

void appendAiFlagsStr(std::string& s, uint32_t flags) {
    if (flags & wowee::pipeline::WoweeCreature::AiPassive)    s += "passive ";
    if (flags & wowee::pipeline::WoweeCreature::AiAggressive) s += "aggressive ";
    if (flags & wowee::pipeline::WoweeCreature::AiFleeLowHp)  s += "flee ";
    if (flags & wowee::pipeline::WoweeCreature::AiCallHelp)   s += "call-help ";
    if (flags & wowee::pipeline::WoweeCreature::AiNoLeash)    s += "no-leash ";
    if (s.empty()) s = "-";
    else if (s.back() == ' ') s.pop_back();
}

bool saveOrError(const wowee::pipeline::WoweeCreature& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeCreatureLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wcrt\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeCreature& c,
                     const std::string& base) {
    std::printf("Wrote %s.wcrt\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  entries : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterCreatures";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcrtExt(base);
    auto c = wowee::pipeline::WoweeCreatureLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-creatures")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBandit(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BanditCreatures";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcrtExt(base);
    auto c = wowee::pipeline::WoweeCreatureLoader::makeBandit(name);
    if (!saveOrError(c, base, "gen-creatures-bandit")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMerchants(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "VillageMerchants";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcrtExt(base);
    auto c = wowee::pipeline::WoweeCreatureLoader::makeMerchants(name);
    if (!saveOrError(c, base, "gen-creatures-merchants")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcrtExt(base);
    if (!wowee::pipeline::WoweeCreatureLoader::exists(base)) {
        std::fprintf(stderr, "WCRT not found: %s.wcrt\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCreatureLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wcrt"] = base + ".wcrt";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            std::string ns, ais;
            appendNpcFlagsStr(ns, e.npcFlags);
            appendAiFlagsStr(ais, e.aiFlags);
            arr.push_back({
                {"creatureId", e.creatureId},
                {"displayId", e.displayId},
                {"name", e.name},
                {"subname", e.subname},
                {"minLevel", e.minLevel},
                {"maxLevel", e.maxLevel},
                {"baseHealth", e.baseHealth},
                {"healthPerLevel", e.healthPerLevel},
                {"baseMana", e.baseMana},
                {"manaPerLevel", e.manaPerLevel},
                {"factionId", e.factionId},
                {"npcFlags", e.npcFlags},
                {"npcFlagsStr", ns},
                {"typeId", e.typeId},
                {"typeName", wowee::pipeline::WoweeCreature::typeName(e.typeId)},
                {"familyId", e.familyId},
                {"familyName", wowee::pipeline::WoweeCreature::familyName(e.familyId)},
                {"damageMin", e.damageMin},
                {"damageMax", e.damageMax},
                {"attackSpeedMs", e.attackSpeedMs},
                {"baseArmor", e.baseArmor},
                {"walkSpeed", e.walkSpeed},
                {"runSpeed", e.runSpeed},
                {"gossipId", e.gossipId},
                {"equippedMain", e.equippedMain},
                {"equippedOffhand", e.equippedOffhand},
                {"equippedRanged", e.equippedRanged},
                {"aiFlags", e.aiFlags},
                {"aiFlagsStr", ais},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WCRT: %s.wcrt\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  entries : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   level    hp    type      faction  npc-flags        name\n");
    for (const auto& e : c.entries) {
        std::string ns;
        appendNpcFlagsStr(ns, e.npcFlags);
        std::printf("  %4u   %2u-%2u  %5u  %-9s  %5u    %-15s  %s%s%s\n",
                    e.creatureId, e.minLevel, e.maxLevel,
                    e.baseHealth,
                    wowee::pipeline::WoweeCreature::typeName(e.typeId),
                    e.factionId, ns.c_str(),
                    e.name.c_str(),
                    e.subname.empty() ? "" : " <",
                    e.subname.empty() ? "" : (e.subname + ">").c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcrtExt(base);
    if (!wowee::pipeline::WoweeCreatureLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wcrt: WCRT not found: %s.wcrt\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCreatureLoader::load(base);
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
                          " (id=" + std::to_string(e.creatureId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.creatureId == 0) {
            errors.push_back(ctx + ": creatureId is 0");
        }
        if (e.minLevel == 0) {
            errors.push_back(ctx + ": minLevel is 0");
        }
        if (e.minLevel > e.maxLevel) {
            errors.push_back(ctx + ": minLevel > maxLevel");
        }
        if (e.baseHealth == 0) {
            errors.push_back(ctx + ": baseHealth is 0 (creature dies on spawn)");
        }
        if (e.damageMin > e.damageMax) {
            errors.push_back(ctx + ": damageMin > damageMax");
        }
        if (e.attackSpeedMs == 0) {
            errors.push_back(ctx + ": attackSpeedMs is 0 (would divide by zero)");
        }
        if (e.runSpeed <= 0 || e.walkSpeed <= 0) {
            errors.push_back(ctx + ": walk/runSpeed must be positive");
        }
        // Conflicting AI flags: passive AND aggressive is incoherent.
        if ((e.aiFlags & wowee::pipeline::WoweeCreature::AiPassive) &&
            (e.aiFlags & wowee::pipeline::WoweeCreature::AiAggressive)) {
            warnings.push_back(ctx + ": both AiPassive and AiAggressive set");
        }
        // Vendor + hostile is rare but possible (gnomish merchants
        // surrounded by hostile NPCs); flag as warning to catch typos.
        if ((e.npcFlags & wowee::pipeline::WoweeCreature::Vendor) &&
            (e.aiFlags & wowee::pipeline::WoweeCreature::AiAggressive)) {
            warnings.push_back(ctx +
                ": vendor with aggressive AI (player can't trade)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.creatureId) {
                errors.push_back(ctx + ": duplicate creatureId");
                break;
            }
        }
        idsSeen.push_back(e.creatureId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wcrt"] = base + ".wcrt";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wcrt: %s.wcrt\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu creatures, all creatureIds unique\n",
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

bool handleCreaturesCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-creatures") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-creatures-bandit") == 0 && i + 1 < argc) {
        outRc = handleGenBandit(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-creatures-merchants") == 0 && i + 1 < argc) {
        outRc = handleGenMerchants(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wcrt") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wcrt") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
