#include "cli_global_channels_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_global_channels.hpp"
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

std::string stripWgchExt(std::string base) {
    stripExt(base, ".wgch");
    return base;
}

const char* channelKindName(uint8_t k) {
    using G = wowee::pipeline::WoweeGlobalChannels;
    switch (k) {
        case G::Global:    return "global";
        case G::RealmZone: return "realmzone";
        case G::Faction:   return "faction";
        case G::Custom:    return "custom";
        default:           return "unknown";
    }
}

const char* accessKindName(uint8_t k) {
    using G = wowee::pipeline::WoweeGlobalChannels;
    switch (k) {
        case G::PublicJoin:     return "publicjoin";
        case G::InviteOnly:     return "inviteonly";
        case G::AutoJoinOnZone: return "autojoinonzone";
        case G::Moderated:      return "moderated";
        default:                return "unknown";
    }
}

bool saveOrError(const wowee::pipeline::WoweeGlobalChannels& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeGlobalChannelsLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wgch\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeGlobalChannels& c,
                     const std::string& base) {
    std::printf("Wrote %s.wgch\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  channels: %zu\n", c.entries.size());
}

int handleGenStandard(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StandardChatChannels";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgchExt(base);
    auto c = wowee::pipeline::WoweeGlobalChannelsLoader::
        makeStandardChannels(name);
    if (!saveOrError(c, base, "gen-gch")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRoleplay(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RoleplayChannels";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgchExt(base);
    auto c = wowee::pipeline::WoweeGlobalChannelsLoader::
        makeRoleplay(name);
    if (!saveOrError(c, base, "gen-gch-rp")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenAdmin(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AdminChannels";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgchExt(base);
    auto c = wowee::pipeline::WoweeGlobalChannelsLoader::
        makeAdminChannels(name);
    if (!saveOrError(c, base, "gen-gch-admin")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgchExt(base);
    if (!wowee::pipeline::WoweeGlobalChannelsLoader::exists(base)) {
        std::fprintf(stderr, "WGCH not found: %s.wgch\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGlobalChannelsLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wgch"] = base + ".wgch";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"channelId", e.channelId},
                {"name", e.name},
                {"description", e.description},
                {"channelKind", e.channelKind},
                {"channelKindName", channelKindName(e.channelKind)},
                {"accessKind", e.accessKind},
                {"accessKindName", accessKindName(e.accessKind)},
                {"passwordRequired", e.passwordRequired != 0},
                {"levelMin", e.levelMin},
                {"maxMembers", e.maxMembers},
                {"topicSetByMods", e.topicSetByMods != 0},
                {"zoneDefaultMapId", e.zoneDefaultMapId},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WGCH: %s.wgch\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  channels: %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   kind        access            pw  lvl  max     zone  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-10s  %-15s   %s   %3u  %5u  %5u   %s\n",
                    e.channelId, channelKindName(e.channelKind),
                    accessKindName(e.accessKind),
                    e.passwordRequired ? "Y" : "n",
                    e.levelMin, e.maxMembers,
                    e.zoneDefaultMapId, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgchExt(base);
    if (!wowee::pipeline::WoweeGlobalChannelsLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wgch: WGCH not found: %s.wgch\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGlobalChannelsLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    std::set<std::string> namesSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.channelId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.channelId == 0)
            errors.push_back(ctx + ": channelId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.channelKind > 3) {
            errors.push_back(ctx + ": channelKind " +
                std::to_string(e.channelKind) +
                " out of range (must be 0..3)");
        }
        if (e.accessKind > 3) {
            errors.push_back(ctx + ": accessKind " +
                std::to_string(e.accessKind) +
                " out of range (must be 0..3)");
        }
        // AutoJoinOnZone REQUIRES zoneDefaultMapId — else
        // the auto-join trigger never fires.
        using G = wowee::pipeline::WoweeGlobalChannels;
        if (e.accessKind == G::AutoJoinOnZone &&
            e.zoneDefaultMapId == 0) {
            errors.push_back(ctx +
                ": AutoJoinOnZone access kind with "
                "zoneDefaultMapId=0 — auto-join trigger "
                "would never fire (no zone bound to "
                "this channel)");
        }
        // Inverse: zoneDefaultMapId set with non-AutoJoin
        // kind is dead data — warn.
        if (e.zoneDefaultMapId != 0 &&
            e.accessKind != G::AutoJoinOnZone) {
            warnings.push_back(ctx +
                ": zoneDefaultMapId=" +
                std::to_string(e.zoneDefaultMapId) +
                " set but accessKind is not AutoJoinOn"
                "Zone — the field is ignored at runtime");
        }
        // Channel names must be unique — chat-window
        // dispatch identifies channels by name in /chat
        // commands.
        if (!e.name.empty() &&
            !namesSeen.insert(e.name).second) {
            errors.push_back(ctx +
                ": duplicate channel name '" + e.name +
                "' — /join command would route "
                "ambiguously");
        }
        if (!idsSeen.insert(e.channelId).second) {
            errors.push_back(ctx + ": duplicate channelId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wgch"] = base + ".wgch";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wgch: %s.wgch\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu channels, all channelIds "
                    "+ names unique, AutoJoinOnZone "
                    "channels have zoneDefaultMapId set\n",
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

bool handleGlobalChannelsCatalog(int& i, int argc, char** argv,
                                   int& outRc) {
    if (std::strcmp(argv[i], "--gen-gch") == 0 && i + 1 < argc) {
        outRc = handleGenStandard(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-gch-rp") == 0 && i + 1 < argc) {
        outRc = handleGenRoleplay(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-gch-admin") == 0 &&
        i + 1 < argc) {
        outRc = handleGenAdmin(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wgch") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wgch") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
