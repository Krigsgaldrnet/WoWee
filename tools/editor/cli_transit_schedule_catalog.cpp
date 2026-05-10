#include "cli_transit_schedule_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_transit_schedule.hpp"
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

std::string stripWtscExt(std::string base) {
    stripExt(base, ".wtsc");
    return base;
}

const char* vehicleTypeName(uint8_t v) {
    using T = wowee::pipeline::WoweeTransitSchedule;
    switch (v) {
        case T::Taxi:     return "taxi";
        case T::Zeppelin: return "zeppelin";
        case T::Boat:     return "boat";
        case T::Mount:    return "mount";
        default:          return "?";
    }
}

const char* factionAccessName(uint8_t f) {
    using T = wowee::pipeline::WoweeTransitSchedule;
    switch (f) {
        case T::Both:     return "both";
        case T::Alliance: return "alliance";
        case T::Horde:    return "horde";
        case T::Neutral:  return "neutral";
        default:          return "?";
    }
}

bool saveOrError(const wowee::pipeline::WoweeTransitSchedule& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeTransitScheduleLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wtsc\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeTransitSchedule& c,
                     const std::string& base) {
    std::printf("Wrote %s.wtsc\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  routes  : %zu\n", c.entries.size());
}

int handleGenZeppelins(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "VanillaZeppelins";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtscExt(base);
    auto c = wowee::pipeline::WoweeTransitScheduleLoader::
        makeZeppelins(name);
    if (!saveOrError(c, base, "gen-trn-zeppelins")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBoats(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "VanillaBoats";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtscExt(base);
    auto c = wowee::pipeline::WoweeTransitScheduleLoader::
        makeBoats(name);
    if (!saveOrError(c, base, "gen-trn-boats")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenTaxis(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "VanillaTaxis";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtscExt(base);
    auto c = wowee::pipeline::WoweeTransitScheduleLoader::
        makeTaxis(name);
    if (!saveOrError(c, base, "gen-trn-taxis")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWtscExt(base);
    if (!wowee::pipeline::WoweeTransitScheduleLoader::exists(base)) {
        std::fprintf(stderr, "WTSC not found: %s.wtsc\n",
                     base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeTransitScheduleLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wtsc"] = base + ".wtsc";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"routeId", e.routeId},
                {"name", e.name},
                {"vehicleType", e.vehicleType},
                {"vehicleTypeName",
                    vehicleTypeName(e.vehicleType)},
                {"factionAccess", e.factionAccess},
                {"factionAccessName",
                    factionAccessName(e.factionAccess)},
                {"originName", e.originName},
                {"originX", e.originX},
                {"originY", e.originY},
                {"originMapId", e.originMapId},
                {"destinationName", e.destinationName},
                {"destinationX", e.destinationX},
                {"destinationY", e.destinationY},
                {"destinationMapId", e.destinationMapId},
                {"departureIntervalSec",
                    e.departureIntervalSec},
                {"travelDurationSec", e.travelDurationSec},
                {"capacity", e.capacity},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WTSC: %s.wtsc\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  routes  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id  vehicle    fact      intv  travel  cap   name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u  %-9s  %-8s  %4us   %4us  %4u  %s\n",
                    e.routeId,
                    vehicleTypeName(e.vehicleType),
                    factionAccessName(e.factionAccess),
                    e.departureIntervalSec,
                    e.travelDurationSec,
                    e.capacity, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWtscExt(base);
    if (!wowee::pipeline::WoweeTransitScheduleLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wtsc: WTSC not found: %s.wtsc\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeTransitScheduleLoader::load(base);
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
                          " (id=" + std::to_string(e.routeId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.routeId == 0)
            errors.push_back(ctx + ": routeId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.originName.empty())
            errors.push_back(ctx + ": originName is empty");
        if (e.destinationName.empty())
            errors.push_back(ctx +
                ": destinationName is empty");
        if (e.vehicleType > 3) {
            errors.push_back(ctx + ": vehicleType " +
                std::to_string(e.vehicleType) +
                " out of range (0..3)");
        }
        if (e.factionAccess > 3) {
            errors.push_back(ctx + ": factionAccess " +
                std::to_string(e.factionAccess) +
                " out of range (0..3)");
        }
        // Critical scheduling invariant: a new
        // departure cannot leave before the previous
        // one has arrived if capacity is finite — an
        // interval shorter than travel would
        // overflow the route's vehicle pool. (This
        // doesn't apply to capacity==0 = solo
        // gryphon, where each ride is independent.)
        if (e.capacity > 0 &&
            e.departureIntervalSec > 0 &&
            e.travelDurationSec > 0 &&
            e.departureIntervalSec < e.travelDurationSec) {
            errors.push_back(ctx +
                ": departureIntervalSec=" +
                std::to_string(e.departureIntervalSec) +
                " < travelDurationSec=" +
                std::to_string(e.travelDurationSec) +
                " with finite capacity — vehicle pool "
                "overflow (next zeppelin departs "
                "before prior arrives)");
        }
        if (e.departureIntervalSec == 0) {
            errors.push_back(ctx +
                ": departureIntervalSec is 0 (route "
                "would never depart)");
        }
        if (e.travelDurationSec == 0) {
            errors.push_back(ctx +
                ": travelDurationSec is 0 (route would "
                "instant-teleport, not a transit)");
        }
        // Same-map vehicle: not an error (some
        // vanilla flightpaths cross only intra-zone)
        // but is worth flagging — the reader may want
        // to verify this is intentional.
        if (e.originMapId == e.destinationMapId &&
            e.originMapId != 0) {
            warnings.push_back(ctx +
                ": originMapId == destinationMapId=" +
                std::to_string(e.originMapId) +
                " — same-map route, verify intentional");
        }
        // No identical (origin, destination) pair within
        // a single catalog — would be a duplicate route.
        if (!e.name.empty() &&
            !namesSeen.insert(e.name).second) {
            errors.push_back(ctx +
                ": duplicate route name '" + e.name +
                "' — UI dispatch would route ambiguously");
        }
        if (!idsSeen.insert(e.routeId).second) {
            errors.push_back(ctx + ": duplicate routeId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wtsc"] = base + ".wtsc";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wtsc: %s.wtsc\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu routes, all routeIds + "
                    "names unique, vehicleType 0..3, "
                    "factionAccess 0..3, no zero "
                    "intervals/travel, no scheduling "
                    "overflow (interval >= travel where "
                    "capacity is finite)\n",
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

bool handleTransitScheduleCatalog(int& i, int argc, char** argv,
                                    int& outRc) {
    if (std::strcmp(argv[i], "--gen-trn-zeppelins") == 0 &&
        i + 1 < argc) {
        outRc = handleGenZeppelins(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-trn-boats") == 0 &&
        i + 1 < argc) {
        outRc = handleGenBoats(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-trn-taxis") == 0 &&
        i + 1 < argc) {
        outRc = handleGenTaxis(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wtsc") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wtsc") == 0 &&
        i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
