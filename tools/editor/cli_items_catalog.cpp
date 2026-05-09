#include "cli_items_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_items.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWitExt(std::string base) {
    stripExt(base, ".wit");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeItem& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeItemLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wit\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeItem& c,
                     const std::string& base) {
    std::printf("Wrote %s.wit\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  entries : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterItems";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWitExt(base);
    auto c = wowee::pipeline::WoweeItemLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-items")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenWeapons(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WeaponCatalog";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWitExt(base);
    auto c = wowee::pipeline::WoweeItemLoader::makeWeapons(name);
    if (!saveOrError(c, base, "gen-items-weapons")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenArmor(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ArmorCatalog";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWitExt(base);
    auto c = wowee::pipeline::WoweeItemLoader::makeArmor(name);
    if (!saveOrError(c, base, "gen-items-armor")) return 1;
    printGenSummary(c, base);
    return 0;
}

void printPriceCopper(uint32_t copper) {
    uint32_t gold = copper / 10000;
    uint32_t silver = (copper / 100) % 100;
    uint32_t cop = copper % 100;
    std::printf("%ug %us %uc", gold, silver, cop);
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWitExt(base);
    if (!wowee::pipeline::WoweeItemLoader::exists(base)) {
        std::fprintf(stderr, "WIT not found: %s.wit\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeItemLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wit"] = base + ".wit";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            nlohmann::json je;
            je["itemId"] = e.itemId;
            je["displayId"] = e.displayId;
            je["quality"] = e.quality;
            je["qualityName"] = wowee::pipeline::WoweeItem::qualityName(e.quality);
            je["itemClass"] = e.itemClass;
            je["itemClassName"] = wowee::pipeline::WoweeItem::classNameOf(e.itemClass);
            je["itemSubClass"] = e.itemSubClass;
            je["inventoryType"] = e.inventoryType;
            je["slotName"] = wowee::pipeline::WoweeItem::slotName(e.inventoryType);
            je["flags"] = e.flags;
            je["requiredLevel"] = e.requiredLevel;
            je["itemLevel"] = e.itemLevel;
            je["sellPriceCopper"] = e.sellPriceCopper;
            je["buyPriceCopper"] = e.buyPriceCopper;
            je["maxStack"] = e.maxStack;
            je["durability"] = e.durability;
            je["damageMin"] = e.damageMin;
            je["damageMax"] = e.damageMax;
            je["attackSpeedMs"] = e.attackSpeedMs;
            nlohmann::json sa = nlohmann::json::array();
            for (const auto& s : e.stats) {
                sa.push_back({
                    {"type", s.type},
                    {"typeName", wowee::pipeline::WoweeItem::statName(s.type)},
                    {"value", s.value}
                });
            }
            je["stats"] = sa;
            je["name"] = e.name;
            je["description"] = e.description;
            arr.push_back(je);
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WIT: %s.wit\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  entries : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   ilvl  quality   class       slot       buy           name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u  %4u  %-9s %-11s %-10s ",
                    e.itemId, e.itemLevel,
                    wowee::pipeline::WoweeItem::qualityName(e.quality),
                    wowee::pipeline::WoweeItem::classNameOf(e.itemClass),
                    wowee::pipeline::WoweeItem::slotName(e.inventoryType));
        printPriceCopper(e.buyPriceCopper);
        std::printf("    %s\n", e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWitExt(base);
    if (!wowee::pipeline::WoweeItemLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wit: WIT not found: %s.wit\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeItemLoader::load(base);
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
                          " (id=" + std::to_string(e.itemId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.itemId == 0) {
            errors.push_back(ctx + ": itemId is 0");
        }
        if (e.quality > wowee::pipeline::WoweeItem::Heirloom) {
            errors.push_back(ctx + ": quality " +
                std::to_string(e.quality) + " not in 0..7");
        }
        // Weapon class implies damage fields > 0 and a 1H/2H slot.
        if (e.itemClass == wowee::pipeline::WoweeItem::Weapon) {
            if (e.damageMin == 0 || e.damageMax == 0) {
                errors.push_back(ctx + ": weapon has zero damage");
            }
            if (e.damageMin > e.damageMax) {
                errors.push_back(ctx + ": damageMin > damageMax");
            }
            if (e.attackSpeedMs == 0) {
                errors.push_back(ctx + ": weapon has zero attackSpeedMs");
            }
            if (e.inventoryType != wowee::pipeline::WoweeItem::Weapon1H &&
                e.inventoryType != wowee::pipeline::WoweeItem::Weapon2H &&
                e.inventoryType != wowee::pipeline::WoweeItem::Ranged) {
                warnings.push_back(ctx + ": weapon has non-weapon inventoryType");
            }
        }
        // Equippable items should have non-zero durability (catches
        // common armor authoring oversight).
        if (e.inventoryType != wowee::pipeline::WoweeItem::NonEquip &&
            e.durability == 0) {
            warnings.push_back(ctx +
                ": equippable item with durability=0");
        }
        // Stack-of-one items shouldn't have maxStack > 1
        // (unique-equip case is already guarded by the Unique flag).
        if (e.inventoryType != wowee::pipeline::WoweeItem::NonEquip &&
            e.maxStack > 1) {
            warnings.push_back(ctx +
                ": equippable item with maxStack > 1");
        }
        // Buy price should be greater than sell price (vendor margin).
        if (e.buyPriceCopper > 0 && e.sellPriceCopper > 0 &&
            e.sellPriceCopper >= e.buyPriceCopper) {
            warnings.push_back(ctx +
                ": sellPrice >= buyPrice (vendor would lose money)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.itemId) {
                errors.push_back(ctx + ": duplicate itemId");
                break;
            }
        }
        idsSeen.push_back(e.itemId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wit"] = base + ".wit";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wit: %s.wit\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu items, all itemIds unique\n",
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

bool handleItemsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-items") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-items-weapons") == 0 && i + 1 < argc) {
        outRc = handleGenWeapons(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-items-armor") == 0 && i + 1 < argc) {
        outRc = handleGenArmor(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wit") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wit") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
