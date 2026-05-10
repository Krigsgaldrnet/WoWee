#include "cli_soulbind_rules_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_soulbind_rules.hpp"
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

std::string stripWbndExt(std::string base) {
    stripExt(base, ".wbnd");
    return base;
}

const char* bindKindName(uint8_t k) {
    using B = wowee::pipeline::WoweeSoulbindRules;
    switch (k) {
        case B::BindOnPickup:  return "bindonpickup";
        case B::BindOnEquip:   return "bindonequip";
        case B::BindOnUse:     return "bindonuse";
        case B::BindOnAccount: return "bindonaccount";
        case B::Soulbound:     return "soulbound";
        case B::NoBind:        return "nobind";
        default:               return "?";
    }
}

const char* qualityName(uint8_t q) {
    using B = wowee::pipeline::WoweeSoulbindRules;
    switch (q) {
        case B::Poor:      return "poor";
        case B::Common:    return "common";
        case B::Uncommon:  return "uncommon";
        case B::Rare:      return "rare";
        case B::Epic:      return "epic";
        case B::Legendary: return "legendary";
        case B::Artifact:  return "artifact";
        case B::Heirloom:  return "heirloom";
        default:           return "?";
    }
}

bool saveOrError(const wowee::pipeline::WoweeSoulbindRules& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSoulbindRulesLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wbnd\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSoulbindRules& c,
                     const std::string& base) {
    std::printf("Wrote %s.wbnd\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  rules   : %zu\n", c.entries.size());
}

