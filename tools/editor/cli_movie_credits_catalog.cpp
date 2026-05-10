#include "cli_movie_credits_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_movie_credits.hpp"
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

std::string stripWmvcExt(std::string base) {
    stripExt(base, ".wmvc");
    return base;
}

const char* categoryName(uint8_t k) {
    using M = wowee::pipeline::WoweeMovieCredits;
    switch (k) {
        case M::Production:  return "production";
        case M::Music:       return "music";
        case M::Audio:       return "audio";
        case M::Engineering: return "engineering";
        case M::Art:         return "art";
        case M::Voice:       return "voice";
        case M::Special:     return "special";
        default:             return "unknown";
    }
}

bool saveOrError(const wowee::pipeline::WoweeMovieCredits& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeMovieCreditsLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wmvc\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeMovieCredits& c,
                     const std::string& base) {
    size_t totalLines = 0;
    for (const auto& e : c.entries) totalLines += e.lines.size();
    std::printf("Wrote %s.wmvc\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  blocks  : %zu (%zu lines total)\n",
                c.entries.size(), totalLines);
}

int handleGenWotLK(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WotLKIntroCredits";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmvcExt(base);
    auto c = wowee::pipeline::WoweeMovieCreditsLoader::makeWotLKIntro(name);
    if (!saveOrError(c, base, "gen-mvc")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenQuest(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "QuestCinematicCredits";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmvcExt(base);
    auto c = wowee::pipeline::WoweeMovieCreditsLoader::makeQuestCinema(name);
    if (!saveOrError(c, base, "gen-mvc-quest")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterRollCredits";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmvcExt(base);
    auto c = wowee::pipeline::WoweeMovieCreditsLoader::makeStarterRoll(name);
    if (!saveOrError(c, base, "gen-mvc-starter")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWmvcExt(base);
    if (!wowee::pipeline::WoweeMovieCreditsLoader::exists(base)) {
        std::fprintf(stderr, "WMVC not found: %s.wmvc\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeMovieCreditsLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wmvc"] = base + ".wmvc";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"rollId", e.rollId},
                {"name", e.name},
                {"description", e.description},
                {"cinematicId", e.cinematicId},
                {"category", e.category},
                {"categoryName", categoryName(e.category)},
                {"orderHint", e.orderHint},
                {"iconColorRGBA", e.iconColorRGBA},
                {"lines", e.lines},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WMVC: %s.wmvc\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  blocks  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   cinematic  category       order  lines  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u    %5u     %-10s     %4u   %4zu   %s\n",
                    e.rollId, e.cinematicId,
                    categoryName(e.category),
                    e.orderHint, e.lines.size(),
                    e.name.c_str());
        for (const auto& L : e.lines) {
            std::printf("           | %s\n", L.c_str());
        }
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWmvcExt(base);
    if (!wowee::pipeline::WoweeMovieCreditsLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wmvc: WMVC not found: %s.wmvc\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeMovieCreditsLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    // Per-cinematic orderHint uniqueness — two blocks at
    // the same orderHint within one cinematic would
    // render in unstable order.
    std::set<uint64_t> orderSlotsSeen;
    auto orderKey = [](uint32_t cine, uint16_t order) {
        return (static_cast<uint64_t>(cine) << 32) | order;
    };
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.rollId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.rollId == 0)
            errors.push_back(ctx + ": rollId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.cinematicId == 0) {
            errors.push_back(ctx +
                ": cinematicId is 0 — credit block is "
                "unbound to any cinematic");
        }
        if (e.category > 6) {
            errors.push_back(ctx + ": category " +
                std::to_string(e.category) +
                " out of range (must be 0..6)");
        }
        if (e.lines.empty()) {
            errors.push_back(ctx +
                ": lines[] is empty — credit block has "
                "nothing to display");
        }
        // Per-line length sanity. WoW cinematic credit
        // line buffer is ~80 chars wide before wrap.
        for (size_t L = 0; L < e.lines.size(); ++L) {
            if (e.lines[L].empty()) {
                warnings.push_back(ctx +
                    ": lines[" + std::to_string(L) +
                    "] is empty — would render as a "
                    "blank line in the credit roll "
                    "(intentional spacers should still "
                    "have a placeholder character)");
            }
            if (e.lines[L].size() > 80) {
                warnings.push_back(ctx + ": lines[" +
                    std::to_string(L) +
                    "] is " + std::to_string(e.lines[L].size()) +
                    " chars (>80) — may wrap or truncate "
                    "in the credit-renderer 80-char text "
                    "buffer");
            }
        }
        if (e.cinematicId != 0) {
            uint64_t key = orderKey(e.cinematicId, e.orderHint);
            if (!orderSlotsSeen.insert(key).second) {
                errors.push_back(ctx +
                    ": (cinematicId=" +
                    std::to_string(e.cinematicId) +
                    ", orderHint=" +
                    std::to_string(e.orderHint) +
                    ") slot already occupied by another "
                    "block — credit roll order would be "
                    "non-deterministic");
            }
        }
        if (!idsSeen.insert(e.rollId).second) {
            errors.push_back(ctx + ": duplicate rollId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wmvc"] = base + ".wmvc";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wmvc: %s.wmvc\n", base.c_str());
    if (ok && warnings.empty()) {
        size_t totalLines = 0;
        for (const auto& e : c.entries) totalLines += e.lines.size();
        std::printf("  OK — %zu blocks, %zu lines, all "
                    "rollIds + per-cinematic orderHints "
                    "unique\n",
                    c.entries.size(), totalLines);
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

bool handleMovieCreditsCatalog(int& i, int argc, char** argv,
                                int& outRc) {
    if (std::strcmp(argv[i], "--gen-mvc") == 0 && i + 1 < argc) {
        outRc = handleGenWotLK(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mvc-quest") == 0 && i + 1 < argc) {
        outRc = handleGenQuest(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mvc-starter") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wmvc") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wmvc") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
