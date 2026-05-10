#include "cli_game_tips_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_game_tips.hpp"
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

std::string stripWgtpExt(std::string base) {
    stripExt(base, ".wgtp");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeGameTip& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeGameTipLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wgtp\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeGameTip& c,
                     const std::string& base) {
    std::printf("Wrote %s.wgtp\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  tips    : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterTips";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgtpExt(base);
    auto c = wowee::pipeline::WoweeGameTipLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-tips")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenNewPlayer(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "NewPlayerTips";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgtpExt(base);
    auto c = wowee::pipeline::WoweeGameTipLoader::makeNewPlayer(name);
    if (!saveOrError(c, base, "gen-tips-new-player")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenAdvanced(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AdvancedTips";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgtpExt(base);
    auto c = wowee::pipeline::WoweeGameTipLoader::makeAdvanced(name);
    if (!saveOrError(c, base, "gen-tips-advanced")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgtpExt(base);
    if (!wowee::pipeline::WoweeGameTipLoader::exists(base)) {
        std::fprintf(stderr, "WGTP not found: %s.wgtp\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGameTipLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wgtp"] = base + ".wgtp";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"tipId", e.tipId},
                {"name", e.name},
                {"text", e.text},
                {"iconPath", e.iconPath},
                {"displayKind", e.displayKind},
                {"displayKindName", wowee::pipeline::WoweeGameTip::displayKindName(e.displayKind)},
                {"audienceFilter", e.audienceFilter},
                {"minLevel", e.minLevel},
                {"maxLevel", e.maxLevel},
                {"displayWeight", e.displayWeight},
                {"conditionId", e.conditionId},
                {"requiredClassMask", e.requiredClassMask},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WGTP: %s.wgtp\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  tips    : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind            audience    levels    wt   cond   classMask  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-13s   0x%08x  %3u-%3u   %3u  %5u   0x%08x  %s\n",
                    e.tipId,
                    wowee::pipeline::WoweeGameTip::displayKindName(e.displayKind),
                    e.audienceFilter,
                    e.minLevel, e.maxLevel,
                    e.displayWeight, e.conditionId,
                    e.requiredClassMask,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgtpExt(base);
    if (!wowee::pipeline::WoweeGameTipLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wgtp: WGTP not found: %s.wgtp\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGameTipLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.tipId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.tipId == 0)
            errors.push_back(ctx + ": tipId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.text.empty())
            errors.push_back(ctx + ": text is empty");
        if (e.displayKind > wowee::pipeline::WoweeGameTip::Hint) {
            errors.push_back(ctx + ": displayKind " +
                std::to_string(e.displayKind) + " not in 0..3");
        }
        if (e.audienceFilter == 0) {
            errors.push_back(ctx +
                ": audienceFilter=0 (tip would never be shown)");
        }
        if (e.minLevel > e.maxLevel) {
            errors.push_back(ctx + ": minLevel " +
                std::to_string(e.minLevel) + " > maxLevel " +
                std::to_string(e.maxLevel));
        }
        if (e.displayWeight == 0) {
            warnings.push_back(ctx +
                ": displayWeight=0 (tip is in pool but never picked)");
        }
        // Tutorial / Hint kinds typically need to be brief —
        // > 280 characters won't fit cleanly on screen.
        bool brief = e.displayKind == wowee::pipeline::WoweeGameTip::Tutorial ||
                      e.displayKind == wowee::pipeline::WoweeGameTip::Hint;
        if (brief && e.text.size() > 280) {
            warnings.push_back(ctx +
                ": text length " + std::to_string(e.text.size()) +
                " exceeds 280 chars (tutorial/hint should be brief)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.tipId) {
                errors.push_back(ctx + ": duplicate tipId");
                break;
            }
        }
        idsSeen.push_back(e.tipId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wgtp"] = base + ".wgtp";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wgtp: %s.wgtp\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu tips, all tipIds unique, all level ranges valid\n",
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

bool handleGameTipsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-tips") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-tips-new-player") == 0 &&
        i + 1 < argc) {
        outRc = handleGenNewPlayer(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-tips-advanced") == 0 &&
        i + 1 < argc) {
        outRc = handleGenAdvanced(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wgtp") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wgtp") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
