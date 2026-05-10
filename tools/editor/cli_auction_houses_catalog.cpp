#include "cli_auction_houses_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_auction_houses.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWauhExt(std::string base) {
    stripExt(base, ".wauh");
    return base;
}

const char* factionAccessName(uint8_t f) {
    using A = wowee::pipeline::WoweeAuctionHouses;
    switch (f) {
        case A::Both:     return "both";
        case A::Alliance: return "alliance";
        case A::Horde:    return "horde";
        case A::Neutral:  return "neutral";
        default:          return "?";
    }
}

bool saveOrError(const wowee::pipeline::WoweeAuctionHouses& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeAuctionHousesLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wauh\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeAuctionHouses& c,
                     const std::string& base) {
    std::printf("Wrote %s.wauh\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  houses  : %zu\n", c.entries.size());
}

int handleGenStormwind(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StormwindAH";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWauhExt(base);
    auto c = wowee::pipeline::WoweeAuctionHousesLoader::
        makeStormwindAH(name);
    if (!saveOrError(c, base, "gen-auh-stormwind")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenOrgrimmar(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "OrgrimmarAH";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWauhExt(base);
    auto c = wowee::pipeline::WoweeAuctionHousesLoader::
        makeOrgrimmarAH(name);
    if (!saveOrError(c, base, "gen-auh-orgrimmar")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBootyBay(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BootyBayAH";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWauhExt(base);
    auto c = wowee::pipeline::WoweeAuctionHousesLoader::
        makeBootyBayAH(name);
    if (!saveOrError(c, base, "gen-auh-bootybay")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWauhExt(base);
    if (!wowee::pipeline::WoweeAuctionHousesLoader::exists(base)) {
        std::fprintf(stderr, "WAUH not found: %s.wauh\n",
                     base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeAuctionHousesLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wauh"] = base + ".wauh";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"ahId", e.ahId},
                {"name", e.name},
                {"factionAccess", e.factionAccess},
                {"factionAccessName",
                    factionAccessName(e.factionAccess)},
                {"depositRatePct", e.depositRatePct},
                {"cutPct", e.cutPct},
                {"minListingDurationHours",
                    e.minListingDurationHours},
                {"maxListingDurationHours",
                    e.maxListingDurationHours},
                {"feePerSlot", e.feePerSlot},
                {"npcAuctioneerId", e.npcAuctioneerId},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WAUH: %s.wauh\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  houses  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id  faction    deposit%%  cut%%  hours    fee  auctioneer  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u  %-8s  %5u    %5u   %2u-%-2u  %5u  %10u  %s\n",
                    e.ahId,
                    factionAccessName(e.factionAccess),
                    e.depositRatePct,
                    e.cutPct,
                    e.minListingDurationHours,
                    e.maxListingDurationHours,
                    e.feePerSlot,
                    e.npcAuctioneerId,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWauhExt(base);
    if (!wowee::pipeline::WoweeAuctionHousesLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wauh: WAUH not found: %s.wauh\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeAuctionHousesLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    std::set<uint32_t> npcsSeen;
    using Pair = std::pair<uint8_t, std::string>;
    std::set<Pair> factionNamePairs;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.ahId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.ahId == 0)
            errors.push_back(ctx + ": ahId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.factionAccess > 3) {
            errors.push_back(ctx + ": factionAccess " +
                std::to_string(e.factionAccess) +
                " out of range (0..3)");
        }
        if (e.depositRatePct > 10000) {
            errors.push_back(ctx + ": depositRatePct " +
                std::to_string(e.depositRatePct) +
                " exceeds 10000 (100% basis points)");
        }
        if (e.cutPct > 10000) {
            errors.push_back(ctx + ": cutPct " +
                std::to_string(e.cutPct) +
                " exceeds 10000 (100% basis points)");
        }
        // CRITICAL invariant: deposit + cut combined
        // must be < 10000 (100%), else seller would
        // lose money on every successful sale.
        // Pretty close to 100% (e.g. 50% + 50% = 100%
        // is valid but unsellable) — error at sum >=
        // 10000.
        uint32_t totalRate = static_cast<uint32_t>(
            e.depositRatePct) +
            static_cast<uint32_t>(e.cutPct);
        if (totalRate >= 10000) {
            errors.push_back(ctx +
                ": depositRatePct=" +
                std::to_string(e.depositRatePct) +
                " + cutPct=" + std::to_string(e.cutPct)
                + " = " + std::to_string(totalRate) +
                " basis points — seller would lose "
                "money on every sale (combined rates "
                ">= 100%)");
        }
        // Warn on combined > 50% — economically
        // viable but harsh enough that listings would
        // dry up.
        if (totalRate > 5000 && totalRate < 10000) {
            warnings.push_back(ctx +
                ": combined deposit+cut=" +
                std::to_string(totalRate / 100) +
                "% exceeds 50% — sellers might find "
                "this AH unprofitable; verify intentional"
                " (e.g. neutral AH penalty)");
        }
        if (e.maxListingDurationHours == 0) {
            errors.push_back(ctx +
                ": maxListingDurationHours is 0 — no "
                "duration available, AH would reject "
                "all listings");
        }
        if (e.minListingDurationHours >
            e.maxListingDurationHours) {
            errors.push_back(ctx +
                ": minListingDurationHours=" +
                std::to_string(e.minListingDurationHours) +
                " > maxListingDurationHours=" +
                std::to_string(e.maxListingDurationHours)
                + " — no valid duration in range");
        }
        if (e.npcAuctioneerId == 0) {
            warnings.push_back(ctx +
                ": npcAuctioneerId is 0 — no NPC bound,"
                " AH only reachable via direct UI "
                "(e.g. console command)");
        }
        // Same NPC bound to two AH configs would
        // dispatch ambiguously when right-clicked.
        if (e.npcAuctioneerId != 0 &&
            !npcsSeen.insert(e.npcAuctioneerId).second) {
            errors.push_back(ctx +
                ": npcAuctioneerId " +
                std::to_string(e.npcAuctioneerId) +
                " already bound to another AH — gossip "
                "dispatch would be ambiguous");
        }
        // Duplicate (faction, name) — UI tab dispatch
        // would tie.
        if (e.factionAccess <= 3 && !e.name.empty()) {
            Pair p{e.factionAccess, e.name};
            if (!factionNamePairs.insert(p).second) {
                errors.push_back(ctx +
                    ": duplicate (factionAccess=" +
                    std::to_string(e.factionAccess) +
                    ", name=" + e.name +
                    ") — AH browser would route "
                    "ambiguously");
            }
        }
        if (!idsSeen.insert(e.ahId).second) {
            errors.push_back(ctx + ": duplicate ahId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wauh"] = base + ".wauh";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wauh: %s.wauh\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu houses, all ahIds + "
                    "(faction,name) + npcAuctioneerId "
                    "unique, factionAccess 0..3, "
                    "depositRatePct + cutPct in 0..10000 "
                    "and combined < 10000, valid "
                    "min<=max listing duration\n",
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

bool handleAuctionHousesCatalog(int& i, int argc, char** argv,
                                  int& outRc) {
    if (std::strcmp(argv[i], "--gen-auh-stormwind") == 0 &&
        i + 1 < argc) {
        outRc = handleGenStormwind(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-auh-orgrimmar") == 0 &&
        i + 1 < argc) {
        outRc = handleGenOrgrimmar(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-auh-bootybay") == 0 &&
        i + 1 < argc) {
        outRc = handleGenBootyBay(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wauh") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wauh") == 0 &&
        i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
