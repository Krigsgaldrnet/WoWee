#include "cli_loot_modes_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_loot_modes.hpp"
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

std::string stripWlmaExt(std::string base) {
    stripExt(base, ".wlma");
    return base;
}

const char* modeKindName(uint8_t k) {
    using L = wowee::pipeline::WoweeLootModes;
    switch (k) {
        case L::FreeForAll:      return "freeforall";
        case L::RoundRobin:      return "roundrobin";
        case L::MasterLoot:      return "masterloot";
        case L::NeedBeforeGreed: return "needbeforegreed";
        case L::Personal:        return "personal";
        case L::Disenchant:      return "disenchant";
        default:                 return "unknown";
    }
}

const char* qualityName(uint8_t q) {
    switch (q) {
        case 0: return "Poor";
        case 1: return "Common";
        case 2: return "Uncommon";
        case 3: return "Rare";
        case 4: return "Epic";
        case 5: return "Legendary";
        case 6: return "Artifact";
        case 7: return "Heirloom";
        default: return "?";
    }
}

bool saveOrError(const wowee::pipeline::WoweeLootModes& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeLootModesLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wlma\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeLootModes& c,
                     const std::string& base) {
    std::printf("Wrote %s.wlma\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  modes   : %zu\n", c.entries.size());
}

int handleGenStandard(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StandardLootModes";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWlmaExt(base);
    auto c = wowee::pipeline::WoweeLootModesLoader::makeStandard(name);
    if (!saveOrError(c, base, "gen-lma")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRaid(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RaidLootPolicies";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWlmaExt(base);
    auto c = wowee::pipeline::WoweeLootModesLoader::makeRaidPolicies(name);
    if (!saveOrError(c, base, "gen-lma-raid")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenAFK(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AFKPreventionLootModes";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWlmaExt(base);
    auto c = wowee::pipeline::WoweeLootModesLoader::makeAFKPrevention(name);
    if (!saveOrError(c, base, "gen-lma-afk")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWlmaExt(base);
    if (!wowee::pipeline::WoweeLootModesLoader::exists(base)) {
        std::fprintf(stderr, "WLMA not found: %s.wlma\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeLootModesLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wlma"] = base + ".wlma";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"modeId", e.modeId},
                {"name", e.name},
                {"description", e.description},
                {"modeKind", e.modeKind},
                {"modeKindName", modeKindName(e.modeKind)},
                {"thresholdQuality", e.thresholdQuality},
                {"thresholdQualityName",
                    qualityName(e.thresholdQuality)},
                {"masterLooterRequired",
                    e.masterLooterRequired != 0},
                {"idleSkipSec", e.idleSkipSec},
                {"timeoutFallbackKind", e.timeoutFallbackKind},
                {"timeoutFallbackKindName",
                    modeKindName(e.timeoutFallbackKind)},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WLMA: %s.wlma\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  modes   : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   kind             threshold     ML   idleSkip  fallback         name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-15s   %-10s    %s    %3us     %-15s   %s\n",
                    e.modeId, modeKindName(e.modeKind),
                    qualityName(e.thresholdQuality),
                    e.masterLooterRequired ? "Y" : "n",
                    e.idleSkipSec,
                    modeKindName(e.timeoutFallbackKind),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWlmaExt(base);
    if (!wowee::pipeline::WoweeLootModesLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wlma: WLMA not found: %s.wlma\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeLootModesLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.modeId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.modeId == 0)
            errors.push_back(ctx + ": modeId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.modeKind > 5) {
            errors.push_back(ctx + ": modeKind " +
                std::to_string(e.modeKind) +
                " out of range (must be 0..5)");
        }
        if (e.thresholdQuality > 7) {
            errors.push_back(ctx + ": thresholdQuality " +
                std::to_string(e.thresholdQuality) +
                " out of range (must be 0..7 — Poor "
                "through Heirloom)");
        }
        if (e.timeoutFallbackKind > 5) {
            errors.push_back(ctx + ": timeoutFallbackKind " +
                std::to_string(e.timeoutFallbackKind) +
                " out of range (must be 0..5)");
        }
        // Per-kind validity: MasterLoot kind REQUIRES
        // masterLooterRequired=1 (else the policy is
        // self-contradicting — saying use Master Loot
        // without requiring a master looter).
        using L = wowee::pipeline::WoweeLootModes;
        if (e.modeKind == L::MasterLoot &&
            e.masterLooterRequired == 0) {
            errors.push_back(ctx +
                ": MasterLoot kind with master"
                "LooterRequired=0 — policy contradicts "
                "itself (Master Loot mode without "
                "requiring a master looter)");
        }
        // Personal kind doesn't use master-looter
        // semantics — flag as warning if set.
        if (e.modeKind == L::Personal &&
            e.masterLooterRequired != 0) {
            warnings.push_back(ctx +
                ": Personal kind with master"
                "LooterRequired=1 — Personal Loot "
                "doesn't use master-looter semantics; "
                "the flag is meaningless");
        }
        // Fallback to self is meaningful only for kinds
        // with a "leader" concept that can disconnect
        // (MasterLoot). Other kinds (FFA, Personal,
        // RoundRobin, NBG) have no leader-timeout
        // scenario so the fallback field is unused —
        // setting it to self is the conventional default
        // and not a bug.
        if (e.modeKind == L::MasterLoot &&
            e.timeoutFallbackKind == e.modeKind) {
            warnings.push_back(ctx +
                ": MasterLoot timeoutFallbackKind equals "
                "modeKind — if the master looter "
                "disconnects, the fallback would just "
                "wait for them to reconnect (no policy "
                "change). Common alternatives: Need"
                "BeforeGreed for democratic recovery, "
                "FreeForAll for fast unblocking.");
        }
        if (!idsSeen.insert(e.modeId).second) {
            errors.push_back(ctx + ": duplicate modeId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wlma"] = base + ".wlma";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wlma: %s.wlma\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu modes, all modeIds unique, "
                    "per-kind constraints satisfied\n",
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

bool handleLootModesCatalog(int& i, int argc, char** argv,
                              int& outRc) {
    if (std::strcmp(argv[i], "--gen-lma") == 0 && i + 1 < argc) {
        outRc = handleGenStandard(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-lma-raid") == 0 && i + 1 < argc) {
        outRc = handleGenRaid(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-lma-afk") == 0 && i + 1 < argc) {
        outRc = handleGenAFK(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wlma") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wlma") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
