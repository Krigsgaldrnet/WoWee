#include "cli_word_filters_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_word_filters.hpp"
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

std::string stripWwflExt(std::string base) {
    stripExt(base, ".wwfl");
    return base;
}

const char* filterKindName(uint8_t k) {
    using F = wowee::pipeline::WoweeWordFilters;
    switch (k) {
        case F::Spam:         return "spam";
        case F::GoldSeller:   return "goldseller";
        case F::AllCaps:      return "allcaps";
        case F::RepeatChar:   return "repeatchar";
        case F::URL:          return "url";
        case F::AdvertReward: return "advertreward";
        case F::Misc:         return "misc";
        default:              return "unknown";
    }
}

const char* severityName(uint8_t s) {
    using F = wowee::pipeline::WoweeWordFilters;
    switch (s) {
        case F::Warn:    return "warn";
        case F::Replace: return "replace";
        case F::Drop:    return "drop";
        case F::Mute:    return "mute";
        default:         return "unknown";
    }
}

bool saveOrError(const wowee::pipeline::WoweeWordFilters& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeWordFiltersLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wwfl\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeWordFilters& c,
                     const std::string& base) {
    std::printf("Wrote %s.wwfl\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  filters : %zu\n", c.entries.size());
}

int handleGenSpam(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "SpamRMTFilters";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWwflExt(base);
    auto c = wowee::pipeline::WoweeWordFiltersLoader::makeSpamRMT(name);
    if (!saveOrError(c, base, "gen-wfl")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenCaps(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AllCapsFilters";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWwflExt(base);
    auto c = wowee::pipeline::WoweeWordFiltersLoader::makeAllCaps(name);
    if (!saveOrError(c, base, "gen-wfl-caps")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenURL(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "URLDetectFilters";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWwflExt(base);
    auto c = wowee::pipeline::WoweeWordFiltersLoader::makeURLDetect(name);
    if (!saveOrError(c, base, "gen-wfl-url")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWwflExt(base);
    if (!wowee::pipeline::WoweeWordFiltersLoader::exists(base)) {
        std::fprintf(stderr, "WWFL not found: %s.wwfl\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeWordFiltersLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wwfl"] = base + ".wwfl";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"filterId", e.filterId},
                {"name", e.name},
                {"description", e.description},
                {"pattern", e.pattern},
                {"replacement", e.replacement},
                {"filterKind", e.filterKind},
                {"filterKindName", filterKindName(e.filterKind)},
                {"severity", e.severity},
                {"severityName", severityName(e.severity)},
                {"caseSensitive", e.caseSensitive != 0},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WWFL: %s.wwfl\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  filters : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   kind          severity   caseS   pattern -> replacement   name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-11s   %-7s    %s     '%s' -> '%s'   %s\n",
                    e.filterId, filterKindName(e.filterKind),
                    severityName(e.severity),
                    e.caseSensitive ? "yes" : "no ",
                    e.pattern.c_str(), e.replacement.c_str(),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWwflExt(base);
    if (!wowee::pipeline::WoweeWordFiltersLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wwfl: WWFL not found: %s.wwfl\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeWordFiltersLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    std::set<std::string> patternsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.filterId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.filterId == 0)
            errors.push_back(ctx + ": filterId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.pattern.empty()) {
            errors.push_back(ctx +
                ": pattern is empty — filter would match "
                "nothing (or every message, depending on "
                "the matcher's empty-string semantics)");
        }
        if (e.filterKind > 5 && e.filterKind != 255) {
            errors.push_back(ctx + ": filterKind " +
                std::to_string(e.filterKind) +
                " out of range (must be 0..5 or 255 Misc)");
        }
        if (e.severity > 3) {
            errors.push_back(ctx + ": severity " +
                std::to_string(e.severity) +
                " out of range (must be 0..3)");
        }
        // Per-severity validity: Replace severity REQUIRES
        // a non-empty replacement (else the substitution
        // would just delete the matched portion, which is
        // Drop semantics).
        using F = wowee::pipeline::WoweeWordFilters;
        if (e.severity == F::Replace && e.replacement.empty()) {
            warnings.push_back(ctx +
                ": Replace severity with empty "
                "replacement — message would silently lose "
                "the matched substring (effectively Drop "
                "semantics for that span). Use severity="
                "Drop explicitly if that's the intent.");
        }
        // Pattern uniqueness — two filters with the same
        // pattern would fire ambiguously.
        if (!e.pattern.empty() &&
            !patternsSeen.insert(e.pattern).second) {
            errors.push_back(ctx +
                ": pattern '" + e.pattern +
                "' already used by another filter — "
                "preprocessor dispatch would be "
                "non-deterministic");
        }
        if (!idsSeen.insert(e.filterId).second) {
            errors.push_back(ctx + ": duplicate filterId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wwfl"] = base + ".wwfl";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wwfl: %s.wwfl\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu filters, all filterIds + "
                    "patterns unique\n", c.entries.size());
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

bool handleWordFiltersCatalog(int& i, int argc, char** argv,
                                int& outRc) {
    if (std::strcmp(argv[i], "--gen-wfl") == 0 && i + 1 < argc) {
        outRc = handleGenSpam(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-wfl-caps") == 0 && i + 1 < argc) {
        outRc = handleGenCaps(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-wfl-url") == 0 && i + 1 < argc) {
        outRc = handleGenURL(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wwfl") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wwfl") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
