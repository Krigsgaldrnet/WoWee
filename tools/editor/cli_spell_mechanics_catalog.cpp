#include "cli_spell_mechanics_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_spell_mechanics.hpp"
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

std::string stripWsmcExt(std::string base) {
    stripExt(base, ".wsmc");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeSpellMechanic& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSpellMechanicLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wsmc\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSpellMechanic& c,
                     const std::string& base) {
    std::printf("Wrote %s.wsmc\n", base.c_str());
    std::printf("  catalog   : %s\n", c.name.c_str());
    std::printf("  mechanics : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterMechanics";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsmcExt(base);
    auto c = wowee::pipeline::WoweeSpellMechanicLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-smc")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenHardCC(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "HardCCMechanics";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsmcExt(base);
    auto c = wowee::pipeline::WoweeSpellMechanicLoader::makeHardCC(name);
    if (!saveOrError(c, base, "gen-smc-hard")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRoots(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RootMechanics";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsmcExt(base);
    auto c = wowee::pipeline::WoweeSpellMechanicLoader::makeRoots(name);
    if (!saveOrError(c, base, "gen-smc-roots")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsmcExt(base);
    if (!wowee::pipeline::WoweeSpellMechanicLoader::exists(base)) {
        std::fprintf(stderr, "WSMC not found: %s.wsmc\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellMechanicLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wsmc"] = base + ".wsmc";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"mechanicId", e.mechanicId},
                {"name", e.name},
                {"description", e.description},
                {"iconPath", e.iconPath},
                {"breaksOnDamage", e.breaksOnDamage},
                {"canBeDispelled", e.canBeDispelled},
                {"drCategory", e.drCategory},
                {"drCategoryName", wowee::pipeline::WoweeSpellMechanic::drCategoryName(e.drCategory)},
                {"dispelType", e.dispelType},
                {"dispelTypeName", wowee::pipeline::WoweeSpellMechanic::dispelTypeName(e.dispelType)},
                {"defaultDurationMs", e.defaultDurationMs},
                {"maxStacks", e.maxStacks},
                {"conflictsMask", e.conflictsMask},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSMC: %s.wsmc\n", base.c_str());
    std::printf("  catalog   : %s\n", c.name.c_str());
    std::printf("  mechanics : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    DR-cat       dispel    breaks  disp  dur(ms)  stack  conflicts   name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-10s   %-8s    %u       %u    %5u     %3u   0x%08x  %s\n",
                    e.mechanicId,
                    wowee::pipeline::WoweeSpellMechanic::drCategoryName(e.drCategory),
                    wowee::pipeline::WoweeSpellMechanic::dispelTypeName(e.dispelType),
                    e.breaksOnDamage, e.canBeDispelled,
                    e.defaultDurationMs, e.maxStacks,
                    e.conflictsMask, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsmcExt(base);
    if (!wowee::pipeline::WoweeSpellMechanicLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wsmc: WSMC not found: %s.wsmc\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellMechanicLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.mechanicId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.mechanicId == 0)
            errors.push_back(ctx + ": mechanicId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.drCategory > wowee::pipeline::WoweeSpellMechanic::DRMisc) {
            errors.push_back(ctx + ": drCategory " +
                std::to_string(e.drCategory) + " not in 0..7");
        }
        if (e.dispelType > wowee::pipeline::WoweeSpellMechanic::DispelStealth) {
            errors.push_back(ctx + ": dispelType " +
                std::to_string(e.dispelType) + " not in 0..6");
        }
        if (e.maxStacks == 0) {
            errors.push_back(ctx +
                ": maxStacks=0 (mechanic could never apply)");
        }
        // canBeDispelled=1 with dispelType=None is contradictory
        // — without a dispel category, no spell can target this
        // mechanic for removal.
        if (e.canBeDispelled &&
            e.dispelType == wowee::pipeline::WoweeSpellMechanic::DispelNone) {
            errors.push_back(ctx +
                ": canBeDispelled=1 but dispelType=none "
                "(no dispel spell can target this)");
        }
        // A mechanic that conflicts with itself is wrong —
        // `conflictsMask & (1 << mechanicId)` would mean the
        // mechanic blocks itself.
        if (e.mechanicId < 32 &&
            (e.conflictsMask & (1u << e.mechanicId))) {
            errors.push_back(ctx +
                ": conflictsMask includes own mechanicId bit "
                "(mechanic conflicts with itself)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.mechanicId) {
                errors.push_back(ctx + ": duplicate mechanicId");
                break;
            }
        }
        idsSeen.push_back(e.mechanicId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wsmc"] = base + ".wsmc";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wsmc: %s.wsmc\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu mechanics, all mechanicIds unique\n",
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

bool handleSpellMechanicsCatalog(int& i, int argc, char** argv,
                                 int& outRc) {
    if (std::strcmp(argv[i], "--gen-smc") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-smc-hard") == 0 && i + 1 < argc) {
        outRc = handleGenHardCC(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-smc-roots") == 0 && i + 1 < argc) {
        outRc = handleGenRoots(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wsmc") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wsmc") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
