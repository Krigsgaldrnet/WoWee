#include "cli_chars_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_chars.hpp"
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

std::string stripWchcExt(std::string base) {
    stripExt(base, ".wchc");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeChars& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeCharsLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wchc\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeChars& c,
                     const std::string& base) {
    std::printf("Wrote %s.wchc\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  classes : %zu  races : %zu  outfits : %zu\n",
                c.classes.size(), c.races.size(), c.outfits.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterChars";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWchcExt(base);
    auto c = wowee::pipeline::WoweeCharsLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-chars")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenAlliance(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AllianceChars";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWchcExt(base);
    auto c = wowee::pipeline::WoweeCharsLoader::makeAlliance(name);
    if (!saveOrError(c, base, "gen-chars-alliance")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenAllRaces(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AllRacesChars";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWchcExt(base);
    auto c = wowee::pipeline::WoweeCharsLoader::makeAllRaces(name);
    if (!saveOrError(c, base, "gen-chars-allraces")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWchcExt(base);
    if (!wowee::pipeline::WoweeCharsLoader::exists(base)) {
        std::fprintf(stderr, "WCHC not found: %s.wchc\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCharsLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wchc"] = base + ".wchc";
        j["name"] = c.name;
        j["classCount"] = c.classes.size();
        j["raceCount"] = c.races.size();
        j["outfitCount"] = c.outfits.size();
        nlohmann::json ca = nlohmann::json::array();
        for (const auto& cls : c.classes) {
            ca.push_back({
                {"classId", cls.classId},
                {"name", cls.name},
                {"iconPath", cls.iconPath},
                {"powerType", cls.powerType},
                {"powerTypeName", wowee::pipeline::WoweeChars::powerTypeName(cls.powerType)},
                {"displayPower", cls.displayPower},
                {"baseHealth", cls.baseHealth},
                {"baseHealthPerLevel", cls.baseHealthPerLevel},
                {"basePower", cls.basePower},
                {"basePowerPerLevel", cls.basePowerPerLevel},
                {"factionAvailability", cls.factionAvailability},
            });
        }
        j["classes"] = ca;
        nlohmann::json ra = nlohmann::json::array();
        for (const auto& r : c.races) {
            ra.push_back({
                {"raceId", r.raceId},
                {"name", r.name},
                {"iconPath", r.iconPath},
                {"factionId", r.factionId},
                {"factionName", wowee::pipeline::WoweeChars::raceFactionName(r.factionId)},
                {"maleDisplayId", r.maleDisplayId},
                {"femaleDisplayId", r.femaleDisplayId},
                {"baseStrength", r.baseStrength},
                {"baseAgility", r.baseAgility},
                {"baseStamina", r.baseStamina},
                {"baseIntellect", r.baseIntellect},
                {"baseSpirit", r.baseSpirit},
                {"startingMapId", r.startingMapId},
                {"startingZoneAreaId", r.startingZoneAreaId},
                {"defaultLanguageSpellId", r.defaultLanguageSpellId},
                {"mountSpellId", r.mountSpellId},
            });
        }
        j["races"] = ra;
        nlohmann::json oa = nlohmann::json::array();
        for (const auto& o : c.outfits) {
            nlohmann::json items = nlohmann::json::array();
            for (const auto& it : o.items) {
                items.push_back({
                    {"itemId", it.itemId},
                    {"displaySlot", it.displaySlot},
                });
            }
            oa.push_back({
                {"classId", o.classId},
                {"raceId", o.raceId},
                {"gender", o.gender},
                {"items", items},
            });
        }
        j["outfits"] = oa;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WCHC: %s.wchc\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  classes : %zu  races : %zu  outfits : %zu\n",
                c.classes.size(), c.races.size(), c.outfits.size());
    if (!c.classes.empty()) {
        std::printf("\n  Classes:\n");
        std::printf("    id  power        baseHP   /lvl    name\n");
        for (const auto& cls : c.classes) {
            std::printf("    %2u  %-11s  %4u     %3u     %s\n",
                        cls.classId,
                        wowee::pipeline::WoweeChars::powerTypeName(cls.powerType),
                        cls.baseHealth, cls.baseHealthPerLevel,
                        cls.name.c_str());
        }
    }
    if (!c.races.empty()) {
        std::printf("\n  Races:\n");
        std::printf("    id  faction    map  zone   name\n");
        for (const auto& r : c.races) {
            std::printf("    %2u  %-9s  %3u  %5u   %s\n",
                        r.raceId,
                        wowee::pipeline::WoweeChars::raceFactionName(r.factionId),
                        r.startingMapId, r.startingZoneAreaId,
                        r.name.c_str());
        }
    }
    if (!c.outfits.empty()) {
        std::printf("\n  Outfits:\n");
        for (const auto& o : c.outfits) {
            std::printf("    class=%-2u race=%-2u gender=%u  items: ",
                        o.classId, o.raceId, o.gender);
            for (size_t k = 0; k < o.items.size(); ++k) {
                std::printf("%s%u@slot%u",
                            k > 0 ? ", " : "",
                            o.items[k].itemId, o.items[k].displaySlot);
            }
            std::printf("\n");
        }
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWchcExt(base);
    if (!wowee::pipeline::WoweeCharsLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wchc: WCHC not found: %s.wchc\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCharsLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.classes.empty() && c.races.empty()) {
        warnings.push_back("catalog has zero classes and zero races");
    }
    std::vector<uint32_t> classIdsSeen;
    for (size_t k = 0; k < c.classes.size(); ++k) {
        const auto& cls = c.classes[k];
        std::string ctx = "class " + std::to_string(k) +
                          " (id=" + std::to_string(cls.classId);
        if (!cls.name.empty()) ctx += " " + cls.name;
        ctx += ")";
        if (cls.classId == 0) errors.push_back(ctx + ": classId is 0");
        if (cls.name.empty()) errors.push_back(ctx + ": name is empty");
        if (cls.baseHealth == 0) {
            errors.push_back(ctx + ": baseHealth is 0 (character dies on creation)");
        }
        if (cls.factionAvailability == 0) {
            errors.push_back(ctx +
                ": factionAvailability=0 (no faction can pick this class)");
        }
        for (uint32_t prev : classIdsSeen) {
            if (prev == cls.classId) {
                errors.push_back(ctx + ": duplicate classId");
                break;
            }
        }
        classIdsSeen.push_back(cls.classId);
    }
    std::vector<uint32_t> raceIdsSeen;
    for (size_t k = 0; k < c.races.size(); ++k) {
        const auto& r = c.races[k];
        std::string ctx = "race " + std::to_string(k) +
                          " (id=" + std::to_string(r.raceId);
        if (!r.name.empty()) ctx += " " + r.name;
        ctx += ")";
        if (r.raceId == 0) errors.push_back(ctx + ": raceId is 0");
        if (r.name.empty()) errors.push_back(ctx + ": name is empty");
        if (r.factionId > wowee::pipeline::WoweeChars::Neutral) {
            errors.push_back(ctx + ": factionId " +
                std::to_string(r.factionId) + " not in 0..2");
        }
        for (uint32_t prev : raceIdsSeen) {
            if (prev == r.raceId) {
                errors.push_back(ctx + ": duplicate raceId");
                break;
            }
        }
        raceIdsSeen.push_back(r.raceId);
    }
    // Outfit cross-references must hit real classes / races.
    for (size_t k = 0; k < c.outfits.size(); ++k) {
        const auto& o = c.outfits[k];
        std::string ctx = "outfit " + std::to_string(k) +
                          " (class=" + std::to_string(o.classId) +
                          " race=" + std::to_string(o.raceId) + ")";
        if (!c.classes.empty() && !c.findClass(o.classId)) {
            errors.push_back(ctx + ": classId does not exist in this catalog");
        }
        if (!c.races.empty() && !c.findRace(o.raceId)) {
            errors.push_back(ctx + ": raceId does not exist in this catalog");
        }
        if (o.gender > wowee::pipeline::WoweeChars::Female) {
            errors.push_back(ctx + ": gender " +
                std::to_string(o.gender) + " not 0 or 1");
        }
        if (o.items.empty()) {
            warnings.push_back(ctx +
                ": no items (player starts naked / unarmed)");
        }
        for (size_t ii = 0; ii < o.items.size(); ++ii) {
            if (o.items[ii].itemId == 0) {
                errors.push_back(ctx + " item " + std::to_string(ii) +
                    ": itemId is 0");
            }
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wchc"] = base + ".wchc";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wchc: %s.wchc\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu classes, %zu races, %zu outfits, all IDs unique\n",
                    c.classes.size(), c.races.size(), c.outfits.size());
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

bool handleCharsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-chars") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-chars-alliance") == 0 && i + 1 < argc) {
        outRc = handleGenAlliance(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-chars-allraces") == 0 && i + 1 < argc) {
        outRc = handleGenAllRaces(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wchc") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wchc") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
