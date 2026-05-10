#include "cli_item_materials_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_item_materials.hpp"
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

std::string stripWmatExt(std::string base) {
    stripExt(base, ".wmat");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeItemMaterial& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeItemMaterialLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wmat\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeItemMaterial& c,
                     const std::string& base) {
    std::printf("Wrote %s.wmat\n", base.c_str());
    std::printf("  catalog   : %s\n", c.name.c_str());
    std::printf("  materials : %zu\n", c.entries.size());
}

int handleGenArmor(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ArmorMaterials";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmatExt(base);
    auto c = wowee::pipeline::WoweeItemMaterialLoader::makeArmor(name);
    if (!saveOrError(c, base, "gen-mat")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenWeapon(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WeaponMaterials";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmatExt(base);
    auto c = wowee::pipeline::WoweeItemMaterialLoader::makeWeapon(name);
    if (!saveOrError(c, base, "gen-mat-weapon")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMagical(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MagicalMaterials";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWmatExt(base);
    auto c = wowee::pipeline::WoweeItemMaterialLoader::makeMagical(name);
    if (!saveOrError(c, base, "gen-mat-magical")) return 1;
    printGenSummary(c, base);
    return 0;
}

void appendMaterialFlagNames(uint32_t flags, std::string& out) {
    using F = wowee::pipeline::WoweeItemMaterial;
    auto add = [&](const char* n) {
        if (!out.empty()) out += "|";
        out += n;
    };
    if (flags & F::IsBreakable)    add("IsBreakable");
    if (flags & F::IsMagical)      add("IsMagical");
    if (flags & F::IsFlammable)    add("IsFlammable");
    if (flags & F::IsConductive)   add("IsConductive");
    if (flags & F::IsHolyCharged)  add("IsHolyCharged");
    if (flags & F::IsCursed)       add("IsCursed");
    if (out.empty()) out = "-";
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWmatExt(base);
    if (!wowee::pipeline::WoweeItemMaterialLoader::exists(base)) {
        std::fprintf(stderr, "WMAT not found: %s.wmat\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeItemMaterialLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wmat"] = base + ".wmat";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            std::string flagNames;
            appendMaterialFlagNames(e.materialFlags, flagNames);
            arr.push_back({
                {"materialId", e.materialId},
                {"name", e.name},
                {"description", e.description},
                {"materialKind", e.materialKind},
                {"materialKindName", wowee::pipeline::WoweeItemMaterial::materialKindName(e.materialKind)},
                {"weightCategory", e.weightCategory},
                {"weightCategoryName", wowee::pipeline::WoweeItemMaterial::weightCategoryName(e.weightCategory)},
                {"foleySoundId", e.foleySoundId},
                {"impactSoundId", e.impactSoundId},
                {"materialFlags", e.materialFlags},
                {"materialFlagsLabels", flagNames},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WMAT: %s.wmat\n", base.c_str());
    std::printf("  catalog   : %s\n", c.name.c_str());
    std::printf("  materials : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind        weight   foley  impact   flags                            name\n");
    for (const auto& e : c.entries) {
        std::string flagNames;
        appendMaterialFlagNames(e.materialFlags, flagNames);
        std::printf("  %4u    %-9s   %-6s   %5u   %5u   %-30s   %s\n",
                    e.materialId,
                    wowee::pipeline::WoweeItemMaterial::materialKindName(e.materialKind),
                    wowee::pipeline::WoweeItemMaterial::weightCategoryName(e.weightCategory),
                    e.foleySoundId, e.impactSoundId,
                    flagNames.c_str(),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWmatExt(base);
    if (!wowee::pipeline::WoweeItemMaterialLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wmat: WMAT not found: %s.wmat\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeItemMaterialLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    constexpr uint32_t kKnownFlagMask =
        wowee::pipeline::WoweeItemMaterial::IsBreakable |
        wowee::pipeline::WoweeItemMaterial::IsMagical |
        wowee::pipeline::WoweeItemMaterial::IsFlammable |
        wowee::pipeline::WoweeItemMaterial::IsConductive |
        wowee::pipeline::WoweeItemMaterial::IsHolyCharged |
        wowee::pipeline::WoweeItemMaterial::IsCursed;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.materialId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.materialId == 0)
            errors.push_back(ctx + ": materialId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.materialKind > wowee::pipeline::WoweeItemMaterial::Hide) {
            errors.push_back(ctx + ": materialKind " +
                std::to_string(e.materialKind) + " not in 0..11");
        }
        if (e.weightCategory > wowee::pipeline::WoweeItemMaterial::Heavy) {
            errors.push_back(ctx + ": weightCategory " +
                std::to_string(e.weightCategory) + " not in 0..2");
        }
        if (e.materialFlags & ~kKnownFlagMask) {
            warnings.push_back(ctx +
                ": materialFlags has bits outside known mask " +
                "(0x" + std::to_string(e.materialFlags & ~kKnownFlagMask) +
                ") — engine will ignore unknown flags");
        }
        // Contradictory flag combos.
        if ((e.materialFlags & wowee::pipeline::WoweeItemMaterial::IsHolyCharged) &&
            (e.materialFlags & wowee::pipeline::WoweeItemMaterial::IsCursed)) {
            warnings.push_back(ctx +
                ": both IsHolyCharged and IsCursed flags set — "
                "engine will pick one (typically IsCursed wins)");
        }
        // Plate kind is canonically heavy. Cloth is light.
        if (e.materialKind == wowee::pipeline::WoweeItemMaterial::Plate &&
            e.weightCategory != wowee::pipeline::WoweeItemMaterial::Heavy) {
            warnings.push_back(ctx +
                ": Plate kind with weightCategory=" +
                wowee::pipeline::WoweeItemMaterial::weightCategoryName(e.weightCategory) +
                " — plate armor is canonically heavy");
        }
        if (e.materialKind == wowee::pipeline::WoweeItemMaterial::Cloth &&
            e.weightCategory != wowee::pipeline::WoweeItemMaterial::Light) {
            warnings.push_back(ctx +
                ": Cloth kind with weightCategory=" +
                wowee::pipeline::WoweeItemMaterial::weightCategoryName(e.weightCategory) +
                " — cloth is canonically light");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.materialId) {
                errors.push_back(ctx + ": duplicate materialId");
                break;
            }
        }
        idsSeen.push_back(e.materialId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wmat"] = base + ".wmat";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wmat: %s.wmat\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu materials, all materialIds unique\n",
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

bool handleItemMaterialsCatalog(int& i, int argc, char** argv,
                                int& outRc) {
    if (std::strcmp(argv[i], "--gen-mat") == 0 && i + 1 < argc) {
        outRc = handleGenArmor(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mat-weapon") == 0 && i + 1 < argc) {
        outRc = handleGenWeapon(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mat-magical") == 0 && i + 1 < argc) {
        outRc = handleGenMagical(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wmat") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wmat") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
