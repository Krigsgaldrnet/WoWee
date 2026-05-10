#include "cli_keybindings_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_keybindings.hpp"
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

std::string stripWkbdExt(std::string base) {
    stripExt(base, ".wkbd");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeKeyBinding& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeKeyBindingLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wkbd\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeKeyBinding& c,
                     const std::string& base) {
    std::printf("Wrote %s.wkbd\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  bindings : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterKeybindings";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWkbdExt(base);
    auto c = wowee::pipeline::WoweeKeyBindingLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-kbd")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMovement(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MovementKeybindings";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWkbdExt(base);
    auto c = wowee::pipeline::WoweeKeyBindingLoader::makeMovement(name);
    if (!saveOrError(c, base, "gen-kbd-movement")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenUI(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "UIPanelKeybindings";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWkbdExt(base);
    auto c = wowee::pipeline::WoweeKeyBindingLoader::makeUIPanels(name);
    if (!saveOrError(c, base, "gen-kbd-ui")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWkbdExt(base);
    if (!wowee::pipeline::WoweeKeyBindingLoader::exists(base)) {
        std::fprintf(stderr, "WKBD not found: %s.wkbd\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeKeyBindingLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wkbd"] = base + ".wkbd";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"bindingId", e.bindingId},
                {"actionName", e.actionName},
                {"description", e.description},
                {"defaultKey", e.defaultKey},
                {"alternateKey", e.alternateKey},
                {"category", e.category},
                {"categoryName", wowee::pipeline::WoweeKeyBinding::categoryName(e.category)},
                {"isUserOverridable", e.isUserOverridable},
                {"sortOrder", e.sortOrder},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WKBD: %s.wkbd\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  bindings : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    category    user   sort   default       alt          actionName\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-9s    %u     %3u   %-12s  %-10s   %s\n",
                    e.bindingId,
                    wowee::pipeline::WoweeKeyBinding::categoryName(e.category),
                    e.isUserOverridable, e.sortOrder,
                    e.defaultKey.c_str(),
                    e.alternateKey.empty() ? "-" : e.alternateKey.c_str(),
                    e.actionName.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWkbdExt(base);
    if (!wowee::pipeline::WoweeKeyBindingLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wkbd: WKBD not found: %s.wkbd\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeKeyBindingLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    std::set<std::string> actionsSeen;
    std::set<std::string> primaryKeysSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.bindingId);
        if (!e.actionName.empty()) ctx += " " + e.actionName;
        ctx += ")";
        if (e.bindingId == 0)
            errors.push_back(ctx + ": bindingId is 0");
        if (e.actionName.empty())
            errors.push_back(ctx + ": actionName is empty");
        if (e.defaultKey.empty())
            errors.push_back(ctx + ": defaultKey is empty");
        if (e.category > wowee::pipeline::WoweeKeyBinding::Other) {
            errors.push_back(ctx + ": category " +
                std::to_string(e.category) + " not in 0..8");
        }
        if (e.alternateKey == e.defaultKey && !e.defaultKey.empty()) {
            errors.push_back(ctx +
                ": alternateKey == defaultKey (no point in alt)");
        }
        // Action name should be SCREAMING_SNAKE — anything
        // with lowercase is suspect.
        for (char ch : e.actionName) {
            if (ch >= 'a' && ch <= 'z') {
                warnings.push_back(ctx +
                    ": actionName contains lowercase chars "
                    "(convention is SCREAMING_SNAKE)");
                break;
            }
        }
        // Duplicate primary keys would conflict at runtime —
        // last one binding loaded wins, leaving the first
        // silently shadowed.
        if (!e.defaultKey.empty()) {
            if (primaryKeysSeen.count(e.defaultKey)) {
                errors.push_back(ctx + ": defaultKey '" +
                    e.defaultKey +
                    "' already bound by an earlier entry "
                    "(would shadow that binding)");
            }
            primaryKeysSeen.insert(e.defaultKey);
        }
        if (!e.actionName.empty()) {
            if (actionsSeen.count(e.actionName)) {
                errors.push_back(ctx +
                    ": duplicate actionName '" + e.actionName + "'");
            }
            actionsSeen.insert(e.actionName);
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.bindingId) {
                errors.push_back(ctx + ": duplicate bindingId");
                break;
            }
        }
        idsSeen.push_back(e.bindingId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wkbd"] = base + ".wkbd";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wkbd: %s.wkbd\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu bindings, all bindingIds unique, "
                    "no key conflicts\n", c.entries.size());
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

bool handleKeybindingsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-kbd") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-kbd-movement") == 0 && i + 1 < argc) {
        outRc = handleGenMovement(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-kbd-ui") == 0 && i + 1 < argc) {
        outRc = handleGenUI(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wkbd") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wkbd") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
