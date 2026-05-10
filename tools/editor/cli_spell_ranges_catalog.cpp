#include "cli_spell_ranges_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_spell_ranges.hpp"
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

std::string stripWsrgExt(std::string base) {
    stripExt(base, ".wsrg");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeSpellRange& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSpellRangeLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wsrg\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSpellRange& c,
                     const std::string& base) {
    std::printf("Wrote %s.wsrg\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  ranges  : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterRanges";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsrgExt(base);
    auto c = wowee::pipeline::WoweeSpellRangeLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-srg")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRanged(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RangedSpellBuckets";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsrgExt(base);
    auto c = wowee::pipeline::WoweeSpellRangeLoader::makeRanged(name);
    if (!saveOrError(c, base, "gen-srg-ranged")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenFriendly(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FriendlyOnlyRanges";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsrgExt(base);
    auto c = wowee::pipeline::WoweeSpellRangeLoader::makeFriendly(name);
    if (!saveOrError(c, base, "gen-srg-friendly")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsrgExt(base);
    if (!wowee::pipeline::WoweeSpellRangeLoader::exists(base)) {
        std::fprintf(stderr, "WSRG not found: %s.wsrg\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellRangeLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wsrg"] = base + ".wsrg";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"rangeId", e.rangeId},
                {"name", e.name},
                {"description", e.description},
                {"rangeKind", e.rangeKind},
                {"rangeKindName", wowee::pipeline::WoweeSpellRange::rangeKindName(e.rangeKind)},
                {"minRange", e.minRange},
                {"maxRange", e.maxRange},
                {"minRangeFriendly", e.minRangeFriendly},
                {"maxRangeFriendly", e.maxRangeFriendly},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSRG: %s.wsrg\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  ranges  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind        min-max(hostile)  min-max(friendly)  color       name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-9s   %5.1f - %6.1f   %5.1f - %6.1f   0x%08x  %s\n",
                    e.rangeId,
                    wowee::pipeline::WoweeSpellRange::rangeKindName(e.rangeKind),
                    e.minRange, e.maxRange,
                    e.minRangeFriendly, e.maxRangeFriendly,
                    e.iconColorRGBA, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsrgExt(base);
    if (!wowee::pipeline::WoweeSpellRangeLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wsrg: WSRG not found: %s.wsrg\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellRangeLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.rangeId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.rangeId == 0)
            errors.push_back(ctx + ": rangeId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.rangeKind > wowee::pipeline::WoweeSpellRange::Unlimited) {
            errors.push_back(ctx + ": rangeKind " +
                std::to_string(e.rangeKind) + " not in 0..6");
        }
        if (e.minRange < 0.0f || e.maxRange < 0.0f ||
            e.minRangeFriendly < 0.0f ||
            e.maxRangeFriendly < 0.0f) {
            errors.push_back(ctx +
                ": negative range value (ranges must be >= 0)");
        }
        if (e.minRange > e.maxRange) {
            errors.push_back(ctx + ": minRange " +
                std::to_string(e.minRange) +
                " > maxRange " + std::to_string(e.maxRange));
        }
        if (e.minRangeFriendly > e.maxRangeFriendly) {
            errors.push_back(ctx + ": minRangeFriendly " +
                std::to_string(e.minRangeFriendly) +
                " > maxRangeFriendly " +
                std::to_string(e.maxRangeFriendly));
        }
        // Self-kind should have max range = 0; otherwise the
        // engine would treat it as targeted.
        if (e.rangeKind == wowee::pipeline::WoweeSpellRange::Self &&
            (e.maxRange != 0.0f || e.maxRangeFriendly != 0.0f)) {
            warnings.push_back(ctx +
                ": Self kind with non-zero maxRange — engine "
                "treats this as targeted, not self-only");
        }
        // Melee-kind should be 0..5y by canonical convention.
        if (e.rangeKind == wowee::pipeline::WoweeSpellRange::Melee &&
            e.maxRange > 8.0f) {
            warnings.push_back(ctx +
                ": Melee kind with maxRange " +
                std::to_string(e.maxRange) +
                " > 8 (canonical melee is 5y)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.rangeId) {
                errors.push_back(ctx + ": duplicate rangeId");
                break;
            }
        }
        idsSeen.push_back(e.rangeId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wsrg"] = base + ".wsrg";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wsrg: %s.wsrg\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu ranges, all rangeIds unique, all min<=max\n",
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

bool handleSpellRangesCatalog(int& i, int argc, char** argv,
                              int& outRc) {
    if (std::strcmp(argv[i], "--gen-srg") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-srg-ranged") == 0 && i + 1 < argc) {
        outRc = handleGenRanged(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-srg-friendly") == 0 && i + 1 < argc) {
        outRc = handleGenFriendly(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wsrg") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wsrg") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
