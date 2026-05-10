#include "cli_realm_list_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_realm_list.hpp"
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

std::string stripWmspExt(std::string base) {
    stripExt(base, ".wmsp");
    return base;
}

const char* realmTypeName(uint8_t t) {
    using R = wowee::pipeline::WoweeRealmList;
    switch (t) {
        case R::Normal: return "normal";
        case R::PvP:    return "pvp";
        case R::RP:     return "rp";
        case R::RPPvP:  return "rppvp";
        case R::Test:   return "test";
        default:        return "unknown";
    }
}

const char* realmCategoryName(uint8_t c) {
    using R = wowee::pipeline::WoweeRealmList;
    switch (c) {
        case R::Public:  return "public";
        case R::Private: return "private";
        case R::Beta:    return "beta";
        case R::Dev:     return "dev";
        default:         return "unknown";
    }
}

const char* expansionName(uint8_t e) {
    using R = wowee::pipeline::WoweeRealmList;
    switch (e) {
        case R::Vanilla: return "vanilla";
        case R::TBC:     return "tbc";
        case R::WotLK:   return "wotlk";
        case R::Cata:    return "cata";
        default:         return "unknown";
    }
}

const char* populationName(uint8_t p) {
    using R = wowee::pipeline::WoweeRealmList;
    switch (p) {
        case R::Low:    return "low";
        case R::Medium: return "medium";
        case R::High:   return "high";
        case R::Full:   return "full";
        case R::Locked: return "locked";
        default:        return "unknown";
    }
}

bool saveOrError(const wowee::pipeline::WoweeRealmList& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeRealmListLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wmsp\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeRealmList& c,
                     const std::string& base) {
    std::printf("Wrote %s.wmsp\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  realms  : %zu\n", c.entries.size());
}

int handleGenSingle(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "SingleRealm";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmspExt(base);
    auto c = wowee::pipeline::WoweeRealmListLoader::makeSingleRealm(name);
    if (!saveOrError(c, base, "gen-msp")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenCluster(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "PvPCluster";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmspExt(base);
    auto c = wowee::pipeline::WoweeRealmListLoader::makePvPCluster(name);
    if (!saveOrError(c, base, "gen-msp-cluster")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMultiExp(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MultiExpansion";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmspExt(base);
    auto c = wowee::pipeline::WoweeRealmListLoader::makeMultiExpansion(name);
    if (!saveOrError(c, base, "gen-msp-multi")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWmspExt(base);
    if (!wowee::pipeline::WoweeRealmListLoader::exists(base)) {
        std::fprintf(stderr, "WMSP not found: %s.wmsp\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeRealmListLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wmsp"] = base + ".wmsp";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            char ver[24];
            std::snprintf(ver, sizeof(ver), "%u.%u.%u",
                          e.versionMajor, e.versionMinor,
                          e.versionPatch);
            arr.push_back({
                {"realmId", e.realmId},
                {"name", e.name},
                {"description", e.description},
                {"address", e.address},
                {"realmType", e.realmType},
                {"realmTypeName", realmTypeName(e.realmType)},
                {"realmCategory", e.realmCategory},
                {"realmCategoryName",
                    realmCategoryName(e.realmCategory)},
                {"expansion", e.expansion},
                {"expansionName", expansionName(e.expansion)},
                {"population", e.population},
                {"populationName", populationName(e.population)},
                {"characterCap", e.characterCap},
                {"gmOnly", e.gmOnly != 0},
                {"timezone", e.timezone},
                {"versionMajor", e.versionMajor},
                {"versionMinor", e.versionMinor},
                {"versionPatch", e.versionPatch},
                {"versionString", ver},
                {"buildNumber", e.buildNumber},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WMSP: %s.wmsp\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  realms  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id  type     category  expansion  pop      cap  gm   build   address\n");
    for (const auto& e : c.entries) {
        std::printf("  %3u  %-7s  %-7s   %-9s  %-7s  %3u  %s   %5u   %s\n",
                    e.realmId,
                    realmTypeName(e.realmType),
                    realmCategoryName(e.realmCategory),
                    expansionName(e.expansion),
                    populationName(e.population),
                    e.characterCap,
                    e.gmOnly ? "Y" : "n",
                    e.buildNumber,
                    e.address.c_str());
        std::printf("           %s\n", e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWmspExt(base);
    if (!wowee::pipeline::WoweeRealmListLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wmsp: WMSP not found: %s.wmsp\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeRealmListLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    std::set<std::string> namesSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.realmId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.realmId == 0)
            errors.push_back(ctx + ": realmId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.address.empty()) {
            errors.push_back(ctx +
                ": address is empty — login server cannot "
                "route session to this realm");
        }
        // Address must contain a colon-separated port.
        if (!e.address.empty() &&
            e.address.find(':') == std::string::npos) {
            warnings.push_back(ctx + ": address '" + e.address +
                "' has no port — login client typically "
                "expects 'host:port' form (defaults to "
                "8085 if absent)");
        }
        // Validate realmType against known enum values.
        using R = wowee::pipeline::WoweeRealmList;
        if (e.realmType != R::Normal && e.realmType != R::PvP &&
            e.realmType != R::RP && e.realmType != R::RPPvP &&
            e.realmType != R::Test) {
            errors.push_back(ctx + ": realmType " +
                std::to_string(e.realmType) +
                " is not a known value (0/1/4/6/8)");
        }
        if (e.realmCategory > 3) {
            errors.push_back(ctx + ": realmCategory " +
                std::to_string(e.realmCategory) +
                " out of range (must be 0..3)");
        }
        if (e.expansion > 3) {
            errors.push_back(ctx + ": expansion " +
                std::to_string(e.expansion) +
                " out of range (must be 0..3)");
        }
        if (e.population > 4) {
            errors.push_back(ctx + ": population " +
                std::to_string(e.population) +
                " out of range (must be 0..4)");
        }
        if (e.characterCap == 0) {
            errors.push_back(ctx +
                ": characterCap=0 — players can't create "
                "any character on this realm");
        }
        // Build number sanity check — known WoW build
        // numbers are at least 5000.
        if (e.buildNumber > 0 && e.buildNumber < 5000) {
            warnings.push_back(ctx + ": buildNumber " +
                std::to_string(e.buildNumber) +
                " < 5000 — known WoW client builds start "
                "at 5875 (Vanilla 1.12.1)");
        }
        // Realm names must be unique on the picker.
        if (!namesSeen.insert(e.name).second) {
            errors.push_back(ctx +
                ": duplicate realm name '" + e.name +
                "' — picker requires unique display names");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.realmId) {
                errors.push_back(ctx + ": duplicate realmId");
                break;
            }
        }
        idsSeen.push_back(e.realmId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wmsp"] = base + ".wmsp";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wmsp: %s.wmsp\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu realms, all realmIds + names "
                    "unique\n", c.entries.size());
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

bool handleRealmListCatalog(int& i, int argc, char** argv,
                             int& outRc) {
    if (std::strcmp(argv[i], "--gen-msp") == 0 && i + 1 < argc) {
        outRc = handleGenSingle(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-msp-cluster") == 0 && i + 1 < argc) {
        outRc = handleGenCluster(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-msp-multi") == 0 && i + 1 < argc) {
        outRc = handleGenMultiExp(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wmsp") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wmsp") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
