#include "cli_buff_book_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_buff_book.hpp"
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

std::string stripWbabExt(std::string base) {
    stripExt(base, ".wbab");
    return base;
}

const char* statBonusKindName(uint8_t k) {
    using B = wowee::pipeline::WoweeBuffBook;
    switch (k) {
        case B::Stamina:     return "stamina";
        case B::Intellect:   return "intellect";
        case B::Spirit:      return "spirit";
        case B::AllStats:    return "allstats";
        case B::Armor:       return "armor";
        case B::SpellPower:  return "spellpower";
        case B::AttackPower: return "attackpower";
        case B::CritRating:  return "critrating";
        case B::HasteRating: return "hasterating";
        case B::ManaRegen:   return "manaregen";
        case B::Other:       return "other";
        default:             return "unknown";
    }
}

std::string targetMaskString(uint8_t m) {
    using B = wowee::pipeline::WoweeBuffBook;
    std::string out;
    auto add = [&](const char* tag) {
        if (!out.empty()) out += "+";
        out += tag;
    };
    if (m & B::TargetSelf)     add("self");
    if (m & B::TargetParty)    add("party");
    if (m & B::TargetRaid)     add("raid");
    if (m & B::TargetFriendly) add("friendly");
    if (out.empty()) out = "none";
    return out;
}

bool saveOrError(const wowee::pipeline::WoweeBuffBook& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeBuffBookLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wbab\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeBuffBook& c,
                     const std::string& base) {
    std::printf("Wrote %s.wbab\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  buffs   : %zu\n", c.entries.size());
}

