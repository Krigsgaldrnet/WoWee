#include "cli_stat_curves_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_stat_curves.hpp"
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

std::string stripWstmExt(std::string base) {
    stripExt(base, ".wstm");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeStatCurve& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeStatCurveLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wstm\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeStatCurve& c,
                     const std::string& base) {
    std::printf("Wrote %s.wstm\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  curves  : %zu\n", c.entries.size());
}

int handleGenCrit(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "CritCurves";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWstmExt(base);
    auto c = wowee::pipeline::WoweeStatCurveLoader::makeCrit(name);
    if (!saveOrError(c, base, "gen-stm")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRegen(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RegenCurves";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWstmExt(base);
    auto c = wowee::pipeline::WoweeStatCurveLoader::makeRegen(name);
    if (!saveOrError(c, base, "gen-stm-regen")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenArmor(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ArmorCurves";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWstmExt(base);
    auto c = wowee::pipeline::WoweeStatCurveLoader::makeArmor(name);
    if (!saveOrError(c, base, "gen-stm-armor")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWstmExt(base);
    if (!wowee::pipeline::WoweeStatCurveLoader::exists(base)) {
        std::fprintf(stderr, "WSTM not found: %s.wstm\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeStatCurveLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wstm"] = base + ".wstm";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"curveId", e.curveId},
                {"name", e.name},
                {"description", e.description},
                {"curveKind", e.curveKind},
                {"curveKindName", wowee::pipeline::WoweeStatCurve::curveKindName(e.curveKind)},
                {"minLevel", e.minLevel},
                {"maxLevel", e.maxLevel},
                {"baseValue", e.baseValue},
                {"perLevelDelta", e.perLevelDelta},
                {"multiplier", e.multiplier},
                {"valueAtLevel80", c.resolveAtLevel(e.curveId, 80)},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSTM: %s.wstm\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  curves  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind         minL  maxL    base      perLvl     mult    @lvl80    name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u    %-10s   %3u   %3u    %7.3f   %7.4f   %5.2f   %7.3f   %s\n",
                    e.curveId,
                    wowee::pipeline::WoweeStatCurve::curveKindName(e.curveKind),
                    e.minLevel, e.maxLevel,
                    e.baseValue, e.perLevelDelta, e.multiplier,
                    c.resolveAtLevel(e.curveId, 80),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWstmExt(base);
    if (!wowee::pipeline::WoweeStatCurveLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wstm: WSTM not found: %s.wstm\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeStatCurveLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.curveId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.curveId == 0)
            errors.push_back(ctx + ": curveId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.curveKind > wowee::pipeline::WoweeStatCurve::Misc) {
            errors.push_back(ctx + ": curveKind " +
                std::to_string(e.curveKind) + " not in 0..6");
        }
        if (e.minLevel > e.maxLevel) {
            errors.push_back(ctx + ": minLevel " +
                std::to_string(e.minLevel) +
                " > maxLevel " + std::to_string(e.maxLevel) +
                " — curve will never apply");
        }
        if (e.maxLevel > 80) {
            warnings.push_back(ctx +
                ": maxLevel " + std::to_string(e.maxLevel) +
                " > 80 — characters cap at 80 in WotLK");
        }
        if (e.multiplier == 0.0f)
            warnings.push_back(ctx +
                ": multiplier=0 — curve always evaluates to 0");
        if (e.multiplier < 0.0f)
            warnings.push_back(ctx +
                ": multiplier=" + std::to_string(e.multiplier) +
                " (< 0) — inverts the curve, double-check this "
                "is intentional");
        // Negative perLevelDelta is unusual — most stats
        // grow with level.
        if (e.perLevelDelta < 0.0f)
            warnings.push_back(ctx +
                ": perLevelDelta=" + std::to_string(e.perLevelDelta) +
                " (< 0) — curve shrinks with level, double-check");
        for (uint32_t prev : idsSeen) {
            if (prev == e.curveId) {
                errors.push_back(ctx + ": duplicate curveId");
                break;
            }
        }
        idsSeen.push_back(e.curveId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wstm"] = base + ".wstm";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wstm: %s.wstm\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu curves, all curveIds unique, all minLevel<=maxLevel\n",
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

bool handleStatCurvesCatalog(int& i, int argc, char** argv,
                             int& outRc) {
    if (std::strcmp(argv[i], "--gen-stm") == 0 && i + 1 < argc) {
        outRc = handleGenCrit(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-stm-regen") == 0 && i + 1 < argc) {
        outRc = handleGenRegen(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-stm-armor") == 0 && i + 1 < argc) {
        outRc = handleGenArmor(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wstm") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wstm") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
