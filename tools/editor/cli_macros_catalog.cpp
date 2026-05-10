#include "cli_macros_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_macros.hpp"
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

std::string stripWmacExt(std::string base) {
    stripExt(base, ".wmac");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeMacro& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeMacroLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wmac\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeMacro& c,
                     const std::string& base) {
    std::printf("Wrote %s.wmac\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  macros  : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterMacros";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmacExt(base);
    auto c = wowee::pipeline::WoweeMacroLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-mac")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenCombat(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "CombatMacros";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmacExt(base);
    auto c = wowee::pipeline::WoweeMacroLoader::makeCombat(name);
    if (!saveOrError(c, base, "gen-mac-combat")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenUtility(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "UtilityMacros";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmacExt(base);
    auto c = wowee::pipeline::WoweeMacroLoader::makeUtility(name);
    if (!saveOrError(c, base, "gen-mac-utility")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWmacExt(base);
    if (!wowee::pipeline::WoweeMacroLoader::exists(base)) {
        std::fprintf(stderr, "WMAC not found: %s.wmac\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeMacroLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wmac"] = base + ".wmac";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"macroId", e.macroId},
                {"name", e.name},
                {"description", e.description},
                {"iconPath", e.iconPath},
                {"macroBody", e.macroBody},
                {"bindKey", e.bindKey},
                {"macroKind", e.macroKind},
                {"macroKindName", wowee::pipeline::WoweeMacro::macroKindName(e.macroKind)},
                {"requiredClassMask", e.requiredClassMask},
                {"maxLength", e.maxLength},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WMAC: %s.wmac\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  macros  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind             classMask    bindKey   maxLen  bodyLen  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-15s  0x%08x   %-8s   %5u   %6zu  %s\n",
                    e.macroId,
                    wowee::pipeline::WoweeMacro::macroKindName(e.macroKind),
                    e.requiredClassMask,
                    e.bindKey.empty() ? "-" : e.bindKey.c_str(),
                    e.maxLength, e.macroBody.size(),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWmacExt(base);
    if (!wowee::pipeline::WoweeMacroLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wmac: WMAC not found: %s.wmac\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeMacroLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.macroId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.macroId == 0)
            errors.push_back(ctx + ": macroId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.macroBody.empty())
            errors.push_back(ctx +
                ": macroBody is empty (macro does nothing)");
        if (e.macroKind > wowee::pipeline::WoweeMacro::SharedMacro) {
            errors.push_back(ctx + ": macroKind " +
                std::to_string(e.macroKind) + " not in 0..4");
        }
        if (e.maxLength != 0 && e.macroBody.size() > e.maxLength) {
            errors.push_back(ctx +
                ": macroBody length " +
                std::to_string(e.macroBody.size()) +
                " exceeds maxLength " +
                std::to_string(e.maxLength));
        }
        // Macro body must start with `/` (slash command) — any
        // line that doesn't is a stray comment / orphan text.
        if (!e.macroBody.empty() && e.macroBody[0] != '/' &&
            e.macroBody[0] != '#') {
            warnings.push_back(ctx +
                ": macroBody doesn't start with '/' or '#' "
                "(likely missing slash on first line)");
        }
        // SystemSlash macros are run by the engine, not by the
        // player — assigning a class restriction makes no sense
        // since the slash command is shared by all classes.
        if (e.macroKind == wowee::pipeline::WoweeMacro::SystemSlash &&
            e.requiredClassMask != 0) {
            warnings.push_back(ctx +
                ": SystemSlash kind with requiredClassMask != 0 "
                "(slash commands are class-agnostic)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.macroId) {
                errors.push_back(ctx + ": duplicate macroId");
                break;
            }
        }
        idsSeen.push_back(e.macroId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wmac"] = base + ".wmac";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wmac: %s.wmac\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu macros, all macroIds unique, "
                    "all bodies within length cap\n", c.entries.size());
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

bool handleMacrosCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-mac") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mac-combat") == 0 && i + 1 < argc) {
        outRc = handleGenCombat(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mac-utility") == 0 && i + 1 < argc) {
        outRc = handleGenUtility(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wmac") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wmac") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
