#include "cli_spawns_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_spawns.hpp"
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWspnExt(std::string base) {
    stripExt(base, ".wspn");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeSpawns& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSpawnsLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wspn\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSpawns& c,
                     const std::string& base) {
    std::printf("Wrote %s.wspn\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  entries : %zu (creature=%u object=%u doodad=%u)\n",
                c.entries.size(),
                c.countByKind(wowee::pipeline::WoweeSpawns::Creature),
                c.countByKind(wowee::pipeline::WoweeSpawns::GameObject),
                c.countByKind(wowee::pipeline::WoweeSpawns::Doodad));
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterSpawns";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWspnExt(base);
    auto c = wowee::pipeline::WoweeSpawnsLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-spawns")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenCamp(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BanditCamp";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWspnExt(base);
    auto c = wowee::pipeline::WoweeSpawnsLoader::makeCamp(name);
    if (!saveOrError(c, base, "gen-spawns-camp")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenVillage(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "VillageSpawns";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWspnExt(base);
    auto c = wowee::pipeline::WoweeSpawnsLoader::makeVillage(name);
    if (!saveOrError(c, base, "gen-spawns-village")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWspnExt(base);
    if (!wowee::pipeline::WoweeSpawnsLoader::exists(base)) {
        std::fprintf(stderr, "WSPN not found: %s.wspn\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpawnsLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wspn"] = base + ".wspn";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        j["countCreature"] =
            c.countByKind(wowee::pipeline::WoweeSpawns::Creature);
        j["countObject"] =
            c.countByKind(wowee::pipeline::WoweeSpawns::GameObject);
        j["countDoodad"] =
            c.countByKind(wowee::pipeline::WoweeSpawns::Doodad);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"kind", e.kind},
                {"kindName", wowee::pipeline::WoweeSpawns::kindName(e.kind)},
                {"entryId", e.entryId},
                {"position", {e.position.x, e.position.y, e.position.z}},
                {"rotation", {e.rotation.x, e.rotation.y, e.rotation.z}},
                {"scale", e.scale},
                {"flags", e.flags},
                {"respawnSec", e.respawnSec},
                {"factionId", e.factionId},
                {"questIdRequired", e.questIdRequired},
                {"wanderRadius", e.wanderRadius},
                {"label", e.label},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSPN: %s.wspn\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  entries : %zu (creature=%u object=%u doodad=%u)\n",
                c.entries.size(),
                c.countByKind(wowee::pipeline::WoweeSpawns::Creature),
                c.countByKind(wowee::pipeline::WoweeSpawns::GameObject),
                c.countByKind(wowee::pipeline::WoweeSpawns::Doodad));
    if (c.entries.empty()) return 0;
    std::printf("    kind     entry  pos (x, y, z)        respawn  rad  label\n");
    for (const auto& e : c.entries) {
        std::printf("  %-9s %5u  (%6.1f,%6.1f,%6.1f)  %5us  %4.1f  %s\n",
                    wowee::pipeline::WoweeSpawns::kindName(e.kind),
                    e.entryId,
                    e.position.x, e.position.y, e.position.z,
                    e.respawnSec, e.wanderRadius,
                    e.label.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWspnExt(base);
    if (!wowee::pipeline::WoweeSpawnsLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wspn: WSPN not found: %s.wspn\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpawnsLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k);
        if (!e.label.empty()) ctx += " (" + e.label + ")";
        if (e.kind > wowee::pipeline::WoweeSpawns::Doodad) {
            errors.push_back(ctx + ": kind " + std::to_string(e.kind) +
                             " not in known range 0..2");
        }
        if (!std::isfinite(e.position.x) ||
            !std::isfinite(e.position.y) ||
            !std::isfinite(e.position.z) ||
            !std::isfinite(e.rotation.x) ||
            !std::isfinite(e.rotation.y) ||
            !std::isfinite(e.rotation.z)) {
            errors.push_back(ctx + ": position/rotation not finite");
        }
        if (!std::isfinite(e.scale) || e.scale <= 0) {
            errors.push_back(ctx + ": scale not finite or <= 0");
        }
        if (!std::isfinite(e.wanderRadius) || e.wanderRadius < 0) {
            errors.push_back(ctx + ": wanderRadius not finite or < 0");
        }
        // Doodads should not have a respawn timer (they are
        // permanent visual props). Catch the common misuse.
        if (e.kind == wowee::pipeline::WoweeSpawns::Doodad &&
            e.respawnSec != 0) {
            warnings.push_back(ctx +
                ": doodad has non-zero respawnSec — doodads are static");
        }
        // Creatures with respawn 0 will spawn once and never
        // come back; flag as a warning since it's almost
        // always a mistake.
        if (e.kind == wowee::pipeline::WoweeSpawns::Creature &&
            e.respawnSec == 0 &&
            !(e.flags & wowee::pipeline::WoweeSpawns::EventOnly)) {
            warnings.push_back(ctx +
                ": creature with respawnSec=0 will not respawn after kill");
        }
        if (e.entryId == 0) {
            warnings.push_back(ctx +
                ": entryId is 0 (no template referenced)");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wspn"] = base + ".wspn";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wspn: %s.wspn\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu entries (creature=%u object=%u doodad=%u)\n",
                    c.entries.size(),
                    c.countByKind(wowee::pipeline::WoweeSpawns::Creature),
                    c.countByKind(wowee::pipeline::WoweeSpawns::GameObject),
                    c.countByKind(wowee::pipeline::WoweeSpawns::Doodad));
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

bool handleSpawnsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-spawns") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-spawns-camp") == 0 && i + 1 < argc) {
        outRc = handleGenCamp(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-spawns-village") == 0 && i + 1 < argc) {
        outRc = handleGenVillage(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wspn") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wspn") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
