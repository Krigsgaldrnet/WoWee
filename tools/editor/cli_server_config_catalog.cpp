#include "cli_server_config_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_server_config.hpp"
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

std::string stripWcfgExt(std::string base) {
    stripExt(base, ".wcfg");
    return base;
}

const char* configKindName(uint8_t k) {
    using C = wowee::pipeline::WoweeServerConfig;
    switch (k) {
        case C::XPRate:      return "xprate";
        case C::DropRate:    return "droprate";
        case C::HonorRate:   return "honorrate";
        case C::RestedXP:    return "restedxp";
        case C::RealmType:   return "realmtype";
        case C::WorldFlag:   return "worldflag";
        case C::Performance: return "performance";
        case C::Security:    return "security";
        case C::Misc:        return "misc";
        default:             return "unknown";
    }
}

const char* valueKindName(uint8_t k) {
    using C = wowee::pipeline::WoweeServerConfig;
    switch (k) {
        case C::Float:  return "float";
        case C::Int:    return "int";
        case C::Bool:   return "bool";
        case C::String: return "string";
        default:        return "unknown";
    }
}

// Render the active value field (per valueKind) as a
// terse human-readable string. Used by the info display
// to show only the meaningful field per entry.
std::string activeValueString(
    const wowee::pipeline::WoweeServerConfig::Entry& e) {
    using C = wowee::pipeline::WoweeServerConfig;
    char buf[64];
    switch (e.valueKind) {
        case C::Float:
            std::snprintf(buf, sizeof(buf), "%.4f",
                          e.floatValue);
            return buf;
        case C::Int:
            std::snprintf(buf, sizeof(buf), "%lld",
                          static_cast<long long>(e.intValue));
            return buf;
        case C::Bool:
            return e.intValue != 0 ? "true" : "false";
        case C::String:
            return "\"" + e.strValue + "\"";
        default:
            return "?";
    }
}

bool saveOrError(const wowee::pipeline::WoweeServerConfig& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeServerConfigLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wcfg\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeServerConfig& c,
                     const std::string& base) {
    std::printf("Wrote %s.wcfg\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  configs : %zu\n", c.entries.size());
}

int handleGenRates(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "RateMultipliers";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcfgExt(base);
    auto c = wowee::pipeline::WoweeServerConfigLoader::makeRates(name);
    if (!saveOrError(c, base, "gen-cfg")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenPerf(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "PerformanceTuning";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcfgExt(base);
    auto c = wowee::pipeline::WoweeServerConfigLoader::makePerformance(name);
    if (!saveOrError(c, base, "gen-cfg-perf")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenSecurity(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "SecurityPolicy";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcfgExt(base);
    auto c = wowee::pipeline::WoweeServerConfigLoader::makeSecurity(name);
    if (!saveOrError(c, base, "gen-cfg-sec")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcfgExt(base);
    if (!wowee::pipeline::WoweeServerConfigLoader::exists(base)) {
        std::fprintf(stderr, "WCFG not found: %s.wcfg\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeServerConfigLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wcfg"] = base + ".wcfg";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"configId", e.configId},
                {"name", e.name},
                {"description", e.description},
                {"configKind", e.configKind},
                {"configKindName", configKindName(e.configKind)},
                {"valueKind", e.valueKind},
                {"valueKindName", valueKindName(e.valueKind)},
                {"restartRequired", e.restartRequired != 0},
                {"floatValue", e.floatValue},
                {"intValue", e.intValue},
                {"strValue", e.strValue},
                {"activeValue", activeValueString(e)},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WCFG: %s.wcfg\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  configs : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   kind          valueKind  restart  value                 name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-11s   %-8s   %s        %-20s   %s\n",
                    e.configId, configKindName(e.configKind),
                    valueKindName(e.valueKind),
                    e.restartRequired ? "Y" : "n",
                    activeValueString(e).c_str(),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcfgExt(base);
    if (!wowee::pipeline::WoweeServerConfigLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wcfg: WCFG not found: %s.wcfg\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeServerConfigLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    std::set<std::string> namesSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.configId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.configId == 0)
            errors.push_back(ctx + ": configId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.configKind > 7 && e.configKind != 255) {
            errors.push_back(ctx + ": configKind " +
                std::to_string(e.configKind) +
                " out of range (must be 0..7 or 255 Misc)");
        }
        if (e.valueKind > 3) {
            errors.push_back(ctx + ": valueKind " +
                std::to_string(e.valueKind) +
                " out of range (must be 0..3)");
        }
        // Per-valueKind validity: each kind requires its
        // matching field to carry meaningful data, others
        // should be unset (server ignores them).
        using C = wowee::pipeline::WoweeServerConfig;
        if (e.valueKind == C::Float &&
            e.floatValue == 0.0f && e.intValue != 0) {
            warnings.push_back(ctx +
                ": valueKind=Float but intValue is "
                "non-zero — intValue will be ignored "
                "by the server but stored on disk; "
                "consider clearing for cleaner serialization");
        }
        if (e.valueKind == C::Int && e.floatValue != 0.0f) {
            warnings.push_back(ctx +
                ": valueKind=Int but floatValue is "
                "non-zero — floatValue will be ignored "
                "by the server");
        }
        if (e.valueKind == C::Bool && e.intValue != 0 &&
            e.intValue != 1) {
            errors.push_back(ctx +
                ": valueKind=Bool but intValue is " +
                std::to_string(e.intValue) +
                " (must be 0 or 1)");
        }
        if (e.valueKind == C::String && e.strValue.empty()) {
            warnings.push_back(ctx +
                ": valueKind=String but strValue is "
                "empty — config would default to empty "
                "string");
        }
        // Names should be unique within a catalog (the
        // server uses name as the primary lookup key for
        // config-by-name access patterns common in
        // legacy SQL-driven server configs).
        if (!e.name.empty() &&
            !namesSeen.insert(e.name).second) {
            errors.push_back(ctx +
                ": duplicate config name '" + e.name +
                "' — server name-based lookups would "
                "be ambiguous");
        }
        if (!idsSeen.insert(e.configId).second) {
            errors.push_back(ctx + ": duplicate configId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wcfg"] = base + ".wcfg";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wcfg: %s.wcfg\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu configs, all configIds + "
                    "names unique, per-kind value semantics "
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

bool handleServerConfigCatalog(int& i, int argc, char** argv,
                                int& outRc) {
    if (std::strcmp(argv[i], "--gen-cfg") == 0 && i + 1 < argc) {
        outRc = handleGenRates(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cfg-perf") == 0 && i + 1 < argc) {
        outRc = handleGenPerf(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cfg-sec") == 0 && i + 1 < argc) {
        outRc = handleGenSecurity(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wcfg") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wcfg") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
