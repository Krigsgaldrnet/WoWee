#include "cli_creature_families_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_creature_families.hpp"
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

std::string stripWcefExt(std::string base) {
    stripExt(base, ".wcef");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeCreatureFamily& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeCreatureFamilyLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wcef\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeCreatureFamily& c,
                     const std::string& base) {
    std::printf("Wrote %s.wcef\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  families : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterFamilies";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcefExt(base);
    auto c = wowee::pipeline::WoweeCreatureFamilyLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-cef")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenFerocity(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FerocityPets";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcefExt(base);
    auto c = wowee::pipeline::WoweeCreatureFamilyLoader::makeFerocity(name);
    if (!saveOrError(c, base, "gen-cef-ferocity")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenExotic(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ExoticBeastMaster";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcefExt(base);
    auto c = wowee::pipeline::WoweeCreatureFamilyLoader::makeExotic(name);
    if (!saveOrError(c, base, "gen-cef-exotic")) return 1;
    printGenSummary(c, base);
    return 0;
}

void appendFoodNames(uint32_t flags, std::string& out) {
    using F = wowee::pipeline::WoweeCreatureFamily;
    auto add = [&](const char* n) {
        if (!out.empty()) out += "|";
        out += n;
    };
    if (flags & F::Meat)   add("Meat");
    if (flags & F::Fish)   add("Fish");
    if (flags & F::Bread)  add("Bread");
    if (flags & F::Cheese) add("Cheese");
    if (flags & F::Fruit)  add("Fruit");
    if (flags & F::Fungus) add("Fungus");
    if (flags & F::Raw)    add("Raw");
    if (out.empty()) out = "-";
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcefExt(base);
    if (!wowee::pipeline::WoweeCreatureFamilyLoader::exists(base)) {
        std::fprintf(stderr, "WCEF not found: %s.wcef\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCreatureFamilyLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wcef"] = base + ".wcef";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            std::string foodNames;
            appendFoodNames(e.petFoodTypes, foodNames);
            arr.push_back({
                {"familyId", e.familyId},
                {"name", e.name},
                {"description", e.description},
                {"familyKind", e.familyKind},
                {"familyKindName", wowee::pipeline::WoweeCreatureFamily::familyKindName(e.familyKind)},
                {"petTalentTree", e.petTalentTree},
                {"petTalentTreeName", wowee::pipeline::WoweeCreatureFamily::petTalentTreeName(e.petTalentTree)},
                {"minLevelForTame", e.minLevelForTame},
                {"skillLine", e.skillLine},
                {"petFoodTypes", e.petFoodTypes},
                {"petFoodTypesLabels", foodNames},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WCEF: %s.wcef\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  families : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind       tree       tameLvl  skill   foods                          name\n");
    for (const auto& e : c.entries) {
        std::string foodNames;
        appendFoodNames(e.petFoodTypes, foodNames);
        std::printf("  %4u    %-9s  %-9s  %5u    %5u   %-30s %s\n",
                    e.familyId,
                    wowee::pipeline::WoweeCreatureFamily::familyKindName(e.familyKind),
                    wowee::pipeline::WoweeCreatureFamily::petTalentTreeName(e.petTalentTree),
                    e.minLevelForTame,
                    e.skillLine,
                    foodNames.c_str(),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcefExt(base);
    if (!wowee::pipeline::WoweeCreatureFamilyLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wcef: WCEF not found: %s.wcef\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCreatureFamilyLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    constexpr uint32_t kKnownFoodMask =
        wowee::pipeline::WoweeCreatureFamily::Meat |
        wowee::pipeline::WoweeCreatureFamily::Fish |
        wowee::pipeline::WoweeCreatureFamily::Bread |
        wowee::pipeline::WoweeCreatureFamily::Cheese |
        wowee::pipeline::WoweeCreatureFamily::Fruit |
        wowee::pipeline::WoweeCreatureFamily::Fungus |
        wowee::pipeline::WoweeCreatureFamily::Raw;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.familyId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.familyId == 0)
            errors.push_back(ctx + ": familyId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.familyKind > wowee::pipeline::WoweeCreatureFamily::Exotic) {
            errors.push_back(ctx + ": familyKind " +
                std::to_string(e.familyKind) + " not in 0..5");
        }
        if (e.petTalentTree > wowee::pipeline::WoweeCreatureFamily::Cunning) {
            errors.push_back(ctx + ": petTalentTree " +
                std::to_string(e.petTalentTree) + " not in 0..3");
        }
        if (e.petFoodTypes & ~kKnownFoodMask) {
            warnings.push_back(ctx +
                ": petFoodTypes has bits outside known mask " +
                "(0x" + std::to_string(e.petFoodTypes & ~kKnownFoodMask) +
                ") — engine will ignore unknown food types");
        }
        // NotPet families should not specify a talent tree —
        // confusing if they do.
        if (e.familyKind == wowee::pipeline::WoweeCreatureFamily::NotPet &&
            e.petTalentTree != wowee::pipeline::WoweeCreatureFamily::TreeNone) {
            warnings.push_back(ctx +
                ": NotPet family with petTalentTree=" +
                wowee::pipeline::WoweeCreatureFamily::petTalentTreeName(e.petTalentTree) +
                " — talent tree is irrelevant for non-pet kinds");
        }
        // Exotic families above level 80 won't be tamable
        // by anyone (level cap).
        if (e.familyKind == wowee::pipeline::WoweeCreatureFamily::Exotic &&
            e.minLevelForTame > 80) {
            warnings.push_back(ctx +
                ": Exotic family with minLevelForTame=" +
                std::to_string(e.minLevelForTame) +
                " > 80 — no hunter can reach this level");
        }
        // Pet kinds with no food types set means they can't
        // be fed — common bug, especially for hand-edited
        // sidecars.
        if ((e.familyKind == wowee::pipeline::WoweeCreatureFamily::Beast ||
             e.familyKind == wowee::pipeline::WoweeCreatureFamily::Exotic) &&
            e.petFoodTypes == 0) {
            warnings.push_back(ctx +
                ": pet-able family with no food types set — "
                "hunter pet will starve, no food will satisfy it");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.familyId) {
                errors.push_back(ctx + ": duplicate familyId");
                break;
            }
        }
        idsSeen.push_back(e.familyId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wcef"] = base + ".wcef";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wcef: %s.wcef\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu families, all familyIds unique\n",
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

bool handleCreatureFamiliesCatalog(int& i, int argc, char** argv,
                                   int& outRc) {
    if (std::strcmp(argv[i], "--gen-cef") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cef-ferocity") == 0 && i + 1 < argc) {
        outRc = handleGenFerocity(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cef-exotic") == 0 && i + 1 < argc) {
        outRc = handleGenExotic(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wcef") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wcef") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
