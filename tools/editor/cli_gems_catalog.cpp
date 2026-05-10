#include "cli_gems_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_gems.hpp"
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

std::string stripWgemExt(std::string base) {
    stripExt(base, ".wgem");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeGem& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeGemLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wgem\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeGem& c,
                     const std::string& base) {
    std::printf("Wrote %s.wgem\n", base.c_str());
    std::printf("  catalog      : %s\n", c.name.c_str());
    std::printf("  gems         : %zu\n", c.gems.size());
    std::printf("  enchantments : %zu\n", c.enchantments.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterGems";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgemExt(base);
    auto c = wowee::pipeline::WoweeGemLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-gems")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenGemSet(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FullGemSet";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgemExt(base);
    auto c = wowee::pipeline::WoweeGemLoader::makeGemSet(name);
    if (!saveOrError(c, base, "gen-gems-set")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenEnchants(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "EnchantSet";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgemExt(base);
    auto c = wowee::pipeline::WoweeGemLoader::makeEnchants(name);
    if (!saveOrError(c, base, "gen-gems-enchants")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgemExt(base);
    if (!wowee::pipeline::WoweeGemLoader::exists(base)) {
        std::fprintf(stderr, "WGEM not found: %s.wgem\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGemLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wgem"] = base + ".wgem";
        j["name"] = c.name;
        j["gemCount"] = c.gems.size();
        j["enchantCount"] = c.enchantments.size();
        nlohmann::json ga = nlohmann::json::array();
        for (const auto& g : c.gems) {
            ga.push_back({
                {"gemId", g.gemId},
                {"itemIdToInsert", g.itemIdToInsert},
                {"name", g.name},
                {"color", g.color},
                {"colorName", wowee::pipeline::WoweeGem::colorName(g.color)},
                {"statType", g.statType},
                {"statValue", g.statValue},
                {"requiredItemQuality", g.requiredItemQuality},
                {"spellId", g.spellId},
            });
        }
        j["gems"] = ga;
        nlohmann::json ea = nlohmann::json::array();
        for (const auto& e : c.enchantments) {
            ea.push_back({
                {"enchantId", e.enchantId},
                {"name", e.name},
                {"description", e.description},
                {"iconPath", e.iconPath},
                {"enchantSlot", e.enchantSlot},
                {"enchantSlotName", wowee::pipeline::WoweeGem::enchantSlotName(e.enchantSlot)},
                {"statType", e.statType},
                {"statValue", e.statValue},
                {"spellId", e.spellId},
                {"durationSeconds", e.durationSeconds},
                {"chargeCount", e.chargeCount},
            });
        }
        j["enchantments"] = ea;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WGEM: %s.wgem\n", base.c_str());
    std::printf("  catalog      : %s\n", c.name.c_str());
    std::printf("  gems         : %zu\n", c.gems.size());
    std::printf("  enchantments : %zu\n", c.enchantments.size());
    if (!c.gems.empty()) {
        std::printf("\n  Gems:\n");
        std::printf("    id   color       stat/value   itemId   name\n");
        for (const auto& g : c.gems) {
            std::printf("  %4u   %-10s  %3u/%-5d   %5u    %s\n",
                        g.gemId,
                        wowee::pipeline::WoweeGem::colorName(g.color),
                        g.statType, g.statValue,
                        g.itemIdToInsert, g.name.c_str());
        }
    }
    if (!c.enchantments.empty()) {
        std::printf("\n  Enchantments:\n");
        std::printf("    id   slot         stat/value   dur(s)   chg   name\n");
        for (const auto& e : c.enchantments) {
            std::printf("  %4u   %-10s   %3u/%-5d   %5u    %3u   %s\n",
                        e.enchantId,
                        wowee::pipeline::WoweeGem::enchantSlotName(e.enchantSlot),
                        e.statType, e.statValue,
                        e.durationSeconds, e.chargeCount,
                        e.name.c_str());
        }
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgemExt(base);
    if (!wowee::pipeline::WoweeGemLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wgem: WGEM not found: %s.wgem\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGemLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.gems.empty() && c.enchantments.empty()) {
        warnings.push_back("catalog has zero gems and zero enchantments");
    }
    std::vector<uint32_t> gemIdsSeen;
    for (size_t k = 0; k < c.gems.size(); ++k) {
        const auto& g = c.gems[k];
        std::string ctx = "gem " + std::to_string(k) +
                          " (id=" + std::to_string(g.gemId);
        if (!g.name.empty()) ctx += " " + g.name;
        ctx += ")";
        if (g.gemId == 0) errors.push_back(ctx + ": gemId is 0");
        if (g.name.empty()) errors.push_back(ctx + ": name is empty");
        if (g.color > wowee::pipeline::WoweeGem::Prismatic) {
            errors.push_back(ctx + ": color " +
                std::to_string(g.color) + " not in 0..7");
        }
        // Stat-only gems (spellId=0) need statValue != 0.
        if (g.spellId == 0 && g.statValue == 0) {
            warnings.push_back(ctx +
                ": no spell + statValue=0 (gem provides nothing)");
        }
        for (uint32_t prev : gemIdsSeen) {
            if (prev == g.gemId) {
                errors.push_back(ctx + ": duplicate gemId");
                break;
            }
        }
        gemIdsSeen.push_back(g.gemId);
    }
    std::vector<uint32_t> enchIdsSeen;
    for (size_t k = 0; k < c.enchantments.size(); ++k) {
        const auto& e = c.enchantments[k];
        std::string ctx = "enchant " + std::to_string(k) +
                          " (id=" + std::to_string(e.enchantId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.enchantId == 0) errors.push_back(ctx + ": enchantId is 0");
        if (e.name.empty()) errors.push_back(ctx + ": name is empty");
        if (e.enchantSlot > wowee::pipeline::WoweeGem::Cloak) {
            errors.push_back(ctx + ": enchantSlot " +
                std::to_string(e.enchantSlot) + " not in 0..4");
        }
        if (e.spellId == 0 && e.statValue == 0) {
            warnings.push_back(ctx +
                ": no spell + statValue=0 (enchant provides nothing)");
        }
        // Charges only meaningful for Temporary enchants.
        if (e.chargeCount > 0 &&
            e.enchantSlot != wowee::pipeline::WoweeGem::Temporary) {
            warnings.push_back(ctx +
                ": chargeCount > 0 on non-Temporary slot (charges ignored)");
        }
        for (uint32_t prev : enchIdsSeen) {
            if (prev == e.enchantId) {
                errors.push_back(ctx + ": duplicate enchantId");
                break;
            }
        }
        enchIdsSeen.push_back(e.enchantId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wgem"] = base + ".wgem";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wgem: %s.wgem\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu gems, %zu enchantments, all IDs unique\n",
                    c.gems.size(), c.enchantments.size());
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

bool handleGemsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-gems") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-gems-set") == 0 && i + 1 < argc) {
        outRc = handleGenGemSet(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-gems-enchants") == 0 && i + 1 < argc) {
        outRc = handleGenEnchants(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wgem") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wgem") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
