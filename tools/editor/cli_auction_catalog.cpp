#include "cli_auction_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_auction.hpp"
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

std::string stripWaucExt(std::string base) {
    stripExt(base, ".wauc");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeAuction& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeAuctionLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wauc\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeAuction& c,
                     const std::string& base) {
    std::printf("Wrote %s.wauc\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  houses  : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterAuction";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWaucExt(base);
    auto c = wowee::pipeline::WoweeAuctionLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-auction")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenPair(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FactionPairAuction";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWaucExt(base);
    auto c = wowee::pipeline::WoweeAuctionLoader::makeFactionPair(name);
    if (!saveOrError(c, base, "gen-auction-pair")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRestricted(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RestrictedAuction";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWaucExt(base);
    auto c = wowee::pipeline::WoweeAuctionLoader::makeRestricted(name);
    if (!saveOrError(c, base, "gen-auction-restricted")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWaucExt(base);
    if (!wowee::pipeline::WoweeAuctionLoader::exists(base)) {
        std::fprintf(stderr, "WAUC not found: %s.wauc\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeAuctionLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wauc"] = base + ".wauc";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"houseId", e.houseId},
                {"auctioneerNpcId", e.auctioneerNpcId},
                {"name", e.name},
                {"factionAccess", e.factionAccess},
                {"factionAccessName", wowee::pipeline::WoweeAuction::factionAccessName(e.factionAccess)},
                {"baseDepositRateBp", e.baseDepositRateBp},
                {"houseCutRateBp", e.houseCutRateBp},
                {"maxBidCopper", e.maxBidCopper},
                {"shortHours", e.shortHours},
                {"mediumHours", e.mediumHours},
                {"longHours", e.longHours},
                {"shortMultBp", e.shortMultBp},
                {"mediumMultBp", e.mediumMultBp},
                {"longMultBp", e.longMultBp},
                {"disallowedClassMask", e.disallowedClassMask},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WAUC: %s.wauc\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  houses  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   faction    deposit%%  cut%%  durations(h)         disallow  npc\n");
    for (const auto& e : c.entries) {
        float depPct = e.baseDepositRateBp / 100.0f;
        float cutPct = e.houseCutRateBp / 100.0f;
        std::printf("  %4u   %-8s   %5.2f    %4.2f  %3u/%3u/%3u           0x%-6x  %5u    %s\n",
                    e.houseId,
                    wowee::pipeline::WoweeAuction::factionAccessName(e.factionAccess),
                    depPct, cutPct,
                    e.shortHours, e.mediumHours, e.longHours,
                    e.disallowedClassMask, e.auctioneerNpcId,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWaucExt(base);
    if (!wowee::pipeline::WoweeAuctionLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wauc: WAUC not found: %s.wauc\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeAuctionLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.houseId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.houseId == 0) errors.push_back(ctx + ": houseId is 0");
        if (e.name.empty()) errors.push_back(ctx + ": name is empty");
        if (e.factionAccess > wowee::pipeline::WoweeAuction::Both) {
            errors.push_back(ctx + ": factionAccess " +
                std::to_string(e.factionAccess) + " not in 0..3");
        }
        if (e.shortHours == 0 || e.mediumHours == 0 || e.longHours == 0) {
            errors.push_back(ctx + ": duration tier is 0 (no listing time)");
        }
        if (e.shortHours > e.mediumHours ||
            e.mediumHours > e.longHours) {
            errors.push_back(ctx +
                ": durations must satisfy short <= medium <= long");
        }
        // Cut rate > 50% is suspicious; rates > 100% mean the
        // seller pays the house more than the buyer paid.
        if (e.houseCutRateBp > 5000) {
            warnings.push_back(ctx +
                ": houseCutRateBp > 5000 (>50% cut — verify intentional)");
        }
        if (e.houseCutRateBp >= wowee::pipeline::WoweeAuction::kBpDenominator) {
            errors.push_back(ctx +
                ": houseCutRateBp >= 10000 (>=100% cut — seller loses money)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.houseId) {
                errors.push_back(ctx + ": duplicate houseId");
                break;
            }
        }
        idsSeen.push_back(e.houseId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wauc"] = base + ".wauc";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wauc: %s.wauc\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu houses, all houseIds unique\n",
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

bool handleAuctionCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-auction") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-auction-pair") == 0 && i + 1 < argc) {
        outRc = handleGenPair(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-auction-restricted") == 0 && i + 1 < argc) {
        outRc = handleGenRestricted(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wauc") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wauc") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
