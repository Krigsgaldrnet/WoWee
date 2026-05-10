#include "cli_titles_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_titles.hpp"
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

std::string stripWtitExt(std::string base) {
    stripExt(base, ".wtit");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeTitle& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeTitleLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wtit\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeTitle& c,
                     const std::string& base) {
    std::printf("Wrote %s.wtit\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  titles  : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterTitles";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtitExt(base);
    auto c = wowee::pipeline::WoweeTitleLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-titles")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenPvp(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "PvpTitles";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtitExt(base);
    auto c = wowee::pipeline::WoweeTitleLoader::makePvp(name);
    if (!saveOrError(c, base, "gen-titles-pvp")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenAchievement(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AchievementTitles";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtitExt(base);
    auto c = wowee::pipeline::WoweeTitleLoader::makeAchievement(name);
    if (!saveOrError(c, base, "gen-titles-achievement")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWtitExt(base);
    if (!wowee::pipeline::WoweeTitleLoader::exists(base)) {
        std::fprintf(stderr, "WTIT not found: %s.wtit\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeTitleLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wtit"] = base + ".wtit";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"titleId", e.titleId},
                {"name", e.name},
                {"nameMale", e.nameMale},
                {"nameFemale", e.nameFemale},
                {"iconPath", e.iconPath},
                {"prefix", e.prefix},
                {"prefixName", e.prefix ? "prefix" : "suffix"},
                {"category", e.category},
                {"categoryName", wowee::pipeline::WoweeTitle::categoryName(e.category)},
                {"sortOrder", e.sortOrder},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WTIT: %s.wtit\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  titles  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   sort   pos     category      name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %4u   %-6s  %-12s  %s\n",
                    e.titleId, e.sortOrder,
                    e.prefix ? "prefix" : "suffix",
                    wowee::pipeline::WoweeTitle::categoryName(e.category),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWtitExt(base);
    if (!wowee::pipeline::WoweeTitleLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wtit: WTIT not found: %s.wtit\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeTitleLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.titleId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.titleId == 0) errors.push_back(ctx + ": titleId is 0");
        if (e.name.empty()) errors.push_back(ctx + ": name is empty");
        if (e.category > wowee::pipeline::WoweeTitle::Custom) {
            errors.push_back(ctx + ": category " +
                std::to_string(e.category) + " not in 0..7");
        }
        // If gender variants are set, both should be set (otherwise
        // the runtime fall-back to canonical leaves one gender
        // displaying the wrong form).
        if (!e.nameMale.empty() && e.nameFemale.empty()) {
            warnings.push_back(ctx +
                ": nameMale set but nameFemale empty (mixed-gender display)");
        }
        if (!e.nameFemale.empty() && e.nameMale.empty()) {
            warnings.push_back(ctx +
                ": nameFemale set but nameMale empty (mixed-gender display)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.titleId) {
                errors.push_back(ctx + ": duplicate titleId");
                break;
            }
        }
        idsSeen.push_back(e.titleId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wtit"] = base + ".wtit";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wtit: %s.wtit\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu titles, all titleIds unique\n",
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

bool handleTitlesCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-titles") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-titles-pvp") == 0 && i + 1 < argc) {
        outRc = handleGenPvp(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-titles-achievement") == 0 && i + 1 < argc) {
        outRc = handleGenAchievement(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wtit") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wtit") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
