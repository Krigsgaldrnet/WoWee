#include "cli_loot_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_loot.hpp"
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWlotExt(std::string base) {
    stripExt(base, ".wlot");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeLoot& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeLootLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wlot\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

uint32_t totalDrops(const wowee::pipeline::WoweeLoot& c) {
    uint32_t n = 0;
    for (const auto& e : c.entries) n += static_cast<uint32_t>(e.itemDrops.size());
    return n;
}

void printGenSummary(const wowee::pipeline::WoweeLoot& c,
                     const std::string& base) {
    std::printf("Wrote %s.wlot\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  tables  : %zu (%u drop entries total)\n",
                c.entries.size(), totalDrops(c));
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterLoot";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWlotExt(base);
    auto c = wowee::pipeline::WoweeLootLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-loot")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBandit(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BanditLoot";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWlotExt(base);
    auto c = wowee::pipeline::WoweeLootLoader::makeBandit(name);
    if (!saveOrError(c, base, "gen-loot-bandit")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBoss(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BossLoot";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWlotExt(base);
    auto c = wowee::pipeline::WoweeLootLoader::makeBoss(name);
    if (!saveOrError(c, base, "gen-loot-boss")) return 1;
    printGenSummary(c, base);
    return 0;
}

void appendDropFlagsStr(std::string& s, uint8_t flags) {
    if (flags & wowee::pipeline::WoweeLoot::QuestRequired)   s += "quest ";
    if (flags & wowee::pipeline::WoweeLoot::GroupRollOnly)   s += "group ";
    if (flags & wowee::pipeline::WoweeLoot::AlwaysDrop)      s += "always ";
    if (s.empty()) s = "-";
    else if (s.back() == ' ') s.pop_back();
}

void appendTableFlagsStr(std::string& s, uint32_t flags) {
    if (flags & wowee::pipeline::WoweeLoot::QuestOnly)  s += "quest-only ";
    if (flags & wowee::pipeline::WoweeLoot::GroupOnly)  s += "group-only ";
    if (flags & wowee::pipeline::WoweeLoot::Pickpocket) s += "pickpocket ";
    if (s.empty()) s = "-";
    else if (s.back() == ' ') s.pop_back();
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWlotExt(base);
    if (!wowee::pipeline::WoweeLootLoader::exists(base)) {
        std::fprintf(stderr, "WLOT not found: %s.wlot\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeLootLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wlot"] = base + ".wlot";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        j["totalDrops"] = totalDrops(c);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            std::string fs;
            appendTableFlagsStr(fs, e.flags);
            nlohmann::json je;
            je["creatureId"] = e.creatureId;
            je["flags"] = e.flags;
            je["flagsStr"] = fs;
            je["dropCount"] = e.dropCount;
            je["moneyMinCopper"] = e.moneyMinCopper;
            je["moneyMaxCopper"] = e.moneyMaxCopper;
            nlohmann::json drops = nlohmann::json::array();
            for (const auto& d : e.itemDrops) {
                std::string dfs;
                appendDropFlagsStr(dfs, d.flags);
                drops.push_back({
                    {"itemId", d.itemId},
                    {"chancePercent", d.chancePercent},
                    {"minQty", d.minQty},
                    {"maxQty", d.maxQty},
                    {"flags", d.flags},
                    {"flagsStr", dfs},
                });
            }
            je["itemDrops"] = drops;
            arr.push_back(je);
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WLOT: %s.wlot\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  tables  : %zu (%u drop entries total)\n",
                c.entries.size(), totalDrops(c));
    if (c.entries.empty()) return 0;
    for (const auto& e : c.entries) {
        std::string fs;
        appendTableFlagsStr(fs, e.flags);
        std::printf("\n  creatureId=%u  dropCount=%u  money=%u..%uc  flags=%s\n",
                    e.creatureId, e.dropCount,
                    e.moneyMinCopper, e.moneyMaxCopper,
                    fs.c_str());
        if (e.itemDrops.empty()) {
            std::printf("    *no item drops*\n");
            continue;
        }
        std::printf("      itemId  chance%%   qty   flags\n");
        for (const auto& d : e.itemDrops) {
            std::string dfs;
            appendDropFlagsStr(dfs, d.flags);
            std::printf("    %6u    %5.1f   %u..%u   %s\n",
                        d.itemId, d.chancePercent,
                        d.minQty, d.maxQty, dfs.c_str());
        }
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWlotExt(base);
    if (!wowee::pipeline::WoweeLootLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wlot: WLOT not found: %s.wlot\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeLootLoader::load(base);
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
                          " (creatureId=" + std::to_string(e.creatureId) + ")";
        if (e.creatureId == 0) {
            errors.push_back(ctx + ": creatureId is 0");
        }
        if (e.moneyMinCopper > e.moneyMaxCopper) {
            errors.push_back(ctx + ": moneyMin > moneyMax");
        }
        if (e.dropCount == 0 && !e.itemDrops.empty()) {
            warnings.push_back(ctx +
                ": dropCount=0 but item drops are defined (none will be rolled)");
        }
        for (size_t di = 0; di < e.itemDrops.size(); ++di) {
            const auto& d = e.itemDrops[di];
            std::string dctx = ctx + " drop " + std::to_string(di) +
                                " (itemId=" + std::to_string(d.itemId) + ")";
            if (d.itemId == 0) {
                errors.push_back(dctx + ": itemId is 0");
            }
            if (!std::isfinite(d.chancePercent) ||
                d.chancePercent < 0.0f || d.chancePercent > 100.0f) {
                errors.push_back(dctx +
                    ": chancePercent must be in 0..100, got " +
                    std::to_string(d.chancePercent));
            }
            if (d.minQty == 0) {
                warnings.push_back(dctx + ": minQty=0 (drop with zero quantity)");
            }
            if (d.minQty > d.maxQty) {
                errors.push_back(dctx + ": minQty > maxQty");
            }
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.creatureId) {
                errors.push_back(ctx + ": duplicate creatureId");
                break;
            }
        }
        idsSeen.push_back(e.creatureId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wlot"] = base + ".wlot";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wlot: %s.wlot\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu tables, %u total drops, all creatureIds unique\n",
                    c.entries.size(), totalDrops(c));
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

bool handleLootCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-loot") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-loot-bandit") == 0 && i + 1 < argc) {
        outRc = handleGenBandit(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-loot-boss") == 0 && i + 1 < argc) {
        outRc = handleGenBoss(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wlot") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wlot") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
