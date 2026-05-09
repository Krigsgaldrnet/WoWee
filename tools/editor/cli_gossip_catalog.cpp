#include "cli_gossip_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_gossip.hpp"
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

std::string stripWgspExt(std::string base) {
    stripExt(base, ".wgsp");
    return base;
}

void appendOptFlagsStr(std::string& s, uint32_t flags) {
    if (flags & wowee::pipeline::WoweeGossip::AllianceOnly) s += "alliance ";
    if (flags & wowee::pipeline::WoweeGossip::HordeOnly)    s += "horde ";
    if (flags & wowee::pipeline::WoweeGossip::Coinpouch)    s += "coin ";
    if (flags & wowee::pipeline::WoweeGossip::QuestGated)   s += "quest-gated ";
    if (flags & wowee::pipeline::WoweeGossip::Closes)       s += "closes ";
    if (s.empty()) s = "-";
    else if (s.back() == ' ') s.pop_back();
}

bool saveOrError(const wowee::pipeline::WoweeGossip& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeGossipLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wgsp\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

uint32_t totalOptions(const wowee::pipeline::WoweeGossip& c) {
    uint32_t n = 0;
    for (const auto& e : c.entries) n += static_cast<uint32_t>(e.options.size());
    return n;
}

void printGenSummary(const wowee::pipeline::WoweeGossip& c,
                     const std::string& base) {
    std::printf("Wrote %s.wgsp\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  menus   : %zu (%u options total)\n",
                c.entries.size(), totalOptions(c));
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterGossip";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgspExt(base);
    auto c = wowee::pipeline::WoweeGossipLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-gossip")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenInnkeeper(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "InnkeeperGossip";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgspExt(base);
    auto c = wowee::pipeline::WoweeGossipLoader::makeInnkeeper(name);
    if (!saveOrError(c, base, "gen-gossip-innkeeper")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenQuestGiver(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "QuestGiverGossip";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgspExt(base);
    auto c = wowee::pipeline::WoweeGossipLoader::makeQuestGiver(name);
    if (!saveOrError(c, base, "gen-gossip-questgiver")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgspExt(base);
    if (!wowee::pipeline::WoweeGossipLoader::exists(base)) {
        std::fprintf(stderr, "WGSP not found: %s.wgsp\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGossipLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wgsp"] = base + ".wgsp";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        j["totalOptions"] = totalOptions(c);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            nlohmann::json je;
            je["menuId"] = e.menuId;
            je["titleText"] = e.titleText;
            nlohmann::json opts = nlohmann::json::array();
            for (const auto& o : e.options) {
                std::string fs;
                appendOptFlagsStr(fs, o.requiredFlags);
                opts.push_back({
                    {"optionId", o.optionId},
                    {"text", o.text},
                    {"kind", o.kind},
                    {"kindName", wowee::pipeline::WoweeGossip::optionKindName(o.kind)},
                    {"actionTarget", o.actionTarget},
                    {"requiredFlags", o.requiredFlags},
                    {"requiredFlagsStr", fs},
                    {"moneyCostCopper", o.moneyCostCopper},
                });
            }
            je["options"] = opts;
            arr.push_back(je);
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WGSP: %s.wgsp\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  menus   : %zu (%u options total)\n",
                c.entries.size(), totalOptions(c));
    if (c.entries.empty()) return 0;
    for (const auto& e : c.entries) {
        std::printf("\n  menuId=%u\n", e.menuId);
        std::printf("    title : \"%s\"\n", e.titleText.c_str());
        if (e.options.empty()) {
            std::printf("    *no options*\n");
            continue;
        }
        for (const auto& o : e.options) {
            std::string fs;
            appendOptFlagsStr(fs, o.requiredFlags);
            std::printf("    [%-9s] target=%-5u  cost=%-6uc  flags=%s\n",
                        wowee::pipeline::WoweeGossip::optionKindName(o.kind),
                        o.actionTarget, o.moneyCostCopper, fs.c_str());
            std::printf("                 \"%s\"\n", o.text.c_str());
        }
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgspExt(base);
    if (!wowee::pipeline::WoweeGossipLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wgsp: WGSP not found: %s.wgsp\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGossipLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    idsSeen.reserve(c.entries.size());
    // Build the set of menuIds present so we can verify
    // intra-format Submenu cross-references resolve.
    std::vector<uint32_t> menuIds;
    menuIds.reserve(c.entries.size());
    for (const auto& e : c.entries) menuIds.push_back(e.menuId);
    auto hasMenu = [&](uint32_t id) {
        for (uint32_t m : menuIds) if (m == id) return true;
        return false;
    };
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (menuId=" + std::to_string(e.menuId) + ")";
        if (e.menuId == 0) {
            errors.push_back(ctx + ": menuId is 0");
        }
        if (e.titleText.empty()) {
            warnings.push_back(ctx + ": titleText is empty");
        }
        if (e.options.empty()) {
            warnings.push_back(ctx +
                ": no options (player has no way to dismiss the menu)");
        }
        // Alliance + Horde restriction is mutually exclusive.
        for (size_t oi = 0; oi < e.options.size(); ++oi) {
            const auto& o = e.options[oi];
            std::string octx = ctx + " option " + std::to_string(oi);
            if (o.kind > wowee::pipeline::WoweeGossip::Auctioneer) {
                errors.push_back(octx + ": kind " +
                    std::to_string(o.kind) + " not in known range 0..12");
            }
            if (o.text.empty()) {
                errors.push_back(octx + ": text is empty");
            }
            // Submenu must point at a menuId that exists in the catalog.
            if (o.kind == wowee::pipeline::WoweeGossip::Submenu) {
                if (o.actionTarget == 0) {
                    errors.push_back(octx + ": Submenu option has actionTarget=0");
                } else if (!hasMenu(o.actionTarget)) {
                    errors.push_back(octx + ": Submenu actionTarget " +
                        std::to_string(o.actionTarget) +
                        " does not exist in this catalog");
                }
            }
            // Coinpouch flag without moneyCost is misleading — the
            // coin icon would show with no actual fee.
            if ((o.requiredFlags & wowee::pipeline::WoweeGossip::Coinpouch) &&
                o.moneyCostCopper == 0) {
                warnings.push_back(octx +
                    ": Coinpouch flag set but moneyCostCopper=0");
            }
            if ((o.requiredFlags & wowee::pipeline::WoweeGossip::AllianceOnly) &&
                (o.requiredFlags & wowee::pipeline::WoweeGossip::HordeOnly)) {
                errors.push_back(octx +
                    ": AllianceOnly and HordeOnly both set (incoherent)");
            }
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.menuId) {
                errors.push_back(ctx + ": duplicate menuId");
                break;
            }
        }
        idsSeen.push_back(e.menuId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wgsp"] = base + ".wgsp";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wgsp: %s.wgsp\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu menus (%u options), all menuIds unique\n",
                    c.entries.size(), totalOptions(c));
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

bool handleGossipCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-gossip") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-gossip-innkeeper") == 0 && i + 1 < argc) {
        outRc = handleGenInnkeeper(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-gossip-questgiver") == 0 && i + 1 < argc) {
        outRc = handleGenQuestGiver(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wgsp") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wgsp") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
