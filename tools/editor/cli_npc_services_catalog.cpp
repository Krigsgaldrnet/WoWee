#include "cli_npc_services_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_npc_services.hpp"
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

std::string stripWbkdExt(std::string base) {
    stripExt(base, ".wbkd");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeNPCService& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeNPCServiceLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wbkd\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeNPCService& c,
                     const std::string& base) {
    std::printf("Wrote %s.wbkd\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  services : %zu\n", c.entries.size());
}

int handleGenCity(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "CityServices";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbkdExt(base);
    auto c = wowee::pipeline::WoweeNPCServiceLoader::makeCity(name);
    if (!saveOrError(c, base, "gen-bkd")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenBattle(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "BattleServices";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbkdExt(base);
    auto c = wowee::pipeline::WoweeNPCServiceLoader::makeBattle(name);
    if (!saveOrError(c, base, "gen-bkd-battle")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenProfession(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ProfessionServices";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWbkdExt(base);
    auto c = wowee::pipeline::WoweeNPCServiceLoader::makeProfession(name);
    if (!saveOrError(c, base, "gen-bkd-profession")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWbkdExt(base);
    if (!wowee::pipeline::WoweeNPCServiceLoader::exists(base)) {
        std::fprintf(stderr, "WBKD not found: %s.wbkd\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeNPCServiceLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wbkd"] = base + ".wbkd";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"serviceId", e.serviceId},
                {"name", e.name},
                {"description", e.description},
                {"serviceKind", e.serviceKind},
                {"serviceKindName", wowee::pipeline::WoweeNPCService::serviceKindName(e.serviceKind)},
                {"requiresGold", e.requiresGold},
                {"factionRequiredId", e.factionRequiredId},
                {"gossipTextId", e.gossipTextId},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WBKD: %s.wbkd\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  services : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind             gold      faction   gossip   name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u    %-13s   %6u      %5u    %5u    %s\n",
                    e.serviceId,
                    wowee::pipeline::WoweeNPCService::serviceKindName(e.serviceKind),
                    e.requiresGold, e.factionRequiredId,
                    e.gossipTextId, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWbkdExt(base);
    if (!wowee::pipeline::WoweeNPCServiceLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wbkd: WBKD not found: %s.wbkd\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeNPCServiceLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.serviceId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.serviceId == 0)
            errors.push_back(ctx + ": serviceId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.serviceKind > wowee::pipeline::WoweeNPCService::Misc) {
            errors.push_back(ctx + ": serviceKind " +
                std::to_string(e.serviceKind) + " not in 0..11");
        }
        // Mailbox is a gameobject service, not an NPC —
        // shouldn't have a gossipTextId. Warn so authors
        // double-check.
        if (e.serviceKind == wowee::pipeline::WoweeNPCService::Mailbox &&
            e.gossipTextId != 0) {
            warnings.push_back(ctx +
                ": Mailbox kind with gossipTextId=" +
                std::to_string(e.gossipTextId) +
                " — mailboxes are gameobject services with "
                "no NPC dialogue; gossip will not display");
        }
        // Innkeeper without a gossip text reads as a silent
        // bind interaction — usually a missing link.
        if (e.serviceKind == wowee::pipeline::WoweeNPCService::Innkeeper &&
            e.gossipTextId == 0) {
            warnings.push_back(ctx +
                ": Innkeeper kind with gossipTextId=0 — "
                "no welcome/bind dialogue, will silently bind");
        }
        // Battlemaster gold cost > 0 is unusual — battle
        // queues are typically free.
        if (e.serviceKind == wowee::pipeline::WoweeNPCService::Battlemaster &&
            e.requiresGold > 0) {
            warnings.push_back(ctx +
                ": Battlemaster kind with requiresGold=" +
                std::to_string(e.requiresGold) +
                " — battle queue services are typically free");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.serviceId) {
                errors.push_back(ctx + ": duplicate serviceId");
                break;
            }
        }
        idsSeen.push_back(e.serviceId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wbkd"] = base + ".wbkd";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wbkd: %s.wbkd\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu services, all serviceIds unique\n",
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

bool handleNPCServicesCatalog(int& i, int argc, char** argv,
                              int& outRc) {
    if (std::strcmp(argv[i], "--gen-bkd") == 0 && i + 1 < argc) {
        outRc = handleGenCity(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-bkd-battle") == 0 && i + 1 < argc) {
        outRc = handleGenBattle(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-bkd-profession") == 0 && i + 1 < argc) {
        outRc = handleGenProfession(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wbkd") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wbkd") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
