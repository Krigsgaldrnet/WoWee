#include "cli_action_bars_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_action_bars.hpp"
#include <nlohmann/json.hpp>

#include <cctype>
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

std::string stripWactExt(std::string base) {
    stripExt(base, ".wact");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeActionBar& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeActionBarLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wact\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeActionBar& c,
                     const std::string& base) {
    std::printf("Wrote %s.wact\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  bindings : %zu\n", c.entries.size());
}

int handleGenWarrior(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WarriorActionBar";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWactExt(base);
    auto c = wowee::pipeline::WoweeActionBarLoader::makeWarrior(name);
    if (!saveOrError(c, base, "gen-act")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMage(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MageActionBar";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWactExt(base);
    auto c = wowee::pipeline::WoweeActionBarLoader::makeMage(name);
    if (!saveOrError(c, base, "gen-act-mage")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenHunterPet(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "HunterPetBar";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWactExt(base);
    auto c = wowee::pipeline::WoweeActionBarLoader::makeHunterPet(name);
    if (!saveOrError(c, base, "gen-act-pet")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWactExt(base);
    if (!wowee::pipeline::WoweeActionBarLoader::exists(base)) {
        std::fprintf(stderr, "WACT not found: %s.wact\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeActionBarLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wact"] = base + ".wact";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"bindingId", e.bindingId},
                {"name", e.name},
                {"description", e.description},
                {"classMask", e.classMask},
                {"spellId", e.spellId},
                {"itemId", e.itemId},
                {"buttonSlot", e.buttonSlot},
                {"barMode", e.barMode},
                {"barModeName", wowee::pipeline::WoweeActionBar::barModeName(e.barMode)},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WACT: %s.wact\n", base.c_str());
    std::printf("  catalog  : %s\n", c.name.c_str());
    std::printf("  bindings : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    classMask    bar       slot   spellId  itemId    name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   0x%08x   %-8s   %3u   %5u    %5u     %s\n",
                    e.bindingId, e.classMask,
                    wowee::pipeline::WoweeActionBar::barModeName(e.barMode),
                    e.buttonSlot, e.spellId, e.itemId,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWactExt(base);
    if (!wowee::pipeline::WoweeActionBarLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wact: WACT not found: %s.wact\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeActionBarLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.bindingId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.bindingId == 0)
            errors.push_back(ctx + ": bindingId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.classMask == 0)
            errors.push_back(ctx +
                ": classMask is 0 — no class can use this binding");
        if (e.barMode > wowee::pipeline::WoweeActionBar::Custom) {
            errors.push_back(ctx + ": barMode " +
                std::to_string(e.barMode) + " not in 0..6");
        }
        if (e.buttonSlot > 143) {
            warnings.push_back(ctx +
                ": buttonSlot " + std::to_string(e.buttonSlot) +
                " > 143 (12 bars × 12 slots = 144 max)");
        }
        // Both spellId and itemId set is contradictory.
        if (e.spellId != 0 && e.itemId != 0) {
            warnings.push_back(ctx +
                ": both spellId and itemId set — engine prefers "
                "spellId; itemId is ignored");
        }
        // Neither set means an empty button.
        if (e.spellId == 0 && e.itemId == 0) {
            warnings.push_back(ctx +
                ": both spellId=0 and itemId=0 — button will be empty");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.bindingId) {
                errors.push_back(ctx + ": duplicate bindingId");
                break;
            }
        }
        idsSeen.push_back(e.bindingId);
    }
    // Cross-entry: detect (classMask, barMode, buttonSlot)
    // collisions where overlapping classes would fight for
    // the same physical slot.
    for (size_t a = 0; a < c.entries.size(); ++a) {
        for (size_t b = a + 1; b < c.entries.size(); ++b) {
            const auto& ea = c.entries[a];
            const auto& eb = c.entries[b];
            if (ea.barMode != eb.barMode) continue;
            if (ea.buttonSlot != eb.buttonSlot) continue;
            if ((ea.classMask & eb.classMask) == 0) continue;
            warnings.push_back(
                "entries " + std::to_string(a) + " (" +
                ea.name + ") and " + std::to_string(b) + " (" +
                eb.name + ") share " +
                wowee::pipeline::WoweeActionBar::barModeName(ea.barMode) +
                " bar slot " + std::to_string(ea.buttonSlot) +
                " for overlapping classMask — slot collision");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wact"] = base + ".wact";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wact: %s.wact\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu bindings, all bindingIds unique, no slot collisions\n",
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

bool handleActionBarsCatalog(int& i, int argc, char** argv,
                             int& outRc) {
    if (std::strcmp(argv[i], "--gen-act") == 0 && i + 1 < argc) {
        outRc = handleGenWarrior(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-act-mage") == 0 && i + 1 < argc) {
        outRc = handleGenMage(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-act-pet") == 0 && i + 1 < argc) {
        outRc = handleGenHunterPet(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wact") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wact") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
