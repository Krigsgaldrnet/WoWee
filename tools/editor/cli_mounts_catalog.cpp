#include "cli_mounts_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_mounts.hpp"
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

std::string stripWmouExt(std::string base) {
    stripExt(base, ".wmou");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeMount& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeMountLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wmou\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeMount& c,
                     const std::string& base) {
    std::printf("Wrote %s.wmou\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  mounts  : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterMounts";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmouExt(base);
    auto c = wowee::pipeline::WoweeMountLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-mounts")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRacial(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RacialMounts";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmouExt(base);
    auto c = wowee::pipeline::WoweeMountLoader::makeRacial(name);
    if (!saveOrError(c, base, "gen-mounts-racial")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenFlying(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FlyingMounts";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmouExt(base);
    auto c = wowee::pipeline::WoweeMountLoader::makeFlying(name);
    if (!saveOrError(c, base, "gen-mounts-flying")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWmouExt(base);
    if (!wowee::pipeline::WoweeMountLoader::exists(base)) {
        std::fprintf(stderr, "WMOU not found: %s.wmou\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeMountLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wmou"] = base + ".wmou";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"mountId", e.mountId},
                {"name", e.name},
                {"description", e.description},
                {"iconPath", e.iconPath},
                {"displayId", e.displayId},
                {"summonSpellId", e.summonSpellId},
                {"itemIdToLearn", e.itemIdToLearn},
                {"requiredSkillId", e.requiredSkillId},
                {"requiredSkillRank", e.requiredSkillRank},
                {"speedPercent", e.speedPercent},
                {"mountKind", e.mountKind},
                {"mountKindName", wowee::pipeline::WoweeMount::kindName(e.mountKind)},
                {"factionId", e.factionId},
                {"factionName", wowee::pipeline::WoweeMount::factionName(e.factionId)},
                {"categoryId", e.categoryId},
                {"categoryName", wowee::pipeline::WoweeMount::categoryName(e.categoryId)},
                {"raceMask", e.raceMask},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WMOU: %s.wmou\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  mounts  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   kind     speed  rank   faction    category      name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-7s  %4u%%  %4u   %-9s  %-12s  %s\n",
                    e.mountId,
                    wowee::pipeline::WoweeMount::kindName(e.mountKind),
                    e.speedPercent, e.requiredSkillRank,
                    wowee::pipeline::WoweeMount::factionName(e.factionId),
                    wowee::pipeline::WoweeMount::categoryName(e.categoryId),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWmouExt(base);
    if (!wowee::pipeline::WoweeMountLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wmou: WMOU not found: %s.wmou\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeMountLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.mountId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.mountId == 0) errors.push_back(ctx + ": mountId is 0");
        if (e.name.empty()) errors.push_back(ctx + ": name is empty");
        if (e.summonSpellId == 0) {
            errors.push_back(ctx + ": summonSpellId is 0 (mount cannot be cast)");
        }
        if (e.mountKind > wowee::pipeline::WoweeMount::Aquatic) {
            errors.push_back(ctx + ": mountKind " +
                std::to_string(e.mountKind) + " not in 0..4");
        }
        if (e.factionId > wowee::pipeline::WoweeMount::Horde) {
            errors.push_back(ctx + ": factionId " +
                std::to_string(e.factionId) + " not in 0..2");
        }
        if (e.categoryId > wowee::pipeline::WoweeMount::ClassMount) {
            errors.push_back(ctx + ": categoryId " +
                std::to_string(e.categoryId) + " not in 0..7");
        }
        if (e.speedPercent == 0) {
            warnings.push_back(ctx +
                ": speedPercent=0 (mount provides no speed bonus)");
        }
        // Flying / Hybrid mounts need >= journeyman riding
        // (rank 150 in canonical Classic+TBC scaling).
        if ((e.mountKind == wowee::pipeline::WoweeMount::Flying ||
             e.mountKind == wowee::pipeline::WoweeMount::Hybrid) &&
            e.requiredSkillRank < 150) {
            warnings.push_back(ctx +
                ": flying mount with riding rank < 150 (player can't fly)");
        }
        // Racial category needs raceMask; non-racial shouldn't have one.
        if (e.categoryId == wowee::pipeline::WoweeMount::Racial &&
            e.raceMask == 0) {
            warnings.push_back(ctx +
                ": Racial category but raceMask=0 (any race can use)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.mountId) {
                errors.push_back(ctx + ": duplicate mountId");
                break;
            }
        }
        idsSeen.push_back(e.mountId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wmou"] = base + ".wmou";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wmou: %s.wmou\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu mounts, all mountIds unique\n",
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

bool handleMountsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-mounts") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mounts-racial") == 0 && i + 1 < argc) {
        outRc = handleGenRacial(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mounts-flying") == 0 && i + 1 < argc) {
        outRc = handleGenFlying(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wmou") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wmou") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