int handleGenMage(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MageBuffBook";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbabExt(base);
    auto c = wowee::pipeline::WoweeBuffBookLoader::makeMage(name);
    if (!saveOrError(c, base, "gen-bab")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenDruid(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "DruidBuffBook";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbabExt(base);
    auto c = wowee::pipeline::WoweeBuffBookLoader::makeDruid(name);
    if (!saveOrError(c, base, "gen-bab-druid")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRaidMax(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RaidMaxBuffs";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbabExt(base);
    auto c = wowee::pipeline::WoweeBuffBookLoader::makeRaidMax(name);
    if (!saveOrError(c, base, "gen-bab-raid")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWbabExt(base);
    if (!wowee::pipeline::WoweeBuffBookLoader::exists(base)) {
        std::fprintf(stderr, "WBAB not found: %s.wbab\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeBuffBookLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wbab"] = base + ".wbab";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"buffId", e.buffId},
                {"name", e.name},
                {"description", e.description},
                {"spellId", e.spellId},
                {"castClassMask", e.castClassMask},
                {"targetTypeMask", e.targetTypeMask},
                {"targetTypeNames", targetMaskString(e.targetTypeMask)},
                {"statBonusKind", e.statBonusKind},
                {"statBonusKindName",
                    statBonusKindName(e.statBonusKind)},
                {"rank", e.rank},
                {"maxStackCount", e.maxStackCount},
                {"statBonusAmount", e.statBonusAmount},
                {"duration", e.duration},
                {"previousRankId", e.previousRankId},
                {"nextRankId", e.nextRankId},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WBAB: %s.wbab\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  buffs   : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   spell  class  tgt              stat        rk  amt   dur(s)  prev   next  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u  %5u   %4u  %-15s  %-11s  %2u  %4d  %5u  %4u  %4u  %s\n",
                    e.buffId, e.spellId, e.castClassMask,
                    targetMaskString(e.targetTypeMask).c_str(),
                    statBonusKindName(e.statBonusKind),
                    e.rank, e.statBonusAmount, e.duration,
                    e.previousRankId, e.nextRankId,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWbabExt(base);
    if (!wowee::pipeline::WoweeBuffBookLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wbab: WBAB not found: %s.wbab\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeBuffBookLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.buffId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.buffId == 0)
            errors.push_back(ctx + ": buffId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.spellId == 0) {
            errors.push_back(ctx +
                ": spellId is 0 — buff has no spell to "
                "cast");
        }
        if (e.castClassMask == 0) {
            errors.push_back(ctx +
                ": castClassMask is 0 — no class can cast "
                "this buff");
        }
        if (e.targetTypeMask == 0) {
            errors.push_back(ctx +
                ": targetTypeMask is 0 — buff has no valid "
                "targets");
        }
        if (e.statBonusKind > 9 && e.statBonusKind != 255) {
            errors.push_back(ctx + ": statBonusKind " +
                std::to_string(e.statBonusKind) +
                " out of range (must be 0..9 or 255 Other)");
        }
        if (e.rank == 0) {
            warnings.push_back(ctx +
                ": rank is 0 — ranks are 1-indexed; rank 0 "
                "may sort unexpectedly in spellbook UI");
        }
        if (e.maxStackCount == 0) {
            warnings.push_back(ctx +
                ": maxStackCount=0 — buff cannot be applied "
                "(zero stack ceiling)");
        }
        // Self-reference check: an entry's own id should
        // never appear in its own next/previous fields.
        if (e.previousRankId == e.buffId) {
            errors.push_back(ctx +
                ": previousRankId equals buffId — would "
                "create a 1-element rank cycle");
        }
        if (e.nextRankId == e.buffId) {
            errors.push_back(ctx +
                ": nextRankId equals buffId — would create "
                "a 1-element rank cycle");
        }
        if (!idsSeen.insert(e.buffId).second) {
            errors.push_back(ctx + ": duplicate buffId");
        }
    }
    // Cross-entry checks: validate the rank chain back-
    // edges. If A.nextRankId = B then B.previousRankId
    // must = A.buffId, and vice versa. Also detect
    // chain cycles.
    auto findIdx = [&](uint32_t id) -> int {
        for (size_t k = 0; k < c.entries.size(); ++k) {
            if (c.entries[k].buffId == id) {
                return static_cast<int>(k);
            }
        }
        return -1;
    };
    for (const auto& e : c.entries) {
        if (e.nextRankId != 0) {
            int next = findIdx(e.nextRankId);
            if (next < 0) {
                errors.push_back("entry id=" +
                    std::to_string(e.buffId) +
                    " (" + e.name + "): nextRankId=" +
                    std::to_string(e.nextRankId) +
                    " references missing entry");
            } else if (c.entries[next].previousRankId !=
                       e.buffId) {
                errors.push_back("rank chain back-edge "
                    "broken: id=" + std::to_string(e.buffId) +
                    " (" + e.name + ").nextRankId=" +
                    std::to_string(e.nextRankId) +
                    " but id=" + std::to_string(e.nextRankId) +
                    " (" + c.entries[next].name +
                    ").previousRankId=" +
                    std::to_string(
                        c.entries[next].previousRankId) +
                    " (expected " +
                    std::to_string(e.buffId) + ")");
            }
        }
        if (e.previousRankId != 0) {
            int prev = findIdx(e.previousRankId);
            if (prev < 0) {
                errors.push_back("entry id=" +
                    std::to_string(e.buffId) +
                    " (" + e.name + "): previousRankId=" +
                    std::to_string(e.previousRankId) +
                    " references missing entry");
            }
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wbab"] = base + ".wbab";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wbab: %s.wbab\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu buffs, all buffIds unique, "
                    "rank chain back-edges symmetric\n",
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

bool handleBuffBookCatalog(int& i, int argc, char** argv,
                            int& outRc) {
    if (std::strcmp(argv[i], "--gen-bab") == 0 && i + 1 < argc) {
        outRc = handleGenMage(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-bab-druid") == 0 && i + 1 < argc) {
        outRc = handleGenDruid(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-bab-raid") == 0 && i + 1 < argc) {
        outRc = handleGenRaidMax(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wbab") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wbab") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
