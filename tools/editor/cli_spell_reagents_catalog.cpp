#include "cli_spell_reagents_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_spell_reagents.hpp"
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

std::string stripWsprExt(std::string base) {
    stripExt(base, ".wspr");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeSpellReagent& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSpellReagentLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wspr\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSpellReagent& c,
                     const std::string& base) {
    std::printf("Wrote %s.wspr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  sets    : %zu\n", c.entries.size());
}

int handleGenMage(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MageReagents";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsprExt(base);
    auto c = wowee::pipeline::WoweeSpellReagentLoader::makeMage(name);
    if (!saveOrError(c, base, "gen-spr")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenWarlock(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "WarlockReagents";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsprExt(base);
    auto c = wowee::pipeline::WoweeSpellReagentLoader::makeWarlock(name);
    if (!saveOrError(c, base, "gen-spr-warlock")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenRez(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ResurrectionReagents";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWsprExt(base);
    auto c = wowee::pipeline::WoweeSpellReagentLoader::makeRez(name);
    if (!saveOrError(c, base, "gen-spr-rez")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsprExt(base);
    if (!wowee::pipeline::WoweeSpellReagentLoader::exists(base)) {
        std::fprintf(stderr, "WSPR not found: %s.wspr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellReagentLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wspr"] = base + ".wspr";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            nlohmann::json items = nlohmann::json::array();
            nlohmann::json counts = nlohmann::json::array();
            for (int s = 0; s < wowee::pipeline::WoweeSpellReagent::kMaxReagentSlots; ++s) {
                items.push_back(e.reagentItemId[s]);
                counts.push_back(e.reagentCount[s]);
            }
            arr.push_back({
                {"reagentSetId", e.reagentSetId},
                {"name", e.name},
                {"description", e.description},
                {"spellId", e.spellId},
                {"reagentItemId", items},
                {"reagentCount", counts},
                {"reagentKind", e.reagentKind},
                {"reagentKindName", wowee::pipeline::WoweeSpellReagent::reagentKindName(e.reagentKind)},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSPR: %s.wspr\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  sets    : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   spellId   kind          slots  reagents (item x count)             name\n");
    for (const auto& e : c.entries) {
        std::string slots;
        int used = 0;
        for (int s = 0; s < wowee::pipeline::WoweeSpellReagent::kMaxReagentSlots; ++s) {
            if (e.reagentItemId[s] == 0) continue;
            if (!slots.empty()) slots += ", ";
            slots += std::to_string(e.reagentItemId[s]) + " x " +
                     std::to_string(e.reagentCount[s]);
            ++used;
        }
        if (slots.empty()) slots = "(none)";
        std::printf("  %4u   %5u    %-12s    %d    %-35s  %s\n",
                    e.reagentSetId, e.spellId,
                    wowee::pipeline::WoweeSpellReagent::reagentKindName(e.reagentKind),
                    used, slots.c_str(), e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWsprExt(base);
    if (!wowee::pipeline::WoweeSpellReagentLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wspr: WSPR not found: %s.wspr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSpellReagentLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    std::vector<uint32_t> spellsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.reagentSetId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.reagentSetId == 0)
            errors.push_back(ctx + ": reagentSetId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.spellId == 0)
            errors.push_back(ctx +
                ": spellId is 0 — missing WSPL cross-ref");
        if (e.reagentKind > wowee::pipeline::WoweeSpellReagent::Tradeable) {
            errors.push_back(ctx + ": reagentKind " +
                std::to_string(e.reagentKind) + " not in 0..4");
        }
        // Per-slot checks: itemId+count must be both set or both clear
        int usedSlots = 0;
        for (int s = 0; s < wowee::pipeline::WoweeSpellReagent::kMaxReagentSlots; ++s) {
            uint32_t it = e.reagentItemId[s];
            uint32_t cnt = e.reagentCount[s];
            if (it != 0 && cnt == 0) {
                warnings.push_back(ctx +
                    ": slot " + std::to_string(s) +
                    " has itemId=" + std::to_string(it) +
                    " but count=0 — reagent will not be consumed");
            } else if (it == 0 && cnt != 0) {
                warnings.push_back(ctx +
                    ": slot " + std::to_string(s) +
                    " has count=" + std::to_string(cnt) +
                    " but itemId=0 — count is unreachable");
            }
            if (it != 0) ++usedSlots;
        }
        // SoulShard kind should reference item 6265 in slot 0.
        // Other shard ids are server-custom but the canonical
        // case is worth flagging.
        if (e.reagentKind == wowee::pipeline::WoweeSpellReagent::SoulShard &&
            e.reagentItemId[0] != 6265 && e.reagentItemId[0] != 0) {
            warnings.push_back(ctx +
                ": SoulShard kind with non-canonical reagent " +
                "id " + std::to_string(e.reagentItemId[0]) +
                " in slot 0 (canonical Soul Shard is item 6265)");
        }
        // FocusedItem kind: reagent is required to cast but
        // not consumed. Should still have an itemId set.
        if (e.reagentKind == wowee::pipeline::WoweeSpellReagent::FocusedItem &&
            usedSlots == 0) {
            warnings.push_back(ctx +
                ": FocusedItem kind with no reagent slots set " +
                "— focused-item gating has nothing to gate");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.reagentSetId) {
                errors.push_back(ctx + ": duplicate reagentSetId");
                break;
            }
        }
        idsSeen.push_back(e.reagentSetId);
        // Two reagent sets for the same spell collide —
        // engine would honor only the first.
        if (e.spellId != 0) {
            for (uint32_t prevSpell : spellsSeen) {
                if (prevSpell == e.spellId) {
                    warnings.push_back(ctx +
                        ": duplicate spellId " +
                        std::to_string(e.spellId) +
                        " — only first reagent set will be used");
                    break;
                }
            }
            spellsSeen.push_back(e.spellId);
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wspr"] = base + ".wspr";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wspr: %s.wspr\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu reagent sets, all reagentSetIds unique, all spell ids set\n",
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

bool handleSpellReagentsCatalog(int& i, int argc, char** argv,
                                int& outRc) {
    if (std::strcmp(argv[i], "--gen-spr") == 0 && i + 1 < argc) {
        outRc = handleGenMage(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-spr-warlock") == 0 && i + 1 < argc) {
        outRc = handleGenWarlock(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-spr-rez") == 0 && i + 1 < argc) {
        outRc = handleGenRez(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wspr") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wspr") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
