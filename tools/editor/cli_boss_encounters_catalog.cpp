#include "cli_boss_encounters_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_boss_encounters.hpp"
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

std::string stripWbosExt(std::string base) {
    stripExt(base, ".wbos");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeBossEncounter& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeBossEncounterLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wbos\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeBossEncounter& c,
                     const std::string& base) {
    std::printf("Wrote %s.wbos\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  encounters : %zu\n", c.entries.size());
}

int handleGenFiveMan(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FiveManBosses";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbosExt(base);
    auto c = wowee::pipeline::WoweeBossEncounterLoader::makeFiveMan(name);
    if (!saveOrError(c, base, "gen-bos")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRaid10(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ICC10NormalBosses";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbosExt(base);
    auto c = wowee::pipeline::WoweeBossEncounterLoader::makeRaid10(name);
    if (!saveOrError(c, base, "gen-bos-raid10")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenWorldBoss(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WorldBosses";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbosExt(base);
    auto c = wowee::pipeline::WoweeBossEncounterLoader::makeWorldBoss(name);
    if (!saveOrError(c, base, "gen-bos-world")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWbosExt(base);
    if (!wowee::pipeline::WoweeBossEncounterLoader::exists(base)) {
        std::fprintf(stderr, "WBOS not found: %s.wbos\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeBossEncounterLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wbos"] = base + ".wbos";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"encounterId", e.encounterId},
                {"name", e.name},
                {"description", e.description},
                {"bossCreatureId", e.bossCreatureId},
                {"mapId", e.mapId},
                {"difficultyId", e.difficultyId},
                {"berserkSpellId", e.berserkSpellId},
                {"enrageTimerMs", e.enrageTimerMs},
                {"phaseCount", e.phaseCount},
                {"requiredPartySize", e.requiredPartySize},
                {"recommendedItemLevel", e.recommendedItemLevel},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WBOS: %s.wbos\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  encounters : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    boss     map   diff  phases  size  ilvl   enrage(min)  berserk  name\n");
    for (const auto& e : c.entries) {
        char enrageBuf[16];
        if (e.enrageTimerMs == 0) {
            std::snprintf(enrageBuf, sizeof(enrageBuf), "-");
        } else {
            std::snprintf(enrageBuf, sizeof(enrageBuf), "%.1f",
                          e.enrageTimerMs / 60000.0);
        }
        std::printf("  %4u   %5u   %5u  %4u   %3u    %3u   %4u   %-9s    %5u    %s\n",
                    e.encounterId, e.bossCreatureId,
                    e.mapId, e.difficultyId,
                    e.phaseCount, e.requiredPartySize,
                    e.recommendedItemLevel, enrageBuf,
                    e.berserkSpellId, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWbosExt(base);
    if (!wowee::pipeline::WoweeBossEncounterLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wbos: WBOS not found: %s.wbos\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeBossEncounterLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.encounterId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.encounterId == 0)
            errors.push_back(ctx + ": encounterId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.bossCreatureId == 0)
            errors.push_back(ctx +
                ": bossCreatureId is 0 — encounter has no boss");
        if (e.mapId == 0)
            errors.push_back(ctx +
                ": mapId is 0 — encounter is unbound to a map");
        if (e.phaseCount == 0)
            errors.push_back(ctx +
                ": phaseCount is 0 — encounter has no phases");
        if (e.requiredPartySize == 0)
            errors.push_back(ctx +
                ": requiredPartySize is 0 — invalid group size");
        if (e.requiredPartySize > 40)
            warnings.push_back(ctx +
                ": requiredPartySize " +
                std::to_string(e.requiredPartySize) +
                " > 40 (max raid size)");
        // Standard sizes are 5/10/25/40 — anything else is a
        // server-custom raid size, worth flagging.
        if (e.requiredPartySize != 5 && e.requiredPartySize != 10 &&
            e.requiredPartySize != 25 && e.requiredPartySize != 40) {
            warnings.push_back(ctx +
                ": non-standard requiredPartySize " +
                std::to_string(e.requiredPartySize) +
                " (canonical sizes are 5/10/25/40)");
        }
        // berserkSpellId without enrageTimerMs is contradictory
        // (the spell never fires).
        if (e.berserkSpellId != 0 && e.enrageTimerMs == 0) {
            warnings.push_back(ctx +
                ": berserkSpellId=" +
                std::to_string(e.berserkSpellId) +
                " set but enrageTimerMs=0 — spell will never "
                "fire (no enrage countdown)");
        }
        // Enrage > 30 minutes is suspicious — typical raid
        // encounters cap at ~15-20 minutes.
        if (e.enrageTimerMs > 1800000) {
            warnings.push_back(ctx +
                ": enrageTimerMs " +
                std::to_string(e.enrageTimerMs) +
                " > 30 min (1800000ms) — exceptionally long "
                "soft enrage, double-check");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.encounterId) {
                errors.push_back(ctx + ": duplicate encounterId");
                break;
            }
        }
        idsSeen.push_back(e.encounterId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wbos"] = base + ".wbos";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wbos: %s.wbos\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu encounters, all encounterIds unique\n",
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

bool handleBossEncountersCatalog(int& i, int argc, char** argv,
                                 int& outRc) {
    if (std::strcmp(argv[i], "--gen-bos") == 0 && i + 1 < argc) {
        outRc = handleGenFiveMan(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-bos-raid10") == 0 && i + 1 < argc) {
        outRc = handleGenRaid10(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-bos-world") == 0 && i + 1 < argc) {
        outRc = handleGenWorldBoss(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wbos") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wbos") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
