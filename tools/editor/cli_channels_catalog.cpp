#include "cli_channels_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_channels.hpp"
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

std::string stripWchnExt(std::string base) {
    stripExt(base, ".wchn");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeChannel& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeChannelLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wchn\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeChannel& c,
                     const std::string& base) {
    std::printf("Wrote %s.wchn\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  channels : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterChannels";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWchnExt(base);
    auto c = wowee::pipeline::WoweeChannelLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-channels")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenCity(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "CityChannels";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWchnExt(base);
    auto c = wowee::pipeline::WoweeChannelLoader::makeCity(name);
    if (!saveOrError(c, base, "gen-channels-city")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenModerated(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ModeratedChannels";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWchnExt(base);
    auto c = wowee::pipeline::WoweeChannelLoader::makeModerated(name);
    if (!saveOrError(c, base, "gen-channels-moderated")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWchnExt(base);
    if (!wowee::pipeline::WoweeChannelLoader::exists(base)) {
        std::fprintf(stderr, "WCHN not found: %s.wchn\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeChannelLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wchn"] = base + ".wchn";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"channelId", e.channelId},
                {"name", e.name},
                {"description", e.description},
                {"channelType", e.channelType},
                {"channelTypeName", wowee::pipeline::WoweeChannel::channelTypeName(e.channelType)},
                {"factionAccess", e.factionAccess},
                {"factionAccessName", wowee::pipeline::WoweeChannel::factionAccessName(e.factionAccess)},
                {"autoJoin", e.autoJoin},
                {"announce", e.announce},
                {"moderated", e.moderated},
                {"minLevel", e.minLevel},
                {"areaIdGate", e.areaIdGate},
                {"mapIdGate", e.mapIdGate},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WCHN: %s.wchn\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  channels : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    type           faction    auto  ann  mod  lvl   map  area  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-13s  %-8s   %u     %u    %u    %3u   %3u  %4u  %s\n",
                    e.channelId,
                    wowee::pipeline::WoweeChannel::channelTypeName(e.channelType),
                    wowee::pipeline::WoweeChannel::factionAccessName(e.factionAccess),
                    e.autoJoin, e.announce, e.moderated,
                    e.minLevel, e.mapIdGate, e.areaIdGate,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWchnExt(base);
    if (!wowee::pipeline::WoweeChannelLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wchn: WCHN not found: %s.wchn\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeChannelLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.channelId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.channelId == 0) errors.push_back(ctx + ": channelId is 0");
        if (e.name.empty()) errors.push_back(ctx + ": name is empty");
        if (e.channelType > wowee::pipeline::WoweeChannel::Pvp) {
            errors.push_back(ctx + ": channelType " +
                std::to_string(e.channelType) + " not in 0..9");
        }
        if (e.factionAccess > wowee::pipeline::WoweeChannel::Both) {
            errors.push_back(ctx + ": factionAccess " +
                std::to_string(e.factionAccess) + " not in 0..2");
        }
        // AreaLocal / Zone channels with no area gate aren't broken,
        // but world / continent channels with an area gate is
        // contradictory.
        if ((e.channelType == wowee::pipeline::WoweeChannel::World ||
             e.channelType == wowee::pipeline::WoweeChannel::Continent) &&
            (e.areaIdGate != 0 || e.mapIdGate != 0)) {
            warnings.push_back(ctx +
                ": world/continent channel with area or map gate "
                "(gate is silently ignored at runtime)");
        }
        if (e.minLevel == 0) {
            warnings.push_back(ctx + ": minLevel=0 (no level gate at all)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.channelId) {
                errors.push_back(ctx + ": duplicate channelId");
                break;
            }
        }
        idsSeen.push_back(e.channelId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wchn"] = base + ".wchn";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wchn: %s.wchn\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu channels, all channelIds unique\n",
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

bool handleChannelsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-channels") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-channels-city") == 0 && i + 1 < argc) {
        outRc = handleGenCity(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-channels-moderated") == 0 && i + 1 < argc) {
        outRc = handleGenModerated(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wchn") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wchn") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