int handleGenVanilla(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "VanillaSoulbindPolicy";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbndExt(base);
    auto c = wowee::pipeline::WoweeSoulbindRulesLoader::
        makeVanillaPolicy(name);
    if (!saveOrError(c, base, "gen-bnd-vanilla")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenTBC(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "TBCSoulbindPolicy";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbndExt(base);
    auto c = wowee::pipeline::WoweeSoulbindRulesLoader::
        makeTBCPolicy(name);
    if (!saveOrError(c, base, "gen-bnd-tbc")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenWotLK(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WotLKSoulbindPolicy";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbndExt(base);
    auto c = wowee::pipeline::WoweeSoulbindRulesLoader::
        makeWotLKPolicy(name);
    if (!saveOrError(c, base, "gen-bnd-wotlk")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWbndExt(base);
    if (!wowee::pipeline::WoweeSoulbindRulesLoader::exists(base)) {
        std::fprintf(stderr, "WBND not found: %s.wbnd\n",
                     base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSoulbindRulesLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wbnd"] = base + ".wbnd";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"ruleId", e.ruleId},
                {"name", e.name},
                {"bindKind", e.bindKind},
                {"bindKindName", bindKindName(e.bindKind)},
                {"itemQualityFloor", e.itemQualityFloor},
                {"itemQualityFloorName",
                    qualityName(e.itemQualityFloor)},
                {"tradableForRaidGroup",
                    e.tradableForRaidGroup != 0},
                {"boeBecomesBoP", e.boeBecomesBoP != 0},
                {"accountBoundCrossFaction",
                    e.accountBoundCrossFaction != 0},
                {"tradableWindowSec", e.tradableWindowSec},
                {"description", e.description},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WBND: %s.wbnd\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  rules   : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id  bind-kind        quality    raid  boe2bop  xfac  window  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u  %-13s    %-9s    %s    %s      %s    %5us  %s\n",
                    e.ruleId,
                    bindKindName(e.bindKind),
                    qualityName(e.itemQualityFloor),
                    e.tradableForRaidGroup ? "Y" : "n",
                    e.boeBecomesBoP ? "Y" : "n",
                    e.accountBoundCrossFaction ? "Y" : "n",
                    e.tradableWindowSec,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWbndExt(base);
    if (!wowee::pipeline::WoweeSoulbindRulesLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wbnd: WBND not found: %s.wbnd\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSoulbindRulesLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    using B = wowee::pipeline::WoweeSoulbindRules;
    std::set<uint32_t> idsSeen;
    using Pair = std::pair<uint8_t, uint8_t>;
    std::set<Pair> kindFloorPairs;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.ruleId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.ruleId == 0)
            errors.push_back(ctx + ": ruleId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.bindKind > 5) {
            errors.push_back(ctx + ": bindKind " +
                std::to_string(e.bindKind) +
                " out of range (0..5)");
        }
        if (e.itemQualityFloor > 7) {
            errors.push_back(ctx + ": itemQualityFloor " +
                std::to_string(e.itemQualityFloor) +
                " out of range (0..7)");
        }
        // tradableForRaidGroup only meaningful for
        // BindOnPickup. NoBind/BoE/BoU/BoA/Soulbound
        // all ignore the raid-window flag.
        if (e.tradableForRaidGroup &&
            e.bindKind != B::BindOnPickup) {
            warnings.push_back(ctx +
                ": tradableForRaidGroup=true but "
                "bindKind is not BindOnPickup — flag "
                "would be ignored at runtime");
        }
        // tradableWindowSec only meaningful when raid-
        // trade is enabled.
        if (e.tradableWindowSec > 0 &&
            !e.tradableForRaidGroup) {
            warnings.push_back(ctx +
                ": tradableWindowSec=" +
                std::to_string(e.tradableWindowSec) +
                " set but tradableForRaidGroup=false "
                "— window would never be reachable");
        }
        // tradableForRaidGroup=true with window=0 is
        // a contradiction (instant window expiry =
        // no window at all).
        if (e.tradableForRaidGroup &&
            e.tradableWindowSec == 0) {
            errors.push_back(ctx +
                ": tradableForRaidGroup=true with "
                "tradableWindowSec=0 — window expires "
                "instantly, equivalent to no window");
        }
        // boeBecomesBoP only meaningful for BindOnEquip
        // (it's the trigger that converts BoE to
        // Soulbound on the pickup acknowledgement).
        if (e.boeBecomesBoP && e.bindKind != B::BindOnEquip) {
            warnings.push_back(ctx +
                ": boeBecomesBoP=true but bindKind is "
                "not BindOnEquip — flag would never "
                "fire");
        }
        // accountBoundCrossFaction only meaningful
        // for BindOnAccount kind.
        if (e.accountBoundCrossFaction &&
            e.bindKind != B::BindOnAccount) {
            warnings.push_back(ctx +
                ": accountBoundCrossFaction=true but "
                "bindKind is not BindOnAccount — flag "
                "would never apply");
        }
        // (bindKind, itemQualityFloor) MUST be unique
        // — runtime resolveForQuality() would
        // ambiguously pick one rule when two rules
        // tie on (bindKind, floor).
        Pair p{e.bindKind, e.itemQualityFloor};
        if (!kindFloorPairs.insert(p).second) {
            errors.push_back(ctx +
                ": duplicate (bindKind=" +
                std::to_string(e.bindKind) +
                ", itemQualityFloor=" +
                std::to_string(e.itemQualityFloor) +
                ") — resolveForQuality() tie");
        }
        if (!idsSeen.insert(e.ruleId).second) {
            errors.push_back(ctx + ": duplicate ruleId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wbnd"] = base + ".wbnd";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wbnd: %s.wbnd\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu rules, all ruleIds + "
                    "(bindKind,itemQualityFloor) unique, "
                    "bindKind 0..5, quality 0..7, no "
                    "tradableForRaidGroup-with-window=0\n",
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

bool handleSoulbindRulesCatalog(int& i, int argc, char** argv,
                                  int& outRc) {
    if (std::strcmp(argv[i], "--gen-bnd-vanilla") == 0 &&
        i + 1 < argc) {
        outRc = handleGenVanilla(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-bnd-tbc") == 0 &&
        i + 1 < argc) {
        outRc = handleGenTBC(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-bnd-wotlk") == 0 &&
        i + 1 < argc) {
        outRc = handleGenWotLK(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wbnd") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wbnd") == 0 &&
        i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
