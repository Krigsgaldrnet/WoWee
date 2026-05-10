#include "cli_item_qualities_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_item_qualities.hpp"
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

std::string stripWiqrExt(std::string base) {
    stripExt(base, ".wiqr");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeItemQuality& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeItemQualityLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wiqr\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeItemQuality& c,
                     const std::string& base) {
    std::printf("Wrote %s.wiqr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  tiers   : %zu\n", c.entries.size());
}

int handleGenStandard(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StandardQualities";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWiqrExt(base);
    auto c = wowee::pipeline::WoweeItemQualityLoader::makeStandard(name);
    if (!saveOrError(c, base, "gen-iqr")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenServerCustom(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ServerCustomQualities";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWiqrExt(base);
    auto c = wowee::pipeline::WoweeItemQualityLoader::makeServerCustom(name);
    if (!saveOrError(c, base, "gen-iqr-server")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRaidTiers(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RaidProgressionQualities";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWiqrExt(base);
    auto c = wowee::pipeline::WoweeItemQualityLoader::makeRaidTiers(name);
    if (!saveOrError(c, base, "gen-iqr-raid")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWiqrExt(base);
    if (!wowee::pipeline::WoweeItemQualityLoader::exists(base)) {
        std::fprintf(stderr, "WIQR not found: %s.wiqr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeItemQualityLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wiqr"] = base + ".wiqr";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"qualityId", e.qualityId},
                {"name", e.name},
                {"description", e.description},
                {"nameColorRGBA", e.nameColorRGBA},
                {"borderColorRGBA", e.borderColorRGBA},
                {"vendorPriceMultiplier", e.vendorPriceMultiplier},
                {"minLevelToDrop", e.minLevelToDrop},
                {"maxLevelToDrop", e.maxLevelToDrop},
                {"canBeDisenchanted", e.canBeDisenchanted != 0},
                {"inventoryBorderTexture", e.inventoryBorderTexture},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WIQR: %s.wiqr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  tiers   : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id     name        nameColor    vendorMul   minLvl   maxLvl   DE   border\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-10s   0x%08x   %6.2fx     %4u     %4u   %s   %s\n",
                    e.qualityId, e.name.c_str(),
                    e.nameColorRGBA, e.vendorPriceMultiplier,
                    e.minLevelToDrop, e.maxLevelToDrop,
                    e.canBeDisenchanted ? "yes" : "no ",
                    e.inventoryBorderTexture.empty() ? "(none)" :
                        e.inventoryBorderTexture.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWiqrExt(base);
    if (!wowee::pipeline::WoweeItemQualityLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wiqr: WIQR not found: %s.wiqr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeItemQualityLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.qualityId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.vendorPriceMultiplier < 0.0f) {
            errors.push_back(ctx +
                ": vendorPriceMultiplier < 0 — vendor would "
                "pay the player to take items");
        }
        if (e.maxLevelToDrop != 0 &&
            e.minLevelToDrop > e.maxLevelToDrop) {
            errors.push_back(ctx + ": minLevelToDrop " +
                std::to_string(e.minLevelToDrop) +
                " > maxLevelToDrop " +
                std::to_string(e.maxLevelToDrop) +
                " — quality will never drop");
        }
        if (e.minLevelToDrop > 80) {
            warnings.push_back(ctx +
                ": minLevelToDrop " +
                std::to_string(e.minLevelToDrop) +
                " > 80 — quality unreachable at WotLK cap");
        }
        if (e.vendorPriceMultiplier > 100.0f) {
            warnings.push_back(ctx +
                ": vendorPriceMultiplier " +
                std::to_string(e.vendorPriceMultiplier) +
                "x is very high — sanity check the economy");
        }
        // Pure transparent color is suspicious (alpha=0).
        if ((e.nameColorRGBA & 0xFF000000u) == 0) {
            warnings.push_back(ctx +
                ": nameColorRGBA has alpha=0 — text will be "
                "invisible in tooltips");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.qualityId) {
                errors.push_back(ctx + ": duplicate qualityId");
                break;
            }
        }
        idsSeen.push_back(e.qualityId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wiqr"] = base + ".wiqr";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wiqr: %s.wiqr\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu tiers, all qualityIds unique\n",
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

bool handleItemQualitiesCatalog(int& i, int argc, char** argv,
                                int& outRc) {
    if (std::strcmp(argv[i], "--gen-iqr") == 0 && i + 1 < argc) {
        outRc = handleGenStandard(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-iqr-server") == 0 && i + 1 < argc) {
        outRc = handleGenServerCustom(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-iqr-raid") == 0 && i + 1 < argc) {
        outRc = handleGenRaidTiers(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wiqr") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wiqr") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
