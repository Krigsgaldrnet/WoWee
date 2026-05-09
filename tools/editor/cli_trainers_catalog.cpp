#include "cli_trainers_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_trainers.hpp"
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

std::string stripWtrnExt(std::string base) {
    stripExt(base, ".wtrn");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeTrainer& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeTrainerLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wtrn\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

uint32_t totalSpellOffers(const wowee::pipeline::WoweeTrainer& c) {
    uint32_t n = 0;
    for (const auto& e : c.entries) n += static_cast<uint32_t>(e.spells.size());
    return n;
}

uint32_t totalItemOffers(const wowee::pipeline::WoweeTrainer& c) {
    uint32_t n = 0;
    for (const auto& e : c.entries) n += static_cast<uint32_t>(e.items.size());
    return n;
}

void printGenSummary(const wowee::pipeline::WoweeTrainer& c,
                     const std::string& base) {
    std::printf("Wrote %s.wtrn\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  npcs    : %zu (%u spells offered, %u items offered)\n",
                c.entries.size(),
                totalSpellOffers(c),
                totalItemOffers(c));
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterTrainers";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtrnExt(base);
    auto c = wowee::pipeline::WoweeTrainerLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-trainers")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMage(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MageTrainer";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtrnExt(base);
    auto c = wowee::pipeline::WoweeTrainerLoader::makeMageTrainer(name);
    if (!saveOrError(c, base, "gen-trainers-mage")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenWeaponVendor(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WeaponVendor";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtrnExt(base);
    auto c = wowee::pipeline::WoweeTrainerLoader::makeWeaponVendor(name);
    if (!saveOrError(c, base, "gen-trainers-weapons")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWtrnExt(base);
    if (!wowee::pipeline::WoweeTrainerLoader::exists(base)) {
        std::fprintf(stderr, "WTRN not found: %s.wtrn\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeTrainerLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wtrn"] = base + ".wtrn";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            nlohmann::json je;
            je["npcId"] = e.npcId;
            je["kindMask"] = e.kindMask;
            je["kindMaskName"] = wowee::pipeline::WoweeTrainer::kindMaskName(e.kindMask);
            je["greeting"] = e.greeting;
            nlohmann::json sa = nlohmann::json::array();
            for (const auto& s : e.spells) {
                sa.push_back({
                    {"spellId", s.spellId},
                    {"moneyCostCopper", s.moneyCostCopper},
                    {"requiredSkillId", s.requiredSkillId},
                    {"requiredSkillRank", s.requiredSkillRank},
                    {"requiredLevel", s.requiredLevel},
                });
            }
            je["spells"] = sa;
            nlohmann::json ia = nlohmann::json::array();
            for (const auto& it : e.items) {
                ia.push_back({
                    {"itemId", it.itemId},
                    {"stockCount", it.stockCount},
                    {"restockSec", it.restockSec},
                    {"extendedCost", it.extendedCost},
                    {"moneyCostCopper", it.moneyCostCopper},
                });
            }
            je["items"] = ia;
            arr.push_back(je);
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WTRN: %s.wtrn\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  npcs    : %zu (%u spells offered, %u items offered)\n",
                c.entries.size(),
                totalSpellOffers(c),
                totalItemOffers(c));
    if (c.entries.empty()) return 0;
    for (const auto& e : c.entries) {
        std::printf("\n  npcId=%u  kind=%s\n",
                    e.npcId,
                    wowee::pipeline::WoweeTrainer::kindMaskName(e.kindMask).c_str());
        if (!e.greeting.empty()) {
            std::printf("    greeting: \"%s\"\n", e.greeting.c_str());
        }
        if (!e.spells.empty()) {
            std::printf("    spells (%zu):\n", e.spells.size());
            std::printf("      spellId  cost     skill/rank  minLvl\n");
            for (const auto& s : e.spells) {
                std::printf("      %5u    %5uc   %4u/%-4u   %u\n",
                            s.spellId, s.moneyCostCopper,
                            s.requiredSkillId, s.requiredSkillRank,
                            s.requiredLevel);
            }
        }
        if (!e.items.empty()) {
            std::printf("    items (%zu):\n", e.items.size());
            std::printf("      itemId  stock      restock  override-cost\n");
            for (const auto& it : e.items) {
                std::string stockStr =
                    it.stockCount == wowee::pipeline::WoweeTrainer::kUnlimitedStock
                        ? std::string("unlimited") : std::to_string(it.stockCount);
                std::printf("      %5u   %-9s  %5us    %s%uc\n",
                            it.itemId, stockStr.c_str(),
                            it.restockSec,
                            it.moneyCostCopper == 0 ? "(WIT) " : "",
                            it.moneyCostCopper);
            }
        }
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWtrnExt(base);
    if (!wowee::pipeline::WoweeTrainerLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wtrn: WTRN not found: %s.wtrn\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeTrainerLoader::load(base);
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
                          " (npcId=" + std::to_string(e.npcId) + ")";
        if (e.npcId == 0) {
            errors.push_back(ctx + ": npcId is 0");
        }
        if (e.kindMask == 0) {
            errors.push_back(ctx + ": kindMask is 0 (NPC offers nothing)");
        }
        // Trainer kind needs spells; vendor kind needs items.
        if ((e.kindMask & wowee::pipeline::WoweeTrainer::Trainer) &&
            e.spells.empty()) {
            warnings.push_back(ctx +
                ": flagged Trainer but has no spells");
        }
        if ((e.kindMask & wowee::pipeline::WoweeTrainer::Vendor) &&
            e.items.empty()) {
            warnings.push_back(ctx +
                ": flagged Vendor but has no items");
        }
        // Items / spells with kindMask not matching are dead config.
        if (!(e.kindMask & wowee::pipeline::WoweeTrainer::Trainer) &&
            !e.spells.empty()) {
            warnings.push_back(ctx +
                ": has " + std::to_string(e.spells.size()) +
                " spells but Trainer bit not set (spells will be ignored)");
        }
        if (!(e.kindMask & wowee::pipeline::WoweeTrainer::Vendor) &&
            !e.items.empty()) {
            warnings.push_back(ctx +
                ": has " + std::to_string(e.items.size()) +
                " items but Vendor bit not set (items will be ignored)");
        }
        for (size_t si = 0; si < e.spells.size(); ++si) {
            const auto& s = e.spells[si];
            std::string sctx = ctx + " spell " + std::to_string(si);
            if (s.spellId == 0) {
                errors.push_back(sctx + ": spellId is 0");
            }
        }
        for (size_t ii = 0; ii < e.items.size(); ++ii) {
            const auto& it = e.items[ii];
            std::string ictx = ctx + " item " + std::to_string(ii);
            if (it.itemId == 0) {
                errors.push_back(ictx + ": itemId is 0");
            }
            // Finite stock with restockSec=0 means "single fill"
            // — usually intentional but worth surfacing.
            if (it.stockCount != wowee::pipeline::WoweeTrainer::kUnlimitedStock &&
                it.restockSec == 0 && it.stockCount > 0) {
                warnings.push_back(ictx +
                    ": finite stock with restockSec=0 (no automatic refresh)");
            }
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.npcId) {
                errors.push_back(ctx + ": duplicate npcId");
                break;
            }
        }
        idsSeen.push_back(e.npcId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wtrn"] = base + ".wtrn";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wtrn: %s.wtrn\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu npcs, %u spell offers, %u item offers\n",
                    c.entries.size(),
                    totalSpellOffers(c),
                    totalItemOffers(c));
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

bool handleTrainersCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-trainers") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-trainers-mage") == 0 && i + 1 < argc) {
        outRc = handleGenMage(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-trainers-weapons") == 0 && i + 1 < argc) {
        outRc = handleGenWeaponVendor(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wtrn") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wtrn") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
