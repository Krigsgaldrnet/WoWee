#include "cli_objects_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_objects.hpp"
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

std::string stripWgotExt(std::string base) {
    stripExt(base, ".wgot");
    return base;
}

void appendObjFlagsStr(std::string& s, uint32_t flags) {
    if (flags & wowee::pipeline::WoweeGameObject::Disabled)        s += "disabled ";
    if (flags & wowee::pipeline::WoweeGameObject::ScriptOnly)      s += "script-only ";
    if (flags & wowee::pipeline::WoweeGameObject::UsableFromMount) s += "from-mount ";
    if (flags & wowee::pipeline::WoweeGameObject::Despawn)         s += "despawn ";
    if (flags & wowee::pipeline::WoweeGameObject::Frozen)          s += "frozen ";
    if (flags & wowee::pipeline::WoweeGameObject::QuestGated)      s += "quest-gated ";
    if (s.empty()) s = "-";
    else if (s.back() == ' ') s.pop_back();
}

bool saveOrError(const wowee::pipeline::WoweeGameObject& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeGameObjectLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wgot\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeGameObject& c,
                     const std::string& base) {
    std::printf("Wrote %s.wgot\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  objects : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterObjects";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgotExt(base);
    auto c = wowee::pipeline::WoweeGameObjectLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-objects")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenDungeon(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "DungeonObjects";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgotExt(base);
    auto c = wowee::pipeline::WoweeGameObjectLoader::makeDungeon(name);
    if (!saveOrError(c, base, "gen-objects-dungeon")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenGather(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "GatheringNodes";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWgotExt(base);
    auto c = wowee::pipeline::WoweeGameObjectLoader::makeGather(name);
    if (!saveOrError(c, base, "gen-objects-gather")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgotExt(base);
    if (!wowee::pipeline::WoweeGameObjectLoader::exists(base)) {
        std::fprintf(stderr, "WGOT not found: %s.wgot\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGameObjectLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wgot"] = base + ".wgot";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            std::string fs;
            appendObjFlagsStr(fs, e.flags);
            arr.push_back({
                {"objectId", e.objectId},
                {"displayId", e.displayId},
                {"name", e.name},
                {"typeId", e.typeId},
                {"typeName", wowee::pipeline::WoweeGameObject::typeName(e.typeId)},
                {"size", e.size},
                {"castBarCaption", e.castBarCaption},
                {"requiredSkill", e.requiredSkill},
                {"requiredSkillValue", e.requiredSkillValue},
                {"lockId", e.lockId},
                {"lootTableId", e.lootTableId},
                {"minOpenTimeMs", e.minOpenTimeMs},
                {"maxOpenTimeMs", e.maxOpenTimeMs},
                {"flags", e.flags},
                {"flagsStr", fs},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WGOT: %s.wgot\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  objects : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   type           lock  loot   skill            name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u  %-13s  %4u  %4u   %4u/%-3u    %s\n",
                    e.objectId,
                    wowee::pipeline::WoweeGameObject::typeName(e.typeId),
                    e.lockId, e.lootTableId,
                    e.requiredSkill, e.requiredSkillValue,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWgotExt(base);
    if (!wowee::pipeline::WoweeGameObjectLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wgot: WGOT not found: %s.wgot\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeGameObjectLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    idsSeen.reserve(c.entries.size());
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.objectId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.objectId == 0) {
            errors.push_back(ctx + ": objectId is 0");
        }
        if (e.size <= 0.0f) {
            errors.push_back(ctx + ": size <= 0");
        }
        if (e.minOpenTimeMs > e.maxOpenTimeMs) {
            errors.push_back(ctx + ": minOpenTimeMs > maxOpenTimeMs");
        }
        // Gathering nodes need a skill requirement to be useful.
        if ((e.typeId == wowee::pipeline::WoweeGameObject::HerbNode ||
             e.typeId == wowee::pipeline::WoweeGameObject::MineralNode ||
             e.typeId == wowee::pipeline::WoweeGameObject::FishingNode) &&
            e.requiredSkill == 0) {
            warnings.push_back(ctx +
                ": gathering node has no required skill (anyone can harvest)");
        }
        // Chest with no loot table is rare but possible (event-spawn
        // chests fill via script).
        if (e.typeId == wowee::pipeline::WoweeGameObject::Chest &&
            e.lootTableId == 0) {
            warnings.push_back(ctx +
                ": chest has no lootTableId (script must populate)");
        }
        // requiredSkillValue without requiredSkill is incoherent.
        if (e.requiredSkill == 0 && e.requiredSkillValue > 0) {
            errors.push_back(ctx +
                ": requiredSkillValue > 0 but requiredSkill is 0");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.objectId) {
                errors.push_back(ctx + ": duplicate objectId");
                break;
            }
        }
        idsSeen.push_back(e.objectId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wgot"] = base + ".wgot";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wgot: %s.wgot\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu objects, all objectIds unique\n",
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

bool handleObjectsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-objects") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-objects-dungeon") == 0 && i + 1 < argc) {
        outRc = handleGenDungeon(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-objects-gather") == 0 && i + 1 < argc) {
        outRc = handleGenGather(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wgot") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wgot") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
