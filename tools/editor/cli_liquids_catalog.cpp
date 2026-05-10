#include "cli_liquids_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_liquids.hpp"
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

std::string stripWliqExt(std::string base) {
    stripExt(base, ".wliq");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeLiquid& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeLiquidLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wliq\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeLiquid& c,
                     const std::string& base) {
    std::printf("Wrote %s.wliq\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  liquids : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterLiquids";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWliqExt(base);
    auto c = wowee::pipeline::WoweeLiquidLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-liquids")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMagical(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MagicalLiquids";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWliqExt(base);
    auto c = wowee::pipeline::WoweeLiquidLoader::makeMagical(name);
    if (!saveOrError(c, base, "gen-liquids-magical")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenHazardous(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "HazardousLiquids";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWliqExt(base);
    auto c = wowee::pipeline::WoweeLiquidLoader::makeHazardous(name);
    if (!saveOrError(c, base, "gen-liquids-hazardous")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWliqExt(base);
    if (!wowee::pipeline::WoweeLiquidLoader::exists(base)) {
        std::fprintf(stderr, "WLIQ not found: %s.wliq\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeLiquidLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wliq"] = base + ".wliq";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"liquidId", e.liquidId},
                {"name", e.name},
                {"description", e.description},
                {"shaderPath", e.shaderPath},
                {"materialPath", e.materialPath},
                {"liquidKind", e.liquidKind},
                {"liquidKindName", wowee::pipeline::WoweeLiquid::liquidKindName(e.liquidKind)},
                {"fogColorR", e.fogColorR},
                {"fogColorG", e.fogColorG},
                {"fogColorB", e.fogColorB},
                {"fogDensity", e.fogDensity},
                {"ambientSoundId", e.ambientSoundId},
                {"splashSoundId", e.splashSoundId},
                {"damageSpellId", e.damageSpellId},
                {"damagePerSecond", e.damagePerSecond},
                {"minimapColor", e.minimapColor},
                {"flowDirection", e.flowDirection},
                {"flowSpeed", e.flowSpeed},
                {"viscosity", e.viscosity},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WLIQ: %s.wliq\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  liquids : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   kind         fog RGB         dens   visc   dps    spell   ambient  splash  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-10s   %3u/%3u/%3u   %4.2f   %4.2f  %5u  %5u    %5u    %5u   %s\n",
                    e.liquidId,
                    wowee::pipeline::WoweeLiquid::liquidKindName(e.liquidKind),
                    e.fogColorR, e.fogColorG, e.fogColorB,
                    e.fogDensity, e.viscosity,
                    e.damagePerSecond, e.damageSpellId,
                    e.ambientSoundId, e.splashSoundId,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWliqExt(base);
    if (!wowee::pipeline::WoweeLiquidLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wliq: WLIQ not found: %s.wliq\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeLiquidLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.liquidId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.liquidId == 0)
            errors.push_back(ctx + ": liquidId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.shaderPath.empty())
            errors.push_back(ctx + ": shaderPath is empty");
        if (e.materialPath.empty())
            errors.push_back(ctx + ": materialPath is empty");
        if (e.liquidKind > wowee::pipeline::WoweeLiquid::UnderworldGoo) {
            errors.push_back(ctx + ": liquidKind " +
                std::to_string(e.liquidKind) + " not in 0..9");
        }
        if (e.fogDensity < 0.0f || e.fogDensity > 1.0f) {
            errors.push_back(ctx + ": fogDensity " +
                std::to_string(e.fogDensity) + " not in 0..1");
        }
        if (e.viscosity < 0.0f || e.viscosity > 1.0f) {
            errors.push_back(ctx + ": viscosity " +
                std::to_string(e.viscosity) + " not in 0..1");
        }
        // Magma / Slime / FelFire / AcidBog liquids without
        // any damage source are mechanically harmless — flag
        // as a warning so the caller can confirm intent.
        bool hazardous =
            e.liquidKind == wowee::pipeline::WoweeLiquid::Magma ||
            e.liquidKind == wowee::pipeline::WoweeLiquid::Slime ||
            e.liquidKind == wowee::pipeline::WoweeLiquid::FelFire ||
            e.liquidKind == wowee::pipeline::WoweeLiquid::AcidBog;
        if (hazardous && e.damageSpellId == 0 &&
            e.damagePerSecond == 0) {
            warnings.push_back(ctx +
                ": hazardous liquid kind but no damageSpellId / "
                "damagePerSecond (won't hurt anything)");
        }
        // Water and OceanSalt with non-zero damage is unusual
        // — could be intentional acid water but worth checking.
        if ((e.liquidKind == wowee::pipeline::WoweeLiquid::Water ||
             e.liquidKind == wowee::pipeline::WoweeLiquid::OceanSalt) &&
            e.damagePerSecond > 0) {
            warnings.push_back(ctx +
                ": Water/OceanSalt with damagePerSecond>0 "
                "(unusual — verify intent)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.liquidId) {
                errors.push_back(ctx + ": duplicate liquidId");
                break;
            }
        }
        idsSeen.push_back(e.liquidId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wliq"] = base + ".wliq";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wliq: %s.wliq\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu liquids, all liquidIds unique\n",
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

bool handleLiquidsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-liquids") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-liquids-magical") == 0 && i + 1 < argc) {
        outRc = handleGenMagical(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-liquids-hazardous") == 0 && i + 1 < argc) {
        outRc = handleGenHazardous(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wliq") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wliq") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
