#include "cli_spell_durations_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_spell_durations.hpp"
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

std::string stripWsdrExt(std::string base) {
    stripExt(base, ".wsdr");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeSpellDuration& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSpellDurationLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wsdr\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSpellDuration& c,
                     const std::string& base) {
    std::printf("Wrote %s.wsdr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  buckets : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterDurations";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsdrExt(base);
    auto c = wowee::pipeline::WoweeSpellDurationLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-sdr")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBuffs(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "LongDurationBuffs";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsdrExt(base);
    auto c = wowee::pipeline::WoweeSpellDurationLoader::makeBuffs(name);
    if (!saveOrError(c, base, "gen-sdr-buffs")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenDot(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "DoTHoTDurations";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsdrExt(base);
    auto c = wowee::pipeline::WoweeSpellDurationLoader::makeDot(name);
    if (!saveOrError(c, base, "gen-sdr-dot")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsdrExt(base);
    if (!wowee::pipeline::WoweeSpellDurationLoader::exists(base)) {
        std::fprintf(stderr, "WSDR not found: %s.wsdr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellDurationLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wsdr"] = base + ".wsdr";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"durationId", e.durationId},
                {"name", e.name},
                {"description", e.description},
                {"durationKind", e.durationKind},
                {"durationKindName", wowee::pipeline::WoweeSpellDuration::durationKindName(e.durationKind)},
                {"baseDurationMs", e.baseDurationMs},
                {"perLevelMs", e.perLevelMs},
                {"maxDurationMs", e.maxDurationMs},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSDR: %s.wsdr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  buckets : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind             baseMs   perLvl     maxMs  color       name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-15s %8d %8d %9d  0x%08x  %s\n",
                    e.durationId,
                    wowee::pipeline::WoweeSpellDuration::durationKindName(e.durationKind),
                    e.baseDurationMs, e.perLevelMs,
                    e.maxDurationMs,
                    e.iconColorRGBA, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsdrExt(base);
    if (!wowee::pipeline::WoweeSpellDurationLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wsdr: WSDR not found: %s.wsdr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellDurationLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.durationId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.durationId == 0)
            errors.push_back(ctx + ": durationId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.durationKind > wowee::pipeline::WoweeSpellDuration::UntilDeath) {
            errors.push_back(ctx + ": durationKind " +
                std::to_string(e.durationKind) + " not in 0..4");
        }
        if (e.maxDurationMs < 0)
            errors.push_back(ctx + ": maxDurationMs < 0");
        if (e.perLevelMs < 0)
            warnings.push_back(ctx +
                ": perLevelMs < 0 — duration shrinks with "
                "level, double-check this is intentional");
        // Instant kind should have base == 0.
        if (e.durationKind == wowee::pipeline::WoweeSpellDuration::Instant &&
            e.baseDurationMs != 0) {
            warnings.push_back(ctx +
                ": Instant kind with baseDurationMs=" +
                std::to_string(e.baseDurationMs) +
                " — engine will track it as a timed aura");
        }
        // UntilCancelled / UntilDeath should signal "no
        // timer" via baseDurationMs<0; otherwise the engine
        // would tick down to expiry.
        if ((e.durationKind == wowee::pipeline::WoweeSpellDuration::UntilCancelled ||
             e.durationKind == wowee::pipeline::WoweeSpellDuration::UntilDeath) &&
            e.baseDurationMs >= 0) {
            warnings.push_back(ctx +
                ": permanent kind with non-negative "
                "baseDurationMs — engine treats this as timed; "
                "set baseDurationMs=-1 to flag as no-timer");
        }
        // Timed/TickBased should have base > 0.
        if ((e.durationKind == wowee::pipeline::WoweeSpellDuration::Timed ||
             e.durationKind == wowee::pipeline::WoweeSpellDuration::TickBased) &&
            e.baseDurationMs <= 0) {
            errors.push_back(ctx +
                ": Timed/TickBased kind requires "
                "baseDurationMs > 0");
        }
        // maxDurationMs<base is contradictory.
        if (e.maxDurationMs > 0 && e.baseDurationMs > e.maxDurationMs) {
            errors.push_back(ctx + ": baseDurationMs " +
                std::to_string(e.baseDurationMs) +
                " > maxDurationMs " +
                std::to_string(e.maxDurationMs));
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.durationId) {
                errors.push_back(ctx + ": duplicate durationId");
                break;
            }
        }
        idsSeen.push_back(e.durationId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wsdr"] = base + ".wsdr";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wsdr: %s.wsdr\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu buckets, all durationIds unique\n",
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

bool handleSpellDurationsCatalog(int& i, int argc, char** argv,
                                 int& outRc) {
    if (std::strcmp(argv[i], "--gen-sdr") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-sdr-buffs") == 0 && i + 1 < argc) {
        outRc = handleGenBuffs(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-sdr-dot") == 0 && i + 1 < argc) {
        outRc = handleGenDot(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wsdr") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wsdr") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
