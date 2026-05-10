#include "cli_char_features_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_char_features.hpp"
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

std::string stripWchfExt(std::string base) {
    stripExt(base, ".wchf");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeCharFeature& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeCharFeatureLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wchf\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeCharFeature& c,
                     const std::string& base) {
    std::printf("Wrote %s.wchf\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  features : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterCharFeatures";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWchfExt(base);
    auto c = wowee::pipeline::WoweeCharFeatureLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-chf")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBloodElf(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BloodElfFemaleHair";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWchfExt(base);
    auto c = wowee::pipeline::WoweeCharFeatureLoader::makeBloodElfFemale(name);
    if (!saveOrError(c, base, "gen-chf-bloodelf")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenTauren(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "TaurenMaleFeatures";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWchfExt(base);
    auto c = wowee::pipeline::WoweeCharFeatureLoader::makeTauren(name);
    if (!saveOrError(c, base, "gen-chf-tauren")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWchfExt(base);
    if (!wowee::pipeline::WoweeCharFeatureLoader::exists(base)) {
        std::fprintf(stderr, "WCHF not found: %s.wchf\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCharFeatureLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wchf"] = base + ".wchf";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"featureId", e.featureId},
                {"raceId", e.raceId},
                {"name", e.name},
                {"description", e.description},
                {"texturePath", e.texturePath},
                {"featureKind", e.featureKind},
                {"featureKindName", wowee::pipeline::WoweeCharFeature::featureKindName(e.featureKind)},
                {"sexId", e.sexId},
                {"sexIdName", wowee::pipeline::WoweeCharFeature::sexIdName(e.sexId)},
                {"variationIndex", e.variationIndex},
                {"requiresExpansion", e.requiresExpansion},
                {"requiresExpansionName", wowee::pipeline::WoweeCharFeature::expansionGateName(e.requiresExpansion)},
                {"geosetGroupBits", e.geosetGroupBits},
                {"hairColorOverlayId", e.hairColorOverlayId},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WCHF: %s.wchf\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  features : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    race  sex     kind          var  geosets     exp        name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u    %3u  %-6s  %-12s  %3u  0x%08x  %-7s    %s\n",
                    e.featureId, e.raceId,
                    wowee::pipeline::WoweeCharFeature::sexIdName(e.sexId),
                    wowee::pipeline::WoweeCharFeature::featureKindName(e.featureKind),
                    e.variationIndex, e.geosetGroupBits,
                    wowee::pipeline::WoweeCharFeature::expansionGateName(e.requiresExpansion),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWchfExt(base);
    if (!wowee::pipeline::WoweeCharFeatureLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wchf: WCHF not found: %s.wchf\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCharFeatureLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    // (race, sex, kind, variation) tuples must be unique —
    // duplicates would shadow each other in the carousel.
    std::set<std::string> tupleSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.featureId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.featureId == 0)
            errors.push_back(ctx + ": featureId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.raceId == 0)
            errors.push_back(ctx +
                ": raceId is 0 (feature not bound to a WCHC race)");
        if (e.featureKind > wowee::pipeline::WoweeCharFeature::Markings) {
            errors.push_back(ctx + ": featureKind " +
                std::to_string(e.featureKind) + " not in 0..8");
        }
        if (e.sexId > wowee::pipeline::WoweeCharFeature::Female) {
            errors.push_back(ctx + ": sexId " +
                std::to_string(e.sexId) + " not in 0..1");
        }
        if (e.requiresExpansion > wowee::pipeline::WoweeCharFeature::TurtleWoW) {
            errors.push_back(ctx + ": requiresExpansion " +
                std::to_string(e.requiresExpansion) + " not in 0..3");
        }
        if (e.texturePath.empty()) {
            errors.push_back(ctx +
                ": texturePath is empty (feature has no texture)");
        }
        // Check tuple uniqueness.
        std::string tuple =
            std::to_string(e.raceId) + "/" +
            std::to_string(e.sexId) + "/" +
            std::to_string(e.featureKind) + "/" +
            std::to_string(e.variationIndex);
        if (tupleSeen.count(tuple)) {
            errors.push_back(ctx +
                ": duplicate (race=" + std::to_string(e.raceId) +
                ", sex=" + std::to_string(e.sexId) +
                ", kind=" + std::to_string(e.featureKind) +
                ", variation=" + std::to_string(e.variationIndex) +
                ") — would shadow earlier entry in carousel");
        }
        tupleSeen.insert(tuple);
        for (uint32_t prev : idsSeen) {
            if (prev == e.featureId) {
                errors.push_back(ctx + ": duplicate featureId");
                break;
            }
        }
        idsSeen.push_back(e.featureId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wchf"] = base + ".wchf";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wchf: %s.wchf\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu features, all featureIds unique, "
                    "all (race,sex,kind,variation) tuples unique\n",
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

bool handleCharFeaturesCatalog(int& i, int argc, char** argv,
                               int& outRc) {
    if (std::strcmp(argv[i], "--gen-chf") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-chf-bloodelf") == 0 && i + 1 < argc) {
        outRc = handleGenBloodElf(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-chf-tauren") == 0 && i + 1 < argc) {
        outRc = handleGenTauren(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wchf") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wchf") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
