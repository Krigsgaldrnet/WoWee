#include "cli_creature_patrols_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_creature_patrols.hpp"
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

std::string stripWcmrExt(std::string base) {
    stripExt(base, ".wcmr");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeCreaturePatrol& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeCreaturePatrolLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wcmr\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeCreaturePatrol& c,
                     const std::string& base) {
    size_t totalWp = 0;
    for (const auto& e : c.entries) totalWp += e.waypoints.size();
    std::printf("Wrote %s.wcmr\n", base.c_str());
    std::printf("  catalog   : %s\n", c.name.c_str());
    std::printf("  paths     : %zu\n", c.entries.size());
    std::printf("  waypoints : %zu (across all paths)\n", totalWp);
}

int handleGenPatrol(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "PatrolPaths";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcmrExt(base);
    auto c = wowee::pipeline::WoweeCreaturePatrolLoader::makePatrol(name);
    if (!saveOrError(c, base, "gen-cmr")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenCity(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "CityGuardRoutes";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcmrExt(base);
    auto c = wowee::pipeline::WoweeCreaturePatrolLoader::makeCity(name);
    if (!saveOrError(c, base, "gen-cmr-city")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBoss(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BossPatrols";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcmrExt(base);
    auto c = wowee::pipeline::WoweeCreaturePatrolLoader::makeBoss(name);
    if (!saveOrError(c, base, "gen-cmr-boss")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcmrExt(base);
    if (!wowee::pipeline::WoweeCreaturePatrolLoader::exists(base)) {
        std::fprintf(stderr, "WCMR not found: %s.wcmr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCreaturePatrolLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wcmr"] = base + ".wcmr";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            nlohmann::json wpArr = nlohmann::json::array();
            for (const auto& w : e.waypoints) {
                wpArr.push_back({
                    {"x", w.x}, {"y", w.y}, {"z", w.z},
                    {"delayMs", w.delayMs},
                });
            }
            arr.push_back({
                {"pathId", e.pathId},
                {"name", e.name},
                {"description", e.description},
                {"creatureGuid", e.creatureGuid},
                {"pathKind", e.pathKind},
                {"pathKindName", wowee::pipeline::WoweeCreaturePatrol::pathKindName(e.pathKind)},
                {"moveType", e.moveType},
                {"moveTypeName", wowee::pipeline::WoweeCreaturePatrol::moveTypeName(e.moveType)},
                {"waypointCount", e.waypoints.size()},
                {"pathLengthYards", c.pathLengthYards(e.pathId)},
                {"waypoints", wpArr},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WCMR: %s.wcmr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  paths   : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    creatureGuid  kind       move    waypoints  pathLen(yd)  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %8u     %-8s   %-5s   %5zu      %8.1f    %s\n",
                    e.pathId, e.creatureGuid,
                    wowee::pipeline::WoweeCreaturePatrol::pathKindName(e.pathKind),
                    wowee::pipeline::WoweeCreaturePatrol::moveTypeName(e.moveType),
                    e.waypoints.size(),
                    c.pathLengthYards(e.pathId),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcmrExt(base);
    if (!wowee::pipeline::WoweeCreaturePatrolLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wcmr: WCMR not found: %s.wcmr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCreaturePatrolLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.pathId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.pathId == 0)
            errors.push_back(ctx + ": pathId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.creatureGuid == 0)
            errors.push_back(ctx +
                ": creatureGuid is 0 — path is unbound to any spawn");
        if (e.pathKind > wowee::pipeline::WoweeCreaturePatrol::Random) {
            errors.push_back(ctx + ": pathKind " +
                std::to_string(e.pathKind) + " not in 0..3");
        }
        if (e.moveType > wowee::pipeline::WoweeCreaturePatrol::Swim) {
            errors.push_back(ctx + ": moveType " +
                std::to_string(e.moveType) + " not in 0..3");
        }
        if (e.waypoints.empty())
            errors.push_back(ctx + ": no waypoints — path has nothing to walk");
        if (e.waypoints.size() == 1)
            warnings.push_back(ctx +
                ": only 1 waypoint — creature will idle in place");
        // Loop with fewer than 3 waypoints is degenerate
        // (back and forth between 2 points isn't a loop).
        if (e.pathKind == wowee::pipeline::WoweeCreaturePatrol::Loop &&
            e.waypoints.size() < 3) {
            warnings.push_back(ctx +
                ": Loop with " +
                std::to_string(e.waypoints.size()) +
                " waypoints — fewer than 3 makes Loop "
                "indistinguishable from Reverse");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.pathId) {
                errors.push_back(ctx + ": duplicate pathId");
                break;
            }
        }
        idsSeen.push_back(e.pathId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wcmr"] = base + ".wcmr";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wcmr: %s.wcmr\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu paths, all pathIds unique\n",
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

bool handleCreaturePatrolsCatalog(int& i, int argc, char** argv,
                                  int& outRc) {
    if (std::strcmp(argv[i], "--gen-cmr") == 0 && i + 1 < argc) {
        outRc = handleGenPatrol(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cmr-city") == 0 && i + 1 < argc) {
        outRc = handleGenCity(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cmr-boss") == 0 && i + 1 < argc) {
        outRc = handleGenBoss(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wcmr") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wcmr") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
