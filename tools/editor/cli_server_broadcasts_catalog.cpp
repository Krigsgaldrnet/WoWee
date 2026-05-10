#include "cli_server_broadcasts_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_server_broadcasts.hpp"
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

std::string stripWscbExt(std::string base) {
    stripExt(base, ".wscb");
    return base;
}

const char* channelKindName(uint8_t k) {
    using S = wowee::pipeline::WoweeServerBroadcasts;
    switch (k) {
        case S::Login:         return "login";
        case S::SystemChannel: return "system";
        case S::RaidWarning:   return "raidwarning";
        case S::MOTD:          return "motd";
        case S::HelpTip:       return "helptip";
        default:               return "unknown";
    }
}

const char* factionFilterName(uint8_t f) {
    using S = wowee::pipeline::WoweeServerBroadcasts;
    switch (f) {
        case S::AllianceOnly: return "alliance";
        case S::HordeOnly:    return "horde";
        case S::Both:         return "both";
        default:              return "unknown";
    }
}

bool saveOrError(const wowee::pipeline::WoweeServerBroadcasts& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeServerBroadcastsLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wscb\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeServerBroadcasts& c,
                     const std::string& base) {
    std::printf("Wrote %s.wscb\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  broadcasts : %zu\n", c.entries.size());
}

int handleGenMotd(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ServerMOTD";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWscbExt(base);
    auto c = wowee::pipeline::WoweeServerBroadcastsLoader::makeMotd(name);
    if (!saveOrError(c, base, "gen-scb")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMaintenance(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MaintenanceWarnings";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWscbExt(base);
    auto c = wowee::pipeline::WoweeServerBroadcastsLoader::makeMaintenance(name);
    if (!saveOrError(c, base, "gen-scb-maintenance")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenHelpTips(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "HelpChannelTips";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWscbExt(base);
    auto c = wowee::pipeline::WoweeServerBroadcastsLoader::makeHelpTips(name);
    if (!saveOrError(c, base, "gen-scb-helptips")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWscbExt(base);
    if (!wowee::pipeline::WoweeServerBroadcastsLoader::exists(base)) {
        std::fprintf(stderr, "WSCB not found: %s.wscb\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeServerBroadcastsLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wscb"] = base + ".wscb";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"broadcastId", e.broadcastId},
                {"name", e.name},
                {"description", e.description},
                {"messageText", e.messageText},
                {"intervalSeconds", e.intervalSeconds},
                {"channelKind", e.channelKind},
                {"channelKindName", channelKindName(e.channelKind)},
                {"factionFilter", e.factionFilter},
                {"factionFilterName",
                    factionFilterName(e.factionFilter)},
                {"minLevel", e.minLevel},
                {"maxLevel", e.maxLevel},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSCB: %s.wscb\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  broadcasts : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   channel       faction   interval(s)  lvls    name\n");
    for (const auto& e : c.entries) {
        char lvls[16];
        std::snprintf(lvls, sizeof(lvls), "%u-%u",
                      e.minLevel, e.maxLevel);
        std::printf("  %4u  %-12s  %-9s  %10u  %-6s  %s\n",
                    e.broadcastId,
                    channelKindName(e.channelKind),
                    factionFilterName(e.factionFilter),
                    e.intervalSeconds, lvls, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWscbExt(base);
    if (!wowee::pipeline::WoweeServerBroadcastsLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wscb: WSCB not found: %s.wscb\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeServerBroadcastsLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.broadcastId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.broadcastId == 0)
            errors.push_back(ctx + ": broadcastId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.messageText.empty())
            errors.push_back(ctx + ": messageText is empty "
                "— broadcast would deliver no payload");
        if (e.factionFilter == 0 || e.factionFilter > 3) {
            errors.push_back(ctx + ": factionFilter " +
                std::to_string(e.factionFilter) +
                " out of range (must be 1=A / 2=H / 3=Both)");
        }
        if (e.channelKind > 4) {
            errors.push_back(ctx + ": channelKind " +
                std::to_string(e.channelKind) +
                " out of range (must be 0..4)");
        }
        if (e.minLevel > 0 && e.maxLevel > 0 &&
            e.minLevel > e.maxLevel) {
            errors.push_back(ctx + ": minLevel " +
                std::to_string(e.minLevel) +
                " > maxLevel " + std::to_string(e.maxLevel));
        }
        // Periodic broadcasts (interval>0) make sense
        // mainly on SystemChannel and HelpTip. Login/MOTD
        // with interval>0 is a configuration mistake —
        // those fire on session enter, not on a timer.
        using S = wowee::pipeline::WoweeServerBroadcasts;
        if (e.intervalSeconds > 0 &&
            (e.channelKind == S::Login ||
             e.channelKind == S::MOTD)) {
            warnings.push_back(ctx +
                ": intervalSeconds=" +
                std::to_string(e.intervalSeconds) +
                " on " + channelKindName(e.channelKind) +
                " channel — login/MOTD fire on session "
                "enter, not on a timer; interval likely "
                "ignored");
        }
        // Very short intervals would spam players. Warn
        // below 60 seconds; reject below 10 seconds.
        if (e.intervalSeconds > 0 && e.intervalSeconds < 10) {
            errors.push_back(ctx + ": intervalSeconds " +
                std::to_string(e.intervalSeconds) +
                " < 10 — would spam players faster than "
                "they can read");
        } else if (e.intervalSeconds > 0 &&
                   e.intervalSeconds < 60) {
            warnings.push_back(ctx + ": intervalSeconds " +
                std::to_string(e.intervalSeconds) +
                " < 60 — broadcast fires more than once "
                "per minute; verify if intentional");
        }
        // Message length sanity: WoW chat message buffer
        // is ~255 chars; over that, server may truncate.
        if (e.messageText.size() > 255) {
            warnings.push_back(ctx + ": messageText is " +
                std::to_string(e.messageText.size()) +
                " chars (>255) — server may truncate on "
                "delivery");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.broadcastId) {
                errors.push_back(ctx + ": duplicate broadcastId");
                break;
            }
        }
        idsSeen.push_back(e.broadcastId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wscb"] = base + ".wscb";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wscb: %s.wscb\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu broadcasts, all ids unique\n",
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

bool handleServerBroadcastsCatalog(int& i, int argc, char** argv,
                                   int& outRc) {
    if (std::strcmp(argv[i], "--gen-scb") == 0 && i + 1 < argc) {
        outRc = handleGenMotd(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-scb-maintenance") == 0 &&
        i + 1 < argc) {
        outRc = handleGenMaintenance(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-scb-helptips") == 0 &&
        i + 1 < argc) {
        outRc = handleGenHelpTips(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wscb") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wscb") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
