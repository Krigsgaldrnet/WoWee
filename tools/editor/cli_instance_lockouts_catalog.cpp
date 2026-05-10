#include "cli_instance_lockouts_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_instance_lockouts.hpp"
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

std::string stripWhldExt(std::string base) {
    stripExt(base, ".whld");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeInstanceLockout& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeInstanceLockoutLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.whld\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeInstanceLockout& c,
                     const std::string& base) {
    std::printf("Wrote %s.whld\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  lockouts : %zu\n", c.entries.size());
}

int handleGenRaidWeekly(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RaidWeeklyLockouts";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWhldExt(base);
    auto c = wowee::pipeline::WoweeInstanceLockoutLoader::makeRaidWeekly(name);
    if (!saveOrError(c, base, "gen-hld")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenDungeonDaily(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "DungeonDailyLockouts";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWhldExt(base);
    auto c = wowee::pipeline::WoweeInstanceLockoutLoader::makeDungeonDaily(name);
    if (!saveOrError(c, base, "gen-hld-dungeon")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenWorldEvent(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WorldEventLockouts";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWhldExt(base);
    auto c = wowee::pipeline::WoweeInstanceLockoutLoader::makeWorldEvent(name);
    if (!saveOrError(c, base, "gen-hld-event")) return 1;
    printGenSummary(c, base);
    return 0;
}

void formatInterval(uint32_t ms, char* buf, size_t bufSize) {
    if (ms == 0) {
        std::snprintf(buf, bufSize, "-");
    } else if (ms % 86400000u == 0) {
        std::snprintf(buf, bufSize, "%ud", ms / 86400000u);
    } else if (ms % 3600000u == 0) {
        std::snprintf(buf, bufSize, "%uh", ms / 3600000u);
    } else if (ms % 60000u == 0) {
        std::snprintf(buf, bufSize, "%um", ms / 60000u);
    } else {
        std::snprintf(buf, bufSize, "%ums", ms);
    }
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWhldExt(base);
    if (!wowee::pipeline::WoweeInstanceLockoutLoader::exists(base)) {
        std::fprintf(stderr, "WHLD not found: %s.whld\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeInstanceLockoutLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["whld"] = base + ".whld";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"lockoutId", e.lockoutId},
                {"name", e.name},
                {"description", e.description},
                {"mapId", e.mapId},
                {"difficultyId", e.difficultyId},
                {"resetIntervalMs", e.resetIntervalMs},
                {"maxBossKillsPerLockout", e.maxBossKillsPerLockout},
                {"bonusRolls", e.bonusRolls},
                {"raidLockoutKind", e.raidLockoutKind},
                {"raidLockoutKindName", wowee::pipeline::WoweeInstanceLockout::lockoutKindName(e.raidLockoutKind)},
                {"raidGroupSize", e.raidGroupSize},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WHLD: %s.whld\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  lockouts : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    map   diff   kind         interval    bosses  size  bonus  name\n");
    for (const auto& e : c.entries) {
        char intervalBuf[16];
        formatInterval(e.resetIntervalMs, intervalBuf, sizeof(intervalBuf));
        std::printf("  %4u   %4u  %4u   %-10s   %-9s   %3u     %3u   %3u    %s\n",
                    e.lockoutId, e.mapId, e.difficultyId,
                    wowee::pipeline::WoweeInstanceLockout::lockoutKindName(e.raidLockoutKind),
                    intervalBuf,
                    e.maxBossKillsPerLockout, e.raidGroupSize,
                    e.bonusRolls, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWhldExt(base);
    if (!wowee::pipeline::WoweeInstanceLockoutLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-whld: WHLD not found: %s.whld\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeInstanceLockoutLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.lockoutId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.lockoutId == 0)
            errors.push_back(ctx + ": lockoutId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.raidLockoutKind > wowee::pipeline::WoweeInstanceLockout::Custom) {
            errors.push_back(ctx + ": raidLockoutKind " +
                std::to_string(e.raidLockoutKind) + " not in 0..3");
        }
        if (e.resetIntervalMs == 0)
            errors.push_back(ctx +
                ": resetIntervalMs is 0 — lockout would never reset");
        // Standard sizes are 5/10/25/40 — anything else is a
        // server-custom raid size.
        if (e.raidGroupSize != 5 && e.raidGroupSize != 10 &&
            e.raidGroupSize != 25 && e.raidGroupSize != 40) {
            warnings.push_back(ctx +
                ": non-standard raidGroupSize " +
                std::to_string(e.raidGroupSize) +
                " (canonical sizes are 5/10/25/40)");
        }
        // Daily kind with non-daily interval is suspicious.
        if (e.raidLockoutKind == wowee::pipeline::WoweeInstanceLockout::Daily &&
            e.resetIntervalMs != wowee::pipeline::WoweeInstanceLockout::kDailyMs) {
            warnings.push_back(ctx +
                ": Daily kind with resetIntervalMs " +
                std::to_string(e.resetIntervalMs) +
                " — canonical Daily is 86400000ms (24h)");
        }
        // Weekly kind with non-weekly interval is suspicious.
        if (e.raidLockoutKind == wowee::pipeline::WoweeInstanceLockout::Weekly &&
            e.resetIntervalMs != wowee::pipeline::WoweeInstanceLockout::kWeeklyMs) {
            warnings.push_back(ctx +
                ": Weekly kind with resetIntervalMs " +
                std::to_string(e.resetIntervalMs) +
                " — canonical Weekly is 604800000ms (7d)");
        }
        if (e.maxBossKillsPerLockout == 0)
            warnings.push_back(ctx +
                ": maxBossKillsPerLockout=0 — instance grants no "
                "lockout-bound kills, every visit is fresh");
        for (uint32_t prev : idsSeen) {
            if (prev == e.lockoutId) {
                errors.push_back(ctx + ": duplicate lockoutId");
                break;
            }
        }
        idsSeen.push_back(e.lockoutId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["whld"] = base + ".whld";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-whld: %s.whld\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu lockouts, all lockoutIds unique\n",
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

bool handleInstanceLockoutsCatalog(int& i, int argc, char** argv,
                                   int& outRc) {
    if (std::strcmp(argv[i], "--gen-hld") == 0 && i + 1 < argc) {
        outRc = handleGenRaidWeekly(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-hld-dungeon") == 0 && i + 1 < argc) {
        outRc = handleGenDungeonDaily(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-hld-event") == 0 && i + 1 < argc) {
        outRc = handleGenWorldEvent(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-whld") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-whld") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
