#include "cli_hearth_binds_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_hearth_binds.hpp"
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

std::string stripWhrtExt(std::string base) {
    stripExt(base, ".whrt");
    return base;
}

const char* bindKindName(uint8_t k) {
    using H = wowee::pipeline::WoweeHearthBinds;
    switch (k) {
        case H::Inn:         return "inn";
        case H::Capital:     return "capital";
        case H::Quest:       return "quest";
        case H::Guild:       return "guild";
        case H::SpecialPort: return "specialport";
        case H::Faction:     return "faction";
        default:             return "unknown";
    }
}

const char* factionMaskName(uint8_t f) {
    using H = wowee::pipeline::WoweeHearthBinds;
    switch (f) {
        case H::AllianceOnly: return "alliance";
        case H::HordeOnly:    return "horde";
        case H::Both:         return "both";
        default:              return "unknown";
    }
}

bool saveOrError(const wowee::pipeline::WoweeHearthBinds& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeHearthBindsLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.whrt\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeHearthBinds& c,
                     const std::string& base) {
    std::printf("Wrote %s.whrt\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  binds   : %zu\n", c.entries.size());
}

int handleGenStarterCities(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterCityBinds";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWhrtExt(base);
    auto c = wowee::pipeline::WoweeHearthBindsLoader::makeStarterCities(name);
    if (!saveOrError(c, base, "gen-hrt")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenCapitals(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "CapitalBinds";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWhrtExt(base);
    auto c = wowee::pipeline::WoweeHearthBindsLoader::makeCapitals(name);
    if (!saveOrError(c, base, "gen-hrt-capitals")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenStarterInns(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterInns";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWhrtExt(base);
    auto c = wowee::pipeline::WoweeHearthBindsLoader::makeStarterInns(name);
    if (!saveOrError(c, base, "gen-hrt-inns")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWhrtExt(base);
    if (!wowee::pipeline::WoweeHearthBindsLoader::exists(base)) {
        std::fprintf(stderr, "WHRT not found: %s.whrt\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeHearthBindsLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["whrt"] = base + ".whrt";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"bindId", e.bindId},
                {"name", e.name},
                {"description", e.description},
                {"mapId", e.mapId},
                {"areaId", e.areaId},
                {"x", e.x}, {"y", e.y}, {"z", e.z},
                {"facing", e.facing},
                {"npcId", e.npcId},
                {"factionMask", e.factionMask},
                {"factionMaskName", factionMaskName(e.factionMask)},
                {"bindKind", e.bindKind},
                {"bindKindName", bindKindName(e.bindKind)},
                {"levelMin", e.levelMin},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WHRT: %s.whrt\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  binds   : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   map  area  faction   kind     npc    lvl  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u  %4u  %4u  %-9s %-12s %5u  %3u  %s\n",
                    e.bindId, e.mapId, e.areaId,
                    factionMaskName(e.factionMask),
                    bindKindName(e.bindKind),
                    e.npcId, e.levelMin, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWhrtExt(base);
    if (!wowee::pipeline::WoweeHearthBindsLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-whrt: WHRT not found: %s.whrt\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeHearthBindsLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.bindId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.bindId == 0)
            errors.push_back(ctx + ": bindId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.factionMask == 0 || e.factionMask > 3) {
            errors.push_back(ctx + ": factionMask " +
                std::to_string(e.factionMask) +
                " out of range (must be 1=A / 2=H / 3=Both)");
        }
        if (e.bindKind > 5) {
            errors.push_back(ctx + ": bindKind " +
                std::to_string(e.bindKind) +
                " out of range (must be 0..5)");
        }
        // Bind position must not be at origin (probably
        // unset). Origin coords often indicate a forgotten
        // SetPosition call in a content authoring tool.
        if (e.x == 0.0f && e.y == 0.0f && e.z == 0.0f) {
            warnings.push_back(ctx +
                ": position is (0,0,0) — likely forgotten "
                "SetPosition; bind would teleport player to "
                "world origin");
        }
        // Inn-kind bindings should have an NPC bind clerk
        // (the innkeeper). SpecialPort bindings often
        // don't. Warn if Inn and npcId=0.
        using H = wowee::pipeline::WoweeHearthBinds;
        if (e.bindKind == H::Inn && e.npcId == 0) {
            warnings.push_back(ctx +
                ": Inn bind has no NPC innkeeper (npcId=0). "
                "Inn bindings should reference the WCRT "
                "innkeeper entry.");
        }
        // Quest-given bindings without level gate are
        // suspicious — quest binds usually require level
        // (Theramore at 30+, Wyrmrest at 70+).
        if (e.bindKind == H::Quest && e.levelMin == 0) {
            warnings.push_back(ctx +
                ": Quest bind has levelMin=0 — quest "
                "bindings usually have a minimum level "
                "gate; verify if intentional");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.bindId) {
                errors.push_back(ctx + ": duplicate bindId");
                break;
            }
        }
        idsSeen.push_back(e.bindId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["whrt"] = base + ".whrt";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-whrt: %s.whrt\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu binds, all bindIds unique\n",
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

bool handleHearthBindsCatalog(int& i, int argc, char** argv,
                              int& outRc) {
    if (std::strcmp(argv[i], "--gen-hrt") == 0 && i + 1 < argc) {
        outRc = handleGenStarterCities(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-hrt-capitals") == 0 && i + 1 < argc) {
        outRc = handleGenCapitals(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-hrt-inns") == 0 && i + 1 < argc) {
        outRc = handleGenStarterInns(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-whrt") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-whrt") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
