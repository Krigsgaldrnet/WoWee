#include "cli_vehicles_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_vehicles.hpp"
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

std::string stripWvhcExt(std::string base) {
    stripExt(base, ".wvhc");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeVehicle& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeVehicleLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wvhc\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

size_t totalSeats(const wowee::pipeline::WoweeVehicle& c) {
    size_t n = 0;
    for (const auto& e : c.entries) n += e.seats.size();
    return n;
}

void printGenSummary(const wowee::pipeline::WoweeVehicle& c,
                     const std::string& base) {
    std::printf("Wrote %s.wvhc\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  vehicles : %zu (%zu seats total)\n",
                c.entries.size(), totalSeats(c));
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterVehicles";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWvhcExt(base);
    auto c = wowee::pipeline::WoweeVehicleLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-vehicles")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenSiege(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "SiegeVehicles";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWvhcExt(base);
    auto c = wowee::pipeline::WoweeVehicleLoader::makeSiege(name);
    if (!saveOrError(c, base, "gen-vehicles-siege")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenFlying(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FlyingVehicles";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWvhcExt(base);
    auto c = wowee::pipeline::WoweeVehicleLoader::makeFlying(name);
    if (!saveOrError(c, base, "gen-vehicles-flying")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWvhcExt(base);
    if (!wowee::pipeline::WoweeVehicleLoader::exists(base)) {
        std::fprintf(stderr, "WVHC not found: %s.wvhc\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeVehicleLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wvhc"] = base + ".wvhc";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            nlohmann::json seats = nlohmann::json::array();
            for (const auto& s : e.seats) {
                seats.push_back({
                    {"seatIndex", s.seatIndex},
                    {"seatFlags", s.seatFlags},
                    {"attachmentId", s.attachmentId},
                    {"controlSpellId", s.controlSpellId},
                    {"exitSpellId", s.exitSpellId},
                    {"passengerYaw", s.passengerYaw},
                });
            }
            arr.push_back({
                {"vehicleId", e.vehicleId},
                {"creatureId", e.creatureId},
                {"name", e.name},
                {"description", e.description},
                {"vehicleKind", e.vehicleKind},
                {"vehicleKindName", wowee::pipeline::WoweeVehicle::vehicleKindName(e.vehicleKind)},
                {"movementKind", e.movementKind},
                {"movementKindName", wowee::pipeline::WoweeVehicle::movementKindName(e.movementKind)},
                {"turnSpeed", e.turnSpeed},
                {"pitchSpeed", e.pitchSpeed},
                {"flightCapabilityId", e.flightCapabilityId},
                {"powerType", e.powerType},
                {"powerTypeName", wowee::pipeline::WoweeVehicle::powerTypeName(e.powerType)},
                {"maxPower", e.maxPower},
                {"seats", seats},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WVHC: %s.wvhc\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  vehicles : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   creature   kind            move      power    seats  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %5u      %-13s   %-7s   %-7s  %3zu    %s\n",
                    e.vehicleId, e.creatureId,
                    wowee::pipeline::WoweeVehicle::vehicleKindName(e.vehicleKind),
                    wowee::pipeline::WoweeVehicle::movementKindName(e.movementKind),
                    wowee::pipeline::WoweeVehicle::powerTypeName(e.powerType),
                    e.seats.size(), e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWvhcExt(base);
    if (!wowee::pipeline::WoweeVehicleLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wvhc: WVHC not found: %s.wvhc\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeVehicleLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.vehicleId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.vehicleId == 0) errors.push_back(ctx + ": vehicleId is 0");
        if (e.name.empty()) errors.push_back(ctx + ": name is empty");
        if (e.creatureId == 0)
            errors.push_back(ctx + ": creatureId is 0 "
                "(no rendered model)");
        if (e.vehicleKind > wowee::pipeline::WoweeVehicle::SiegeWeapon) {
            errors.push_back(ctx + ": vehicleKind " +
                std::to_string(e.vehicleKind) + " not in 0..7");
        }
        if (e.movementKind > wowee::pipeline::WoweeVehicle::AmphibiousGW) {
            errors.push_back(ctx + ": movementKind " +
                std::to_string(e.movementKind) + " not in 0..5");
        }
        if (e.powerType > wowee::pipeline::WoweeVehicle::None) {
            errors.push_back(ctx + ": powerType " +
                std::to_string(e.powerType) + " not in 0..4");
        }
        if (e.seats.empty()) {
            errors.push_back(ctx +
                ": no seats (vehicle has no rideable position)");
        }
        // Flying vehicles MUST be on Air or AmphibiousAW
        // movement, otherwise they fall through the world.
        if ((e.vehicleKind == wowee::pipeline::WoweeVehicle::FlyingMount ||
             e.vehicleKind == wowee::pipeline::WoweeVehicle::Gunship) &&
            e.movementKind != wowee::pipeline::WoweeVehicle::Air &&
            e.movementKind != wowee::pipeline::WoweeVehicle::AmphibiousAW) {
            errors.push_back(ctx +
                ": flying vehicle without Air/AmphibiousAW movement "
                "(would fall through world)");
        }
        // Driver-flag exclusivity check.
        int driverCount = 0;
        std::vector<uint8_t> seatIdxSeen;
        for (size_t si = 0; si < e.seats.size(); ++si) {
            const auto& s = e.seats[si];
            if (s.seatFlags & wowee::pipeline::WoweeVehicle::kSeatDriver) {
                ++driverCount;
            }
            for (uint8_t prev : seatIdxSeen) {
                if (prev == s.seatIndex) {
                    errors.push_back(ctx + ": seat[" +
                        std::to_string(si) + "] duplicate seatIndex=" +
                        std::to_string(s.seatIndex));
                    break;
                }
            }
            seatIdxSeen.push_back(s.seatIndex);
        }
        if (driverCount == 0) {
            warnings.push_back(ctx +
                ": no seat marked kSeatDriver "
                "(no one can steer this vehicle)");
        }
        if (driverCount > 1) {
            errors.push_back(ctx +
                ": multiple seats marked kSeatDriver (driverCount=" +
                std::to_string(driverCount) + ")");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.vehicleId) {
                errors.push_back(ctx + ": duplicate vehicleId");
                break;
            }
        }
        idsSeen.push_back(e.vehicleId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wvhc"] = base + ".wvhc";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wvhc: %s.wvhc\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu vehicles, %zu seats, all vehicleIds unique\n",
                    c.entries.size(), totalSeats(c));
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

bool handleVehiclesCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-vehicles") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-vehicles-siege") == 0 && i + 1 < argc) {
        outRc = handleGenSiege(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-vehicles-flying") == 0 && i + 1 < argc) {
        outRc = handleGenFlying(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wvhc") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wvhc") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
