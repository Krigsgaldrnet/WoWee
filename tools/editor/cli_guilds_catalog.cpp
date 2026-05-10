#include "cli_guilds_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_guilds.hpp"
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

std::string stripWgldExt(std::string base) {
    stripExt(base, ".wgld");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeGuild& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeGuildLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wgld\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeGuild& c,
                     const std::string& base) {
    std::printf("Wrote %s.wgld\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  guilds  : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterGuilds";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgldExt(base);
    auto c = wowee::pipeline::WoweeGuildLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-guilds")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenFull(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FullGuild";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgldExt(base);
    auto c = wowee::pipeline::WoweeGuildLoader::makeFull(name);
    if (!saveOrError(c, base, "gen-guilds-full")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenFactionPair(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FactionPair";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgldExt(base);
    auto c = wowee::pipeline::WoweeGuildLoader::makeFactionPair(name);
    if (!saveOrError(c, base, "gen-guilds-pair")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgldExt(base);
    if (!wowee::pipeline::WoweeGuildLoader::exists(base)) {
        std::fprintf(stderr, "WGLD not found: %s.wgld\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGuildLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wgld"] = base + ".wgld";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            nlohmann::json je;
            je["guildId"] = e.guildId;
            je["name"] = e.name;
            je["leaderName"] = e.leaderName;
            je["motd"] = e.motd;
            je["info"] = e.info;
            je["creationDate"] = e.creationDate;
            je["experience"] = e.experience;
            je["level"] = e.level;
            je["factionId"] = e.factionId;
            je["factionName"] = wowee::pipeline::WoweeGuild::factionName(e.factionId);
            je["bankCopper"] = e.bankCopper;
            je["emblem"] = e.emblem;
            nlohmann::json ranks = nlohmann::json::array();
            for (const auto& r : e.ranks) {
                ranks.push_back({
                    {"rankIndex", r.rankIndex},
                    {"name", r.name},
                    {"permissionsMask", r.permissionsMask},
                    {"moneyPerDayCopper", r.moneyPerDayCopper},
                });
            }
            je["ranks"] = ranks;
            nlohmann::json members = nlohmann::json::array();
            for (const auto& m : e.members) {
                members.push_back({
                    {"characterName", m.characterName},
                    {"rankIndex", m.rankIndex},
                    {"joinedDate", m.joinedDate},
                    {"publicNote", m.publicNote},
                    {"officerNote", m.officerNote},
                });
            }
            je["members"] = members;
            nlohmann::json tabs = nlohmann::json::array();
            for (const auto& t : e.bankTabs) {
                tabs.push_back({
                    {"tabIndex", t.tabIndex},
                    {"name", t.name},
                    {"iconPath", t.iconPath},
                    {"depositPermissionMask", t.depositPermissionMask},
                    {"withdrawPermissionMask", t.withdrawPermissionMask},
                    {"viewPermissionMask", t.viewPermissionMask},
                });
            }
            je["bankTabs"] = tabs;
            nlohmann::json perks = nlohmann::json::array();
            for (const auto& p : e.perks) {
                perks.push_back({
                    {"perkId", p.perkId},
                    {"name", p.name},
                    {"spellId", p.spellId},
                    {"requiredGuildLevel", p.requiredGuildLevel},
                });
            }
            je["perks"] = perks;
            arr.push_back(je);
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WGLD: %s.wgld\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  guilds  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    for (const auto& e : c.entries) {
        std::printf("\n  guildId=%u  faction=%s  level=%u  bank=%uc\n",
                    e.guildId,
                    wowee::pipeline::WoweeGuild::factionName(e.factionId),
                    e.level, e.bankCopper);
        std::printf("    name      : %s\n", e.name.c_str());
        std::printf("    leader    : %s\n", e.leaderName.c_str());
        if (!e.motd.empty()) {
            std::printf("    motd      : %s\n", e.motd.c_str());
        }
        std::printf("    ranks=%zu  members=%zu  tabs=%zu  perks=%zu\n",
                    e.ranks.size(), e.members.size(),
                    e.bankTabs.size(), e.perks.size());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgldExt(base);
    if (!wowee::pipeline::WoweeGuildLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wgld: WGLD not found: %s.wgld\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGuildLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "guild " + std::to_string(k) +
                          " (id=" + std::to_string(e.guildId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.guildId == 0) errors.push_back(ctx + ": guildId is 0");
        if (e.name.empty()) errors.push_back(ctx + ": name is empty");
        if (e.leaderName.empty()) {
            errors.push_back(ctx + ": leaderName is empty");
        }
        if (e.factionId > wowee::pipeline::WoweeGuild::Horde) {
            errors.push_back(ctx + ": factionId " +
                std::to_string(e.factionId) + " not in 0..1");
        }
        if (e.ranks.empty()) {
            errors.push_back(ctx + ": no ranks (cannot have any members)");
        }
        // Validate that each member's rankIndex resolves.
        uint8_t maxRankIdx = 0;
        for (const auto& r : e.ranks) {
            if (r.rankIndex > maxRankIdx) maxRankIdx = r.rankIndex;
        }
        for (size_t mi = 0; mi < e.members.size(); ++mi) {
            const auto& m = e.members[mi];
            if (m.rankIndex > maxRankIdx) {
                errors.push_back(ctx + " member " + std::to_string(mi) +
                    " (" + m.characterName + "): rankIndex " +
                    std::to_string(m.rankIndex) +
                    " exceeds highest defined rank " +
                    std::to_string(maxRankIdx));
            }
            if (m.characterName.empty()) {
                errors.push_back(ctx + " member " + std::to_string(mi) +
                    ": characterName is empty");
            }
        }
        // Bank tab indices should be unique.
        std::vector<uint8_t> tabIdxSeen;
        for (const auto& t : e.bankTabs) {
            for (uint8_t prev : tabIdxSeen) {
                if (prev == t.tabIndex) {
                    errors.push_back(ctx +
                        ": duplicate bank tabIndex " +
                        std::to_string(t.tabIndex));
                    break;
                }
            }
            tabIdxSeen.push_back(t.tabIndex);
        }
        // Perks should reference a non-zero spellId.
        for (size_t pi = 0; pi < e.perks.size(); ++pi) {
            const auto& p = e.perks[pi];
            if (p.spellId == 0) {
                warnings.push_back(ctx + " perk " + std::to_string(pi) +
                    " (" + p.name + "): spellId is 0 (perk does nothing)");
            }
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.guildId) {
                errors.push_back(ctx + ": duplicate guildId");
                break;
            }
        }
        idsSeen.push_back(e.guildId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wgld"] = base + ".wgld";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wgld: %s.wgld\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu guilds, all guildIds unique\n",
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

bool handleGuildsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-guilds") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-guilds-full") == 0 && i + 1 < argc) {
        outRc = handleGenFull(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-guilds-pair") == 0 && i + 1 < argc) {
        outRc = handleGenFactionPair(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wgld") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wgld") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
