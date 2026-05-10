#include "cli_learning_notifications_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_learning_notifications.hpp"
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

std::string stripWldnExt(std::string base) {
    stripExt(base, ".wldn");
    return base;
}

const char* triggerKindName(uint8_t k) {
    using L = wowee::pipeline::WoweeLearningNotifications;
    switch (k) {
        case L::LevelReach:      return "levelreach";
        case L::FactionStanding: return "factionstanding";
        case L::ItemAcquired:    return "itemacquired";
        case L::QuestComplete:   return "questcomplete";
        case L::SpellLearned:    return "spelllearned";
        case L::ZoneEntered:     return "zoneentered";
        default:                 return "unknown";
    }
}

const char* channelKindName(uint8_t k) {
    using L = wowee::pipeline::WoweeLearningNotifications;
    switch (k) {
        case L::RaidWarning: return "raidwarning";
        case L::SystemMsg:   return "systemmsg";
        case L::Subtitle:    return "subtitle";
        case L::Tutorial:    return "tutorial";
        case L::MOTDAppend:  return "motdappend";
        default:             return "unknown";
    }
}

const char* factionFilterName(uint8_t f) {
    using L = wowee::pipeline::WoweeLearningNotifications;
    switch (f) {
        case L::AllianceOnly: return "alliance";
        case L::HordeOnly:    return "horde";
        case L::Both:         return "both";
        default:              return "unknown";
    }
}

bool saveOrError(const wowee::pipeline::WoweeLearningNotifications& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeLearningNotificationsLoader::save(
            c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wldn\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeLearningNotifications& c,
                     const std::string& base) {
    std::printf("Wrote %s.wldn\n", base.c_str());
    std::printf("  catalog       : %s\n", c.name.c_str());
    std::printf("  notifications : %zu\n", c.entries.size());
}

int handleGenLevels(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "LevelMilestones";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWldnExt(base);
    auto c = wowee::pipeline::WoweeLearningNotificationsLoader::
        makeLevelMilestones(name);
    if (!saveOrError(c, base, "gen-ldn")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenAccount(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AccountUnlocks";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWldnExt(base);
    auto c = wowee::pipeline::WoweeLearningNotificationsLoader::
        makeAccountUnlocks(name);
    if (!saveOrError(c, base, "gen-ldn-account")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenReputation(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ReputationMilestones";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWldnExt(base);
    auto c = wowee::pipeline::WoweeLearningNotificationsLoader::
        makeReputation(name);
    if (!saveOrError(c, base, "gen-ldn-rep")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWldnExt(base);
    if (!wowee::pipeline::WoweeLearningNotificationsLoader::exists(
            base)) {
        std::fprintf(stderr, "WLDN not found: %s.wldn\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeLearningNotificationsLoader::load(
        base);
    if (jsonOut) {
        nlohmann::json j;
        j["wldn"] = base + ".wldn";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"notificationId", e.notificationId},
                {"name", e.name},
                {"description", e.description},
                {"messageText", e.messageText},
                {"triggerKind", e.triggerKind},
                {"triggerKindName", triggerKindName(e.triggerKind)},
                {"channelKind", e.channelKind},
                {"channelKindName", channelKindName(e.channelKind)},
                {"factionFilter", e.factionFilter},
                {"factionFilterName",
                    factionFilterName(e.factionFilter)},
                {"triggerValue", e.triggerValue},
                {"soundId", e.soundId},
                {"minTotalTimePlayed", e.minTotalTimePlayed},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WLDN: %s.wldn\n", base.c_str());
    std::printf("  catalog       : %s\n", c.name.c_str());
    std::printf("  notifications : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   trigger          val    channel       faction   minPlayed   name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-15s  %5d   %-12s  %-9s  %8u   %s\n",
                    e.notificationId,
                    triggerKindName(e.triggerKind),
                    e.triggerValue,
                    channelKindName(e.channelKind),
                    factionFilterName(e.factionFilter),
                    e.minTotalTimePlayed,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWldnExt(base);
    if (!wowee::pipeline::WoweeLearningNotificationsLoader::exists(
            base)) {
        std::fprintf(stderr,
            "validate-wldn: WLDN not found: %s.wldn\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeLearningNotificationsLoader::load(
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
                          " (id=" + std::to_string(e.notificationId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.notificationId == 0)
            errors.push_back(ctx + ": notificationId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.messageText.empty())
            errors.push_back(ctx +
                ": messageText is empty — notification "
                "would deliver no payload");
        if (e.triggerKind > 5) {
            errors.push_back(ctx + ": triggerKind " +
                std::to_string(e.triggerKind) +
                " out of range (must be 0..5)");
        }
        if (e.channelKind > 4) {
            errors.push_back(ctx + ": channelKind " +
                std::to_string(e.channelKind) +
                " out of range (must be 0..4)");
        }
        if (e.factionFilter == 0 || e.factionFilter > 3) {
            errors.push_back(ctx + ": factionFilter " +
                std::to_string(e.factionFilter) +
                " out of range (must be 1=A / 2=H / 3=Both)");
        }
        // Per-trigger-kind validity of triggerValue.
        using L = wowee::pipeline::WoweeLearningNotifications;
        if (e.triggerKind == L::LevelReach) {
            if (e.triggerValue < 1 || e.triggerValue > 80) {
                warnings.push_back(ctx +
                    ": LevelReach triggerValue " +
                    std::to_string(e.triggerValue) +
                    " outside 1-80 range — current cap is "
                    "level 80 (WotLK)");
            }
        } else if (e.triggerKind == L::FactionStanding) {
            // Standing range: Hated=-42000, Exalted=42000.
            if (e.triggerValue < -42000 ||
                e.triggerValue > 42000) {
                errors.push_back(ctx +
                    ": FactionStanding triggerValue " +
                    std::to_string(e.triggerValue) +
                    " outside [-42000, 42000] valid range");
            }
        } else if (e.triggerKind == L::ItemAcquired ||
                   e.triggerKind == L::QuestComplete ||
                   e.triggerKind == L::SpellLearned ||
                   e.triggerKind == L::ZoneEntered) {
            if (e.triggerValue <= 0) {
                errors.push_back(ctx +
                    ": " + std::string(triggerKindName(
                        e.triggerKind)) +
                    " triggerValue " +
                    std::to_string(e.triggerValue) +
                    " <= 0 — must be a positive id");
            }
        }
        if (e.messageText.size() > 255) {
            warnings.push_back(ctx + ": messageText is " +
                std::to_string(e.messageText.size()) +
                " chars (>255) — server may truncate on "
                "delivery");
        }
        if (!idsSeen.insert(e.notificationId).second) {
            errors.push_back(ctx + ": duplicate notificationId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wldn"] = base + ".wldn";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wldn: %s.wldn\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu notifications, all "
                    "notificationIds unique, triggerValues "
                    "valid for kind\n",
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

bool handleLearningNotificationsCatalog(int& i, int argc, char** argv,
                                          int& outRc) {
    if (std::strcmp(argv[i], "--gen-ldn") == 0 && i + 1 < argc) {
        outRc = handleGenLevels(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-ldn-account") == 0 &&
        i + 1 < argc) {
        outRc = handleGenAccount(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-ldn-rep") == 0 && i + 1 < argc) {
        outRc = handleGenReputation(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wldn") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wldn") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
