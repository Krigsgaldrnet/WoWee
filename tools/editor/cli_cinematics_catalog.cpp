#include "cli_cinematics_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_cinematics.hpp"
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

std::string stripWcmsExt(std::string base) {
    stripExt(base, ".wcms");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeCinematic& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeCinematicLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wcms\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeCinematic& c,
                     const std::string& base) {
    std::printf("Wrote %s.wcms\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  cinematics : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterCinematics";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcmsExt(base);
    auto c = wowee::pipeline::WoweeCinematicLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-cinematics")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenIntros(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "ClassIntros";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcmsExt(base);
    auto c = wowee::pipeline::WoweeCinematicLoader::makeIntros(name);
    if (!saveOrError(c, base, "gen-cinematics-intros")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenQuests(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "QuestCinematics";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWcmsExt(base);
    auto c = wowee::pipeline::WoweeCinematicLoader::makeQuestCinematics(name);
    if (!saveOrError(c, base, "gen-cinematics-quests")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcmsExt(base);
    if (!wowee::pipeline::WoweeCinematicLoader::exists(base)) {
        std::fprintf(stderr, "WCMS not found: %s.wcms\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCinematicLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wcms"] = base + ".wcms";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"cinematicId", e.cinematicId},
                {"name", e.name},
                {"description", e.description},
                {"mediaPath", e.mediaPath},
                {"kind", e.kind},
                {"kindName", wowee::pipeline::WoweeCinematic::kindName(e.kind)},
                {"triggerKind", e.triggerKind},
                {"triggerKindName", wowee::pipeline::WoweeCinematic::triggerKindName(e.triggerKind)},
                {"triggerTargetId", e.triggerTargetId},
                {"durationSeconds", e.durationSeconds},
                {"skippable", e.skippable},
                {"soundtrackId", e.soundtrackId},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WCMS: %s.wcms\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  cinematics : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    kind        trigger          target  dur  skip  snd  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-10s  %-15s  %5u   %3us   %u    %3u  %s\n",
                    e.cinematicId,
                    wowee::pipeline::WoweeCinematic::kindName(e.kind),
                    wowee::pipeline::WoweeCinematic::triggerKindName(e.triggerKind),
                    e.triggerTargetId,
                    e.durationSeconds,
                    e.skippable,
                    e.soundtrackId,
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWcmsExt(base);
    if (!wowee::pipeline::WoweeCinematicLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wcms: WCMS not found: %s.wcms\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCinematicLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.cinematicId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.cinematicId == 0)
            errors.push_back(ctx + ": cinematicId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.mediaPath.empty())
            errors.push_back(ctx + ": mediaPath is empty");
        if (e.kind > wowee::pipeline::WoweeCinematic::Slideshow) {
            errors.push_back(ctx + ": kind " +
                std::to_string(e.kind) + " not in 0..4");
        }
        if (e.triggerKind > wowee::pipeline::WoweeCinematic::LevelUp) {
            errors.push_back(ctx + ": triggerKind " +
                std::to_string(e.triggerKind) + " not in 0..8");
        }
        // Triggers other than Manual/Login require a non-zero
        // target id (questId, mapId, classId, achievementId etc).
        if (e.triggerKind != wowee::pipeline::WoweeCinematic::Manual &&
            e.triggerKind != wowee::pipeline::WoweeCinematic::Login &&
            e.triggerKind != wowee::pipeline::WoweeCinematic::LevelUp &&
            e.triggerTargetId == 0) {
            errors.push_back(ctx +
                ": triggerKind " +
                wowee::pipeline::WoweeCinematic::triggerKindName(e.triggerKind) +
                " requires a non-zero triggerTargetId");
        }
        if (e.durationSeconds == 0) {
            warnings.push_back(ctx + ": durationSeconds=0 "
                "(cinematic will be skipped instantly)");
        }
        if (e.kind == wowee::pipeline::WoweeCinematic::PreRenderedVideo &&
            e.skippable == 0) {
            warnings.push_back(ctx + ": pre-rendered video is "
                "non-skippable (player can't escape)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.cinematicId) {
                errors.push_back(ctx + ": duplicate cinematicId");
                break;
            }
        }
        idsSeen.push_back(e.cinematicId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wcms"] = base + ".wcms";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wcms: %s.wcms\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu cinematics, all cinematicIds unique\n",
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

bool handleCinematicsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-cinematics") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cinematics-intros") == 0 && i + 1 < argc) {
        outRc = handleGenIntros(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-cinematics-quests") == 0 && i + 1 < argc) {
        outRc = handleGenQuests(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wcms") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wcms") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
