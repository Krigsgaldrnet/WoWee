#include "cli_group_compositions_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_group_compositions.hpp"
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

std::string stripWgrpExt(std::string base) {
    stripExt(base, ".wgrp");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeGroupComposition& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeGroupCompositionLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wgrp\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeGroupComposition& c,
                     const std::string& base) {
    std::printf("Wrote %s.wgrp\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  comps   : %zu\n", c.entries.size());
}

int handleGenFiveMan(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FiveManComps";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgrpExt(base);
    auto c = wowee::pipeline::WoweeGroupCompositionLoader::makeFiveMan(name);
    if (!saveOrError(c, base, "gen-grp")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRaid10(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "Raid10Comps";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgrpExt(base);
    auto c = wowee::pipeline::WoweeGroupCompositionLoader::makeRaid10(name);
    if (!saveOrError(c, base, "gen-grp-raid10")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRaid25(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "Raid25Comps";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgrpExt(base);
    auto c = wowee::pipeline::WoweeGroupCompositionLoader::makeRaid25(name);
    if (!saveOrError(c, base, "gen-grp-raid25")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgrpExt(base);
    if (!wowee::pipeline::WoweeGroupCompositionLoader::exists(base)) {
        std::fprintf(stderr, "WGRP not found: %s.wgrp\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGroupCompositionLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wgrp"] = base + ".wgrp";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"compId", e.compId},
                {"name", e.name},
                {"description", e.description},
                {"mapId", e.mapId},
                {"difficultyId", e.difficultyId},
                {"requiredTanks", e.requiredTanks},
                {"requiredHealers", e.requiredHealers},
                {"requiredDamageDealers", e.requiredDamageDealers},
                {"minPartySize", e.minPartySize},
                {"maxPartySize", e.maxPartySize},
                {"requireSpec", e.requireSpec != 0},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WGRP: %s.wgrp\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  comps   : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    map   diff   tanks  heal   dps   min  max  spec   name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %4u   %4u    %3u   %3u   %3u   %3u  %3u  %s    %s\n",
                    e.compId, e.mapId, e.difficultyId,
                    e.requiredTanks, e.requiredHealers,
                    e.requiredDamageDealers,
                    e.minPartySize, e.maxPartySize,
                    e.requireSpec ? "yes" : "no ",
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgrpExt(base);
    if (!wowee::pipeline::WoweeGroupCompositionLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wgrp: WGRP not found: %s.wgrp\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGroupCompositionLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.compId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.compId == 0)
            errors.push_back(ctx + ": compId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.mapId == 0)
            errors.push_back(ctx +
                ": mapId is 0 — composition is unbound to a map");
        if (e.minPartySize > e.maxPartySize) {
            errors.push_back(ctx + ": minPartySize " +
                std::to_string(e.minPartySize) +
                " > maxPartySize " +
                std::to_string(e.maxPartySize));
        }
        // Sum of required roles must fit in the party size.
        uint32_t requiredSum = e.requiredTanks +
                                e.requiredHealers +
                                e.requiredDamageDealers;
        if (requiredSum > e.maxPartySize) {
            errors.push_back(ctx +
                ": required roles sum " + std::to_string(requiredSum) +
                " > maxPartySize " +
                std::to_string(e.maxPartySize) +
                " — composition can never be filled");
        }
        if (requiredSum < e.minPartySize) {
            warnings.push_back(ctx +
                ": required roles sum " + std::to_string(requiredSum) +
                " < minPartySize " +
                std::to_string(e.minPartySize) +
                " — extra slots have no role requirement");
        }
        // Standard sizes: 5 / 10 / 25 / 40.
        if (e.maxPartySize != 5 && e.maxPartySize != 10 &&
            e.maxPartySize != 25 && e.maxPartySize != 40) {
            warnings.push_back(ctx +
                ": non-standard maxPartySize " +
                std::to_string(e.maxPartySize) +
                " (canonical sizes are 5/10/25/40)");
        }
        // Zero-tank composition is unusual but legal for
        // tank-immune content; warn so the author confirms.
        if (e.requiredTanks == 0) {
            warnings.push_back(ctx +
                ": requiredTanks=0 — zero-tank composition. "
                "Legal for tank-immune fights but unusual; "
                "double-check this is intentional");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.compId) {
                errors.push_back(ctx + ": duplicate compId");
                break;
            }
        }
        idsSeen.push_back(e.compId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wgrp"] = base + ".wgrp";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wgrp: %s.wgrp\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu compositions, all compIds unique\n",
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

bool handleGroupCompositionsCatalog(int& i, int argc, char** argv,
                                    int& outRc) {
    if (std::strcmp(argv[i], "--gen-grp") == 0 && i + 1 < argc) {
        outRc = handleGenFiveMan(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-grp-raid10") == 0 && i + 1 < argc) {
        outRc = handleGenRaid10(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-grp-raid25") == 0 && i + 1 < argc) {
        outRc = handleGenRaid25(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wgrp") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wgrp") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
