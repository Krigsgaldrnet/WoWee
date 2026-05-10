#include "cli_world_state_ui_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_world_state_ui.hpp"
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

std::string stripWwuiExt(std::string base) {
    stripExt(base, ".wwui");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeWorldStateUI& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeWorldStateUILoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wwui\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeWorldStateUI& c,
                     const std::string& base) {
    std::printf("Wrote %s.wwui\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  states  : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterWorldStateUI";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWwuiExt(base);
    auto c = wowee::pipeline::WoweeWorldStateUILoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-wsui")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenWintergrasp(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WintergraspUI";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWwuiExt(base);
    auto c = wowee::pipeline::WoweeWorldStateUILoader::makeWintergrasp(name);
    if (!saveOrError(c, base, "gen-wsui-wintergrasp")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenDungeon(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "DungeonUI";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWwuiExt(base);
    auto c = wowee::pipeline::WoweeWorldStateUILoader::makeDungeon(name);
    if (!saveOrError(c, base, "gen-wsui-dungeon")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWwuiExt(base);
    if (!wowee::pipeline::WoweeWorldStateUILoader::exists(base)) {
        std::fprintf(stderr, "WWUI not found: %s.wwui\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeWorldStateUILoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wwui"] = base + ".wwui";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"worldStateId", e.worldStateId},
                {"name", e.name},
                {"description", e.description},
                {"iconPath", e.iconPath},
                {"displayKind", e.displayKind},
                {"displayKindName", wowee::pipeline::WoweeWorldStateUI::displayKindName(e.displayKind)},
                {"panelPosition", e.panelPosition},
                {"panelPositionName", wowee::pipeline::WoweeWorldStateUI::panelPositionName(e.panelPosition)},
                {"alwaysVisible", e.alwaysVisible},
                {"hideWhenZero", e.hideWhenZero},
                {"mapId", e.mapId},
                {"areaId", e.areaId},
                {"variableIndex", e.variableIndex},
                {"defaultValue", e.defaultValue},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WWUI: %s.wwui\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  states  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind          pos        always  hideZ  map    area    var   default  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-13s %-9s    %u       %u    %5u  %5u   %3u   %6d   %s\n",
                    e.worldStateId,
                    wowee::pipeline::WoweeWorldStateUI::displayKindName(e.displayKind),
                    wowee::pipeline::WoweeWorldStateUI::panelPositionName(e.panelPosition),
                    e.alwaysVisible, e.hideWhenZero,
                    e.mapId, e.areaId, e.variableIndex,
                    e.defaultValue, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWwuiExt(base);
    if (!wowee::pipeline::WoweeWorldStateUILoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wwui: WWUI not found: %s.wwui\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeWorldStateUILoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    std::vector<uint32_t> varsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.worldStateId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.worldStateId == 0)
            errors.push_back(ctx + ": worldStateId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.displayKind > wowee::pipeline::WoweeWorldStateUI::Custom) {
            errors.push_back(ctx + ": displayKind " +
                std::to_string(e.displayKind) + " not in 0..5");
        }
        if (e.panelPosition > wowee::pipeline::WoweeWorldStateUI::Center) {
            errors.push_back(ctx + ": panelPosition " +
                std::to_string(e.panelPosition) + " not in 0..4");
        }
        if (e.variableIndex == 0) {
            warnings.push_back(ctx + ": variableIndex=0 "
                "(not bound to a server-side variable)");
        }
        // alwaysVisible + hideWhenZero is contradictory —
        // hide-when-zero implicitly negates always-visible
        // when the value happens to be 0.
        if (e.alwaysVisible && e.hideWhenZero) {
            warnings.push_back(ctx +
                ": both alwaysVisible and hideWhenZero set "
                "(hideWhenZero wins when value=0)");
        }
        // Two world-state entries sharing the same
        // (mapId, variableIndex) pair conflict — they'd both
        // try to read the same server slot at the same time.
        for (size_t m = 0; m < k; ++m) {
            const auto& other = c.entries[m];
            if (other.mapId == e.mapId && e.mapId != 0 &&
                other.variableIndex == e.variableIndex &&
                e.variableIndex != 0) {
                warnings.push_back(ctx +
                    ": shares (mapId=" + std::to_string(e.mapId) +
                    ", variableIndex=" +
                    std::to_string(e.variableIndex) +
                    ") with entry " + std::to_string(m) +
                    " — values will collide");
                break;
            }
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.worldStateId) {
                errors.push_back(ctx + ": duplicate worldStateId");
                break;
            }
        }
        idsSeen.push_back(e.worldStateId);
        varsSeen.push_back(e.variableIndex);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wwui"] = base + ".wwui";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wwui: %s.wwui\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu world states, all worldStateIds unique\n",
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

bool handleWorldStateUICatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-wsui") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-wsui-wintergrasp") == 0 &&
        i + 1 < argc) {
        outRc = handleGenWintergrasp(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-wsui-dungeon") == 0 &&
        i + 1 < argc) {
        outRc = handleGenDungeon(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wwui") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wwui") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
