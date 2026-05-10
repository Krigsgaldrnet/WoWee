#include "cli_pet_care_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_pet_care.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWpcrExt(std::string base) {
    stripExt(base, ".wpcr");
    return base;
}

const char* actionKindName(uint8_t k) {
    using P = wowee::pipeline::WoweePetCare;
    switch (k) {
        case P::Revive:    return "revive";
        case P::Mend:      return "mend";
        case P::Feed:      return "feed";
        case P::Dismiss:   return "dismiss";
        case P::Tame:      return "tame";
        case P::BeastLore: return "beastlore";
        case P::Stable:    return "stable";
        case P::Untrain:   return "untrain";
        case P::Rename:    return "rename";
        case P::Abandon:   return "abandon";
        case P::Summon:    return "summon";
        default:           return "unknown";
    }
}

bool saveOrError(const wowee::pipeline::WoweePetCare& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweePetCareLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wpcr\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweePetCare& c,
                     const std::string& base) {
    std::printf("Wrote %s.wpcr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  actions : %zu\n", c.entries.size());
}

int handleGenHunter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "HunterPetCare";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpcrExt(base);
    auto c = wowee::pipeline::WoweePetCareLoader::makeHunterCare(name);
    if (!saveOrError(c, base, "gen-pcr")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenStable(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StableMasterActions";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpcrExt(base);
    auto c = wowee::pipeline::WoweePetCareLoader::makeStableActions(name);
    if (!saveOrError(c, base, "gen-pcr-stable")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenWarlock(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WarlockMinionSummons";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWpcrExt(base);
    auto c = wowee::pipeline::WoweePetCareLoader::makeWarlockMinions(name);
    if (!saveOrError(c, base, "gen-pcr-warlock")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWpcrExt(base);
    if (!wowee::pipeline::WoweePetCareLoader::exists(base)) {
        std::fprintf(stderr, "WPCR not found: %s.wpcr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweePetCareLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wpcr"] = base + ".wpcr";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"actionId", e.actionId},
                {"name", e.name},
                {"description", e.description},
                {"spellId", e.spellId},
                {"classFilter", e.classFilter},
                {"actionKind", e.actionKind},
                {"actionKindName", actionKindName(e.actionKind)},
                {"happinessRestore", e.happinessRestore},
                {"requiresPet", e.requiresPet != 0},
                {"requiresStableNPC", e.requiresStableNPC != 0},
                {"costCopper", e.costCopper},
                {"reagentItemId", e.reagentItemId},
                {"castTimeMs", e.castTimeMs},
                {"cooldownSec", e.cooldownSec},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WPCR: %s.wpcr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  actions : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   spell   class   kind        happy  pet  stb  cost(c)  reagent  cast(ms)  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %5u   %5u   %-9s    %+3d   %s   %s    %6u    %5u    %5u   %s\n",
                    e.actionId, e.spellId, e.classFilter,
                    actionKindName(e.actionKind),
                    e.happinessRestore,
                    e.requiresPet ? "Y" : "n",
                    e.requiresStableNPC ? "Y" : "n",
                    e.costCopper, e.reagentItemId,
                    e.castTimeMs, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWpcrExt(base);
    if (!wowee::pipeline::WoweePetCareLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wpcr: WPCR not found: %s.wpcr\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweePetCareLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.actionId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.actionId == 0)
            errors.push_back(ctx + ": actionId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.classFilter == 0) {
            errors.push_back(ctx +
                ": classFilter is 0 — no class can use "
                "this action");
        }
        if (e.actionKind > 10) {
            errors.push_back(ctx + ": actionKind " +
                std::to_string(e.actionKind) +
                " out of range (must be 0..10)");
        }
        if (e.happinessRestore < -25 || e.happinessRestore > 25) {
            warnings.push_back(ctx +
                ": happinessRestore " +
                std::to_string(e.happinessRestore) +
                " outside +/-25 — pet happiness range "
                "is normally [-100, +100], single-action "
                "swing >25 is unusual");
        }
        // Per-kind validity rules:
        using P = wowee::pipeline::WoweePetCare;
        if (e.actionKind == P::Tame && e.requiresPet != 0) {
            errors.push_back(ctx +
                ": Tame action requires NO pet active "
                "(requiresPet must be 0) — you can't tame "
                "while another pet is out");
        }
        if (e.actionKind == P::Summon && e.requiresPet != 0) {
            errors.push_back(ctx +
                ": Summon action requires NO pet active "
                "(requiresPet must be 0) — Warlock can't "
                "summon while another minion is out");
        }
        if (e.actionKind == P::Revive && e.requiresPet != 0) {
            warnings.push_back(ctx +
                ": Revive action with requiresPet=1 — "
                "revive should target a DEAD pet, not "
                "require an active one. Verify intent.");
        }
        if (e.actionKind == P::Stable &&
            e.requiresStableNPC == 0) {
            warnings.push_back(ctx +
                ": Stable action with requiresStableNPC=0 "
                "— stable slot purchases are normally "
                "gated to stable-master conversation");
        }
        // Tame with cooldown — Tame Beast has a fixed
        // 15-second internal cooldown in 3.3.5; warn if
        // unset.
        if (e.actionKind == P::Tame && e.cooldownSec == 0) {
            warnings.push_back(ctx +
                ": Tame action with cooldownSec=0 — Tame "
                "Beast canonically has a 15-sec internal "
                "cooldown to prevent macro-spam");
        }
        if (!idsSeen.insert(e.actionId).second) {
            errors.push_back(ctx + ": duplicate actionId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wpcr"] = base + ".wpcr";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wpcr: %s.wpcr\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu actions, all actionIds "
                    "unique, per-kind constraints "
                    "satisfied\n", c.entries.size());
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

bool handlePetCareCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-pcr") == 0 && i + 1 < argc) {
        outRc = handleGenHunter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-pcr-stable") == 0 && i + 1 < argc) {
        outRc = handleGenStable(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-pcr-warlock") == 0 && i + 1 < argc) {
        outRc = handleGenWarlock(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wpcr") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wpcr") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
