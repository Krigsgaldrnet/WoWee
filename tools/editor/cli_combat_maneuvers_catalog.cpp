#include "cli_combat_maneuvers_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_combat_maneuvers.hpp"
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

std::string stripWcmgExt(std::string base) {
    stripExt(base, ".wcmg");
    return base;
}

const char* categoryKindName(uint8_t k) {
    using C = wowee::pipeline::WoweeCombatManeuvers;
    switch (k) {
        case C::Stance:   return "stance";
        case C::Form:     return "form";
        case C::Aspect:   return "aspect";
        case C::Presence: return "presence";
        case C::Posture:  return "posture";
        case C::Sigil:    return "sigil";
        default:          return "unknown";
    }
}

bool saveOrError(const wowee::pipeline::WoweeCombatManeuvers& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeCombatManeuversLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wcmg\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeCombatManeuvers& c,
                     const std::string& base) {
    size_t totalSpells = 0;
    for (const auto& e : c.entries) totalSpells += e.members.size();
    std::printf("Wrote %s.wcmg\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  groups  : %zu (%zu member spells total)\n",
                c.entries.size(), totalSpells);
}

int handleGenWarrior(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WarriorStanceMutex";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcmgExt(base);
    auto c = wowee::pipeline::WoweeCombatManeuversLoader::makeWarrior(name);
    if (!saveOrError(c, base, "gen-cmg")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenDruid(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "DruidShapeshiftMutex";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcmgExt(base);
    auto c = wowee::pipeline::WoweeCombatManeuversLoader::makeDruid(name);
    if (!saveOrError(c, base, "gen-cmg-druid")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenAllMutex(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AllClassMutexGroups";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcmgExt(base);
    auto c = wowee::pipeline::WoweeCombatManeuversLoader::makeAllMutex(name);
    if (!saveOrError(c, base, "gen-cmg-all")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcmgExt(base);
    if (!wowee::pipeline::WoweeCombatManeuversLoader::exists(base)) {
        std::fprintf(stderr, "WCMG not found: %s.wcmg\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCombatManeuversLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wcmg"] = base + ".wcmg";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"groupId", e.groupId},
                {"name", e.name},
                {"description", e.description},
                {"classMask", e.classMask},
                {"categoryKind", e.categoryKind},
                {"categoryKindName",
                    categoryKindName(e.categoryKind)},
                {"exclusive", e.exclusive != 0},
                {"iconColorRGBA", e.iconColorRGBA},
                {"members", e.members},
                {"memberCount", e.members.size()},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WCMG: %s.wcmg\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  groups  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   classMask  category   excl   members   name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %8u  %-9s  %4s  %7zu   %s\n",
                    e.groupId, e.classMask,
                    categoryKindName(e.categoryKind),
                    e.exclusive ? "yes " : "no  ",
                    e.members.size(), e.name.c_str());
        // Spell ID list as a wrapped sub-line for the
        // operator's eye.
        if (!e.members.empty()) {
            std::printf("           members:");
            size_t col = 19;
            for (size_t k = 0; k < e.members.size(); ++k) {
                char buf[16];
                int n = std::snprintf(buf, sizeof(buf),
                    " %u%s", e.members[k],
                    (k + 1 < e.members.size()) ? "," : "");
                if (col + n > 78) {
                    std::printf("\n                    ");
                    col = 20;
                }
                std::printf("%s", buf);
                col += n;
            }
            std::printf("\n");
        }
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcmgExt(base);
    if (!wowee::pipeline::WoweeCombatManeuversLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wcmg: WCMG not found: %s.wcmg\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCombatManeuversLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    // Track which spell IDs appear in any exclusive group
    // — a spell that appears in TWO different exclusive
    // groups creates an undecidable mutex (which group's
    // outline does the action bar use?).
    std::set<uint32_t> spellInExclusiveGroup;
    std::vector<std::pair<uint32_t, uint32_t>> doubleAssign;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.groupId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.groupId == 0)
            errors.push_back(ctx + ": groupId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.classMask == 0) {
            errors.push_back(ctx +
                ": classMask is 0 — group is not "
                "associated with any class");
        }
        if (e.categoryKind > 5) {
            errors.push_back(ctx + ": categoryKind " +
                std::to_string(e.categoryKind) +
                " out of range (must be 0..5)");
        }
        if (e.members.empty()) {
            errors.push_back(ctx +
                ": members[] is empty — mutex group has "
                "nothing to switch between");
        }
        // A single-member mutex group is technically legal
        // but suggests a content authoring error.
        if (e.members.size() == 1) {
            warnings.push_back(ctx +
                ": only 1 member spell — mutex with one "
                "element has no exclusion to enforce; "
                "verify if intentional");
        }
        // Check for duplicate spell IDs WITHIN this
        // group's members list.
        std::set<uint32_t> seen;
        for (uint32_t m : e.members) {
            if (m == 0) {
                errors.push_back(ctx +
                    ": members[] contains spellId 0");
                continue;
            }
            if (!seen.insert(m).second) {
                errors.push_back(ctx +
                    ": duplicate spellId " +
                    std::to_string(m) + " within members[]");
            }
            // Cross-group check: same spell in two
            // exclusive groups is an authoring bug.
            if (e.exclusive) {
                if (!spellInExclusiveGroup.insert(m).second) {
                    doubleAssign.push_back({m, e.groupId});
                }
            }
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.groupId) {
                errors.push_back(ctx + ": duplicate groupId");
                break;
            }
        }
        idsSeen.push_back(e.groupId);
    }
    for (auto [spellId, groupId] : doubleAssign) {
        errors.push_back(
            "spellId " + std::to_string(spellId) +
            " appears in multiple exclusive groups "
            "(latest: groupId " + std::to_string(groupId) +
            ") — action bar mutex would be undecidable");
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wcmg"] = base + ".wcmg";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wcmg: %s.wcmg\n", base.c_str());
    if (ok && warnings.empty()) {
        size_t totalSpells = 0;
        for (const auto& e : c.entries) totalSpells += e.members.size();
        std::printf("  OK — %zu groups, %zu member spells, all "
                    "groupIds unique\n",
                    c.entries.size(), totalSpells);
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

bool handleCombatManeuversCatalog(int& i, int argc, char** argv,
                                   int& outRc) {
    if (std::strcmp(argv[i], "--gen-cmg") == 0 && i + 1 < argc) {
        outRc = handleGenWarrior(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cmg-druid") == 0 && i + 1 < argc) {
        outRc = handleGenDruid(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cmg-all") == 0 && i + 1 < argc) {
        outRc = handleGenAllMutex(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wcmg") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wcmg") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
