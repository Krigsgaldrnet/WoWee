#include "cli_anniversary_events_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_anniversary_events.hpp"
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

std::string stripWanvExt(std::string base) {
    stripExt(base, ".wanv");
    return base;
}

const char* eventKindName(uint8_t k) {
    using A = wowee::pipeline::WoweeAnniversaryEvents;
    switch (k) {
        case A::Holiday:           return "holiday";
        case A::Anniversary:       return "anniversary";
        case A::DoubleXP:          return "doublexp";
        case A::DoubleHonor:       return "doublehonor";
        case A::PetBattleWeekend:  return "petbattle";
        case A::BattlegroundBonus: return "bgbonus";
        case A::SeasonalQuest:     return "seasonalquest";
        case A::Misc:              return "misc";
        default:                   return "unknown";
    }
}

const char* recurrenceKindName(uint8_t k) {
    using A = wowee::pipeline::WoweeAnniversaryEvents;
    switch (k) {
        case A::Yearly:  return "yearly";
        case A::Monthly: return "monthly";
        case A::Weekly:  return "weekly";
        case A::OneOff:  return "oneoff";
        default:         return "unknown";
    }
}

const char* weekdayName(uint8_t d) {
    static const char* kDays[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    return d <= 6 ? kDays[d] : "?";
}

bool saveOrError(const wowee::pipeline::WoweeAnniversaryEvents& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeAnniversaryEventsLoader::save(
            c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wanv\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeAnniversaryEvents& c,
                     const std::string& base) {
    std::printf("Wrote %s.wanv\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  events  : %zu\n", c.entries.size());
}

int handleGenHolidays(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StandardHolidays";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWanvExt(base);
    auto c = wowee::pipeline::WoweeAnniversaryEventsLoader::
        makeStandardHolidays(name);
    if (!saveOrError(c, base, "gen-anv")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBonus(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WeeklyBonusEvents";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWanvExt(base);
    auto c = wowee::pipeline::WoweeAnniversaryEventsLoader::
        makeBonusEvents(name);
    if (!saveOrError(c, base, "gen-anv-bonus")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenAnniversary(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "GameLaunchAnniversaries";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWanvExt(base);
    auto c = wowee::pipeline::WoweeAnniversaryEventsLoader::
        makeAnniversary(name);
    if (!saveOrError(c, base, "gen-anv-launch")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWanvExt(base);
    if (!wowee::pipeline::WoweeAnniversaryEventsLoader::exists(
            base)) {
        std::fprintf(stderr, "WANV not found: %s.wanv\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeAnniversaryEventsLoader::load(
        base);
    if (jsonOut) {
        nlohmann::json j;
        j["wanv"] = base + ".wanv";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"eventId", e.eventId},
                {"name", e.name},
                {"description", e.description},
                {"eventKind", e.eventKind},
                {"eventKindName", eventKindName(e.eventKind)},
                {"recurrenceKind", e.recurrenceKind},
                {"recurrenceKindName",
                    recurrenceKindName(e.recurrenceKind)},
                {"startMonth", e.startMonth},
                {"startDay", e.startDay},
                {"durationDays", e.durationDays},
                {"payloadSpellId", e.payloadSpellId},
                {"payloadItemId", e.payloadItemId},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WANV: %s.wanv\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  events  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   kind         recurrence   schedule          dur(d)  spell  item   name\n");
    for (const auto& e : c.entries) {
        char schedule[32];
        using A = wowee::pipeline::WoweeAnniversaryEvents;
        if (e.recurrenceKind == A::Weekly) {
            std::snprintf(schedule, sizeof(schedule),
                          "%-3s every week", weekdayName(e.startDay));
        } else if (e.recurrenceKind == A::Monthly) {
            std::snprintf(schedule, sizeof(schedule),
                          "Day %u every month", e.startDay);
        } else {
            std::snprintf(schedule, sizeof(schedule),
                          "%02u/%02u %s", e.startMonth, e.startDay,
                          recurrenceKindName(e.recurrenceKind));
        }
        std::printf("  %4u   %-10s   %-10s   %-15s   %4u   %5u  %5u  %s\n",
                    e.eventId, eventKindName(e.eventKind),
                    recurrenceKindName(e.recurrenceKind),
                    schedule, e.durationDays,
                    e.payloadSpellId, e.payloadItemId,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWanvExt(base);
    if (!wowee::pipeline::WoweeAnniversaryEventsLoader::exists(
            base)) {
        std::fprintf(stderr,
            "validate-wanv: WANV not found: %s.wanv\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeAnniversaryEventsLoader::load(
        base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.eventId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.eventId == 0)
            errors.push_back(ctx + ": eventId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.eventKind > 6 && e.eventKind != 255) {
            errors.push_back(ctx + ": eventKind " +
                std::to_string(e.eventKind) +
                " out of range (must be 0..6 or 255 Misc)");
        }
        if (e.recurrenceKind > 3) {
            errors.push_back(ctx + ": recurrenceKind " +
                std::to_string(e.recurrenceKind) +
                " out of range (must be 0..3)");
        }
        if (e.durationDays == 0) {
            errors.push_back(ctx +
                ": durationDays is 0 — event would never "
                "have an active window");
        }
        // Per-recurrence schedule validity: Yearly /
        // Monthly / OneOff need valid month + day; Weekly
        // ignores month + uses day as weekday 0..6.
        using A = wowee::pipeline::WoweeAnniversaryEvents;
        if (e.recurrenceKind == A::Weekly) {
            if (e.startDay > 6) {
                errors.push_back(ctx +
                    ": Weekly recurrence with startDay " +
                    std::to_string(e.startDay) +
                    " > 6 — must be 0 (Sun) through 6 (Sat)");
            }
            if (e.durationDays > 7) {
                warnings.push_back(ctx +
                    ": Weekly recurrence with "
                    "durationDays > 7 — event would "
                    "overlap with itself across week "
                    "boundaries");
            }
        } else {
            if (e.startMonth < 1 || e.startMonth > 12) {
                errors.push_back(ctx +
                    ": startMonth " +
                    std::to_string(e.startMonth) +
                    " out of range (must be 1..12 for "
                    "Yearly / Monthly / OneOff)");
            }
            if (e.startDay < 1 || e.startDay > 31) {
                errors.push_back(ctx + ": startDay " +
                    std::to_string(e.startDay) +
                    " out of range (must be 1..31)");
            }
            // Calendar sanity: Feb has 28-29 days, etc.
            // The validator doesn't try to be a full
            // calendar — just catches the obvious "Feb 30"
            // type errors.
            if (e.startMonth == 2 && e.startDay > 29) {
                errors.push_back(ctx +
                    ": startDay " + std::to_string(e.startDay) +
                    " for February — must be 1..29 (28 in "
                    "non-leap years; the schedule rolls "
                    "over to Mar 1 in those cases)");
            }
            if ((e.startMonth == 4 || e.startMonth == 6 ||
                 e.startMonth == 9 || e.startMonth == 11) &&
                e.startDay > 30) {
                errors.push_back(ctx +
                    ": startDay 31 for month " +
                    std::to_string(e.startMonth) +
                    " — that month only has 30 days");
            }
        }
        if (!idsSeen.insert(e.eventId).second) {
            errors.push_back(ctx + ": duplicate eventId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wanv"] = base + ".wanv";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wanv: %s.wanv\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu events, all eventIds "
                    "unique, calendar dates valid\n",
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

bool handleAnniversaryEventsCatalog(int& i, int argc, char** argv,
                                     int& outRc) {
    if (std::strcmp(argv[i], "--gen-anv") == 0 && i + 1 < argc) {
        outRc = handleGenHolidays(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-anv-bonus") == 0 &&
        i + 1 < argc) {
        outRc = handleGenBonus(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-anv-launch") == 0 &&
        i + 1 < argc) {
        outRc = handleGenAnniversary(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wanv") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wanv") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
