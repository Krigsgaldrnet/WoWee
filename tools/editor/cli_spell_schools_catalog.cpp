#include "cli_spell_schools_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_spell_schools.hpp"
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

std::string stripWschExt(std::string base) {
    stripExt(base, ".wsch");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeSpellSchool& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSpellSchoolLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wsch\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSpellSchool& c,
                     const std::string& base) {
    std::printf("Wrote %s.wsch\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  schools : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterSchools";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWschExt(base);
    auto c = wowee::pipeline::WoweeSpellSchoolLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-sch")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMagical(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MagicalSchools";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWschExt(base);
    auto c = wowee::pipeline::WoweeSpellSchoolLoader::makeMagical(name);
    if (!saveOrError(c, base, "gen-sch-magical")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenCombined(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "CombinedSchools";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWschExt(base);
    auto c = wowee::pipeline::WoweeSpellSchoolLoader::makeCombined(name);
    if (!saveOrError(c, base, "gen-sch-combined")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWschExt(base);
    if (!wowee::pipeline::WoweeSpellSchoolLoader::exists(base)) {
        std::fprintf(stderr, "WSCH not found: %s.wsch\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellSchoolLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wsch"] = base + ".wsch";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"schoolId", e.schoolId},
                {"name", e.name},
                {"description", e.description},
                {"iconPath", e.iconPath},
                {"canBeImmune", e.canBeImmune},
                {"canBeAbsorbed", e.canBeAbsorbed},
                {"canBeReflected", e.canBeReflected},
                {"canCrit", e.canCrit},
                {"colorRGBA", e.colorRGBA},
                {"baseResistanceCap", e.baseResistanceCap},
                {"castSoundId", e.castSoundId},
                {"impactSoundId", e.impactSoundId},
                {"combinedSchoolMask", e.combinedSchoolMask},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSCH: %s.wsch\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  schools : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    schoolId    immune absorb reflect crit  resCap  combined-mask  name\n");
    for (const auto& e : c.entries) {
        std::printf("  0x%08x   %u      %u      %u      %u    %5u    0x%08x   %s\n",
                    e.schoolId, e.canBeImmune, e.canBeAbsorbed,
                    e.canBeReflected, e.canCrit,
                    e.baseResistanceCap, e.combinedSchoolMask,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWschExt(base);
    if (!wowee::pipeline::WoweeSpellSchoolLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wsch: WSCH not found: %s.wsch\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellSchoolLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    // Build the set of canonical (single-bit) school IDs so we
    // can check that combinedSchoolMask only references real
    // schools defined in this catalog.
    std::vector<uint32_t> canonicalIds;
    for (const auto& e : c.entries) {
        // A canonical school is one whose schoolId is a single
        // power of 2 (only one bit set). Hybrid schools have
        // the high bit set or multiple bits.
        if (e.schoolId != 0 &&
            (e.schoolId & (e.schoolId - 1)) == 0 &&
            e.schoolId < (1u << 16)) {
            canonicalIds.push_back(e.schoolId);
        }
    }
    auto schoolBitDefined = [&](uint32_t bit) {
        for (uint32_t s : canonicalIds) if (s == bit) return true;
        return false;
    };
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=0x" +
                          [&]() {
                              char buf[16];
                              std::snprintf(buf, sizeof(buf),
                                  "%08x", e.schoolId);
                              return std::string(buf);
                          }();
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.schoolId == 0)
            errors.push_back(ctx + ": schoolId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        // canBeReflected without canBeAbsorbed is unusual —
        // reflected damage typically also goes through absorb.
        if (e.canBeReflected && !e.canBeAbsorbed) {
            warnings.push_back(ctx +
                ": canBeReflected=1 but canBeAbsorbed=0 "
                "(reflected damage usually absorbable too)");
        }
        // combinedSchoolMask must only reference canonical
        // school bits that exist in this catalog.
        if (e.combinedSchoolMask != 0) {
            for (int b = 0; b < 16; ++b) {
                uint32_t bit = 1u << b;
                if ((e.combinedSchoolMask & bit) &&
                    !schoolBitDefined(bit)) {
                    warnings.push_back(ctx +
                        ": combinedSchoolMask references bit 0x" +
                        [&]() {
                            char buf[16];
                            std::snprintf(buf, sizeof(buf),
                                "%x", bit);
                            return std::string(buf);
                        }() +
                        " which isn't defined in this catalog");
                }
            }
            // A hybrid school's combinedSchoolMask should not
            // include itself (would be self-referential and
            // confuses the resistance lookup).
            if (e.combinedSchoolMask & e.schoolId) {
                errors.push_back(ctx +
                    ": combinedSchoolMask includes own schoolId "
                    "(self-referential)");
            }
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.schoolId) {
                errors.push_back(ctx + ": duplicate schoolId");
                break;
            }
        }
        idsSeen.push_back(e.schoolId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wsch"] = base + ".wsch";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wsch: %s.wsch\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu schools, all schoolIds unique, "
                    "all combined masks resolve\n", c.entries.size());
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

bool handleSpellSchoolsCatalog(int& i, int argc, char** argv,
                               int& outRc) {
    if (std::strcmp(argv[i], "--gen-sch") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-sch-magical") == 0 &&
        i + 1 < argc) {
        outRc = handleGenMagical(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-sch-combined") == 0 &&
        i + 1 < argc) {
        outRc = handleGenCombined(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wsch") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wsch") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
