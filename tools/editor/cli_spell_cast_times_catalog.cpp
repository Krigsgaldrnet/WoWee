#include "cli_spell_cast_times_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_spell_cast_times.hpp"
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

std::string stripWsctExt(std::string base) {
    stripExt(base, ".wsct");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeSpellCastTime& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSpellCastTimeLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wsct\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSpellCastTime& c,
                     const std::string& base) {
    std::printf("Wrote %s.wsct\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  buckets : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterCastTimes";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsctExt(base);
    auto c = wowee::pipeline::WoweeSpellCastTimeLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-sct")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenChannel(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ChannelCastTimes";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsctExt(base);
    auto c = wowee::pipeline::WoweeSpellCastTimeLoader::makeChannel(name);
    if (!saveOrError(c, base, "gen-sct-channel")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRamp(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "LevelScaledCastTimes";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsctExt(base);
    auto c = wowee::pipeline::WoweeSpellCastTimeLoader::makeRamp(name);
    if (!saveOrError(c, base, "gen-sct-ramp")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsctExt(base);
    if (!wowee::pipeline::WoweeSpellCastTimeLoader::exists(base)) {
        std::fprintf(stderr, "WSCT not found: %s.wsct\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellCastTimeLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wsct"] = base + ".wsct";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"castTimeId", e.castTimeId},
                {"name", e.name},
                {"description", e.description},
                {"castKind", e.castKind},
                {"castKindName", wowee::pipeline::WoweeSpellCastTime::castKindName(e.castKind)},
                {"baseCastMs", e.baseCastMs},
                {"perLevelMs", e.perLevelMs},
                {"minCastMs", e.minCastMs},
                {"maxCastMs", e.maxCastMs},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSCT: %s.wsct\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  buckets : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind       baseMs perLvl  minMs   maxMs   color       name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-8s   %6d %6d %6d %7d  0x%08x  %s\n",
                    e.castTimeId,
                    wowee::pipeline::WoweeSpellCastTime::castKindName(e.castKind),
                    e.baseCastMs, e.perLevelMs,
                    e.minCastMs, e.maxCastMs,
                    e.iconColorRGBA, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsctExt(base);
    if (!wowee::pipeline::WoweeSpellCastTimeLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wsct: WSCT not found: %s.wsct\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellCastTimeLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.castTimeId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.castTimeId == 0)
            errors.push_back(ctx + ": castTimeId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.castKind > wowee::pipeline::WoweeSpellCastTime::ChargeCast) {
            errors.push_back(ctx + ": castKind " +
                std::to_string(e.castKind) + " not in 0..4");
        }
        if (e.baseCastMs < 0)
            errors.push_back(ctx + ": baseCastMs < 0");
        if (e.perLevelMs < 0)
            warnings.push_back(ctx +
                ": perLevelMs < 0 — cast time shrinks with "
                "level, double-check this is intentional");
        if (e.maxCastMs > 0 && e.minCastMs > e.maxCastMs) {
            errors.push_back(ctx + ": minCastMs " +
                std::to_string(e.minCastMs) +
                " > maxCastMs " + std::to_string(e.maxCastMs));
        }
        // Instant kind should have base == 0 — otherwise the
        // engine would still display a cast bar.
        if (e.castKind == wowee::pipeline::WoweeSpellCastTime::Instant &&
            e.baseCastMs != 0) {
            warnings.push_back(ctx +
                ": Instant kind with baseCastMs=" +
                std::to_string(e.baseCastMs) +
                " — engine will draw a cast bar (use Cast "
                "kind if that's intended)");
        }
        // Channel kind should have base > 0 — otherwise it
        // would tick once and immediately end.
        if (e.castKind == wowee::pipeline::WoweeSpellCastTime::Channel &&
            e.baseCastMs <= 0) {
            errors.push_back(ctx +
                ": Channel kind requires baseCastMs > 0 "
                "(channel duration)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.castTimeId) {
                errors.push_back(ctx + ": duplicate castTimeId");
                break;
            }
        }
        idsSeen.push_back(e.castTimeId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wsct"] = base + ".wsct";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wsct: %s.wsct\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu buckets, all castTimeIds unique, all min<=max\n",
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

bool handleSpellCastTimesCatalog(int& i, int argc, char** argv,
                                 int& outRc) {
    if (std::strcmp(argv[i], "--gen-sct") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-sct-channel") == 0 && i + 1 < argc) {
        outRc = handleGenChannel(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-sct-ramp") == 0 && i + 1 < argc) {
        outRc = handleGenRamp(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wsct") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wsct") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
