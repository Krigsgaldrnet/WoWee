#include "cli_locks_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_locks.hpp"
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

std::string stripWlckExt(std::string base) {
    stripExt(base, ".wlck");
    return base;
}

void appendLockFlagsStr(std::string& s, uint32_t flags) {
    if (flags & wowee::pipeline::WoweeLock::DestructOnOpen) s += "destruct ";
    if (flags & wowee::pipeline::WoweeLock::RespawnOnKey)   s += "respawn ";
    if (flags & wowee::pipeline::WoweeLock::TrapOnFail)     s += "trap ";
    if (s.empty()) s = "-";
    else if (s.back() == ' ') s.pop_back();
}

bool saveOrError(const wowee::pipeline::WoweeLock& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeLockLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wlck\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeLock& c,
                     const std::string& base) {
    std::printf("Wrote %s.wlck\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  locks   : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterLocks";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWlckExt(base);
    auto c = wowee::pipeline::WoweeLockLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-locks")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenDungeon(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "DungeonLocks";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWlckExt(base);
    auto c = wowee::pipeline::WoweeLockLoader::makeDungeon(name);
    if (!saveOrError(c, base, "gen-locks-dungeon")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenProfessions(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ProfessionLocks";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWlckExt(base);
    auto c = wowee::pipeline::WoweeLockLoader::makeProfessions(name);
    if (!saveOrError(c, base, "gen-locks-professions")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWlckExt(base);
    if (!wowee::pipeline::WoweeLockLoader::exists(base)) {
        std::fprintf(stderr, "WLCK not found: %s.wlck\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeLockLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wlck"] = base + ".wlck";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            std::string fs;
            appendLockFlagsStr(fs, e.flags);
            nlohmann::json je;
            je["lockId"] = e.lockId;
            je["name"] = e.name;
            je["flags"] = e.flags;
            je["flagsStr"] = fs;
            nlohmann::json chans = nlohmann::json::array();
            for (int k = 0; k < wowee::pipeline::WoweeLock::kChannelSlots; ++k) {
                const auto& ch = e.channels[k];
                chans.push_back({
                    {"slot", k},
                    {"kind", ch.kind},
                    {"kindName", wowee::pipeline::WoweeLock::channelKindName(ch.kind)},
                    {"skillRequired", ch.skillRequired},
                    {"targetId", ch.targetId},
                });
            }
            je["channels"] = chans;
            arr.push_back(je);
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WLCK: %s.wlck\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  locks   : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    for (const auto& e : c.entries) {
        std::string fs;
        appendLockFlagsStr(fs, e.flags);
        std::printf("\n  lockId=%u  flags=%s  %s\n",
                    e.lockId, fs.c_str(), e.name.c_str());
        for (int k = 0; k < wowee::pipeline::WoweeLock::kChannelSlots; ++k) {
            const auto& ch = e.channels[k];
            if (ch.kind == wowee::pipeline::WoweeLock::ChannelNone) continue;
            std::printf("    slot %d : %-9s  target=%u  skillReq=%u\n",
                        k,
                        wowee::pipeline::WoweeLock::channelKindName(ch.kind),
                        ch.targetId, ch.skillRequired);
        }
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWlckExt(base);
    if (!wowee::pipeline::WoweeLockLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wlck: WLCK not found: %s.wlck\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeLockLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    idsSeen.reserve(c.entries.size());
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.lockId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.lockId == 0) {
            errors.push_back(ctx + ": lockId is 0");
        }
        // At least one channel must be active or the lock can
        // never be opened.
        bool anyActive = false;
        for (int ci = 0; ci < wowee::pipeline::WoweeLock::kChannelSlots; ++ci) {
            const auto& ch = e.channels[ci];
            if (ch.kind != wowee::pipeline::WoweeLock::ChannelNone) {
                anyActive = true;
            }
            if (ch.kind > wowee::pipeline::WoweeLock::ChannelDamage) {
                errors.push_back(ctx + " slot " + std::to_string(ci) +
                    ": kind " + std::to_string(ch.kind) +
                    " not in known range 0..4");
            }
            // Item / Spell / Lockpick channels need a non-zero
            // targetId; Damage channels don't.
            if ((ch.kind == wowee::pipeline::WoweeLock::ChannelItem ||
                 ch.kind == wowee::pipeline::WoweeLock::ChannelSpell ||
                 ch.kind == wowee::pipeline::WoweeLock::ChannelLockpick) &&
                ch.targetId == 0) {
                errors.push_back(ctx + " slot " + std::to_string(ci) +
                    ": kind requires non-zero targetId");
            }
            // skillRequired only meaningful for Lockpick — flag
            // odd usage on other kinds.
            if (ch.kind != wowee::pipeline::WoweeLock::ChannelLockpick &&
                ch.kind != wowee::pipeline::WoweeLock::ChannelNone &&
                ch.skillRequired != 0) {
                warnings.push_back(ctx + " slot " + std::to_string(ci) +
                    ": skillRequired set on non-Lockpick channel (ignored at runtime)");
            }
        }
        if (!anyActive) {
            errors.push_back(ctx + ": all 5 channels are None (lock can never open)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.lockId) {
                errors.push_back(ctx + ": duplicate lockId");
                break;
            }
        }
        idsSeen.push_back(e.lockId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wlck"] = base + ".wlck";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wlck: %s.wlck\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu locks, all lockIds unique\n",
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

bool handleLocksCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-locks") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-locks-dungeon") == 0 && i + 1 < argc) {
        outRc = handleGenDungeon(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-locks-professions") == 0 && i + 1 < argc) {
        outRc = handleGenProfessions(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wlck") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wlck") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
