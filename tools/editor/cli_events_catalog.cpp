#include "cli_events_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_events.hpp"
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

std::string stripWseaExt(std::string base) {
    stripExt(base, ".wsea");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeEvent& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeEventLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wsea\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeEvent& c,
                     const std::string& base) {
    std::printf("Wrote %s.wsea\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  events  : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterEvents";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWseaExt(base);
    auto c = wowee::pipeline::WoweeEventLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-events")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenYearly(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "YearlyEvents";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWseaExt(base);
    auto c = wowee::pipeline::WoweeEventLoader::makeYearly(name);
    if (!saveOrError(c, base, "gen-events-yearly")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBonusWeekends(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BonusWeekends";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWseaExt(base);
    auto c = wowee::pipeline::WoweeEventLoader::makeBonusWeekends(name);
    if (!saveOrError(c, base, "gen-events-weekends")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWseaExt(base);
    if (!wowee::pipeline::WoweeEventLoader::exists(base)) {
        std::fprintf(stderr, "WSEA not found: %s.wsea\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeEventLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wsea"] = base + ".wsea";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"eventId", e.eventId},
                {"name", e.name},
                {"description", e.description},
                {"iconPath", e.iconPath},
                {"announceMessage", e.announceMessage},
                {"startDate", e.startDate},
                {"duration_seconds", e.duration_seconds},
                {"recurrenceDays", e.recurrenceDays},
                {"holidayKind", e.holidayKind},
                {"holidayKindName", wowee::pipeline::WoweeEvent::holidayKindName(e.holidayKind)},
                {"factionGroup", e.factionGroup},
                {"factionGroupName", wowee::pipeline::WoweeEvent::factionGroupName(e.factionGroup)},
                {"bonusXpPercent", e.bonusXpPercent},
                {"tokenIdReward", e.tokenIdReward},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSEA: %s.wsea\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  events  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   kind         duration  recur   bonus%%  token   name\n");
    for (const auto& e : c.entries) {
        uint32_t durDays = e.duration_seconds / (24 * 3600);
        uint32_t durHours = (e.duration_seconds % (24 * 3600)) / 3600;
        std::printf("  %4u   %-11s  %2ud %2uh    %4u    %3u     %4u    %s\n",
                    e.eventId,
                    wowee::pipeline::WoweeEvent::holidayKindName(e.holidayKind),
                    durDays, durHours,
                    e.recurrenceDays, e.bonusXpPercent,
                    e.tokenIdReward, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWseaExt(base);
    if (!wowee::pipeline::WoweeEventLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wsea: WSEA not found: %s.wsea\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeEventLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.eventId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.eventId == 0) errors.push_back(ctx + ": eventId is 0");
        if (e.name.empty()) errors.push_back(ctx + ": name is empty");
        if (e.holidayKind > wowee::pipeline::WoweeEvent::WorldEvent) {
            errors.push_back(ctx + ": holidayKind " +
                std::to_string(e.holidayKind) + " not in 0..6");
        }
        if (e.factionGroup > wowee::pipeline::WoweeEvent::FactionHorde) {
            errors.push_back(ctx + ": factionGroup " +
                std::to_string(e.factionGroup) + " not in 0..2");
        }
        if (e.duration_seconds == 0) {
            errors.push_back(ctx + ": duration_seconds is 0 (event never runs)");
        }
        if (e.recurrenceDays > 0 &&
            e.duration_seconds > e.recurrenceDays * 24u * 3600u) {
            errors.push_back(ctx +
                ": duration exceeds recurrence period (events would overlap)");
        }
        if (e.bonusXpPercent > 200) {
            warnings.push_back(ctx +
                ": bonusXpPercent > 200 (very high — verify intentional)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.eventId) {
                errors.push_back(ctx + ": duplicate eventId");
                break;
            }
        }
        idsSeen.push_back(e.eventId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wsea"] = base + ".wsea";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wsea: %s.wsea\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu events, all eventIds unique\n",
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

bool handleEventsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-events") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-events-yearly") == 0 && i + 1 < argc) {
        outRc = handleGenYearly(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-events-weekends") == 0 && i + 1 < argc) {
        outRc = handleGenBonusWeekends(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wsea") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wsea") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
