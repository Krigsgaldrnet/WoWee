#include "cli_animations_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_animations.hpp"
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

std::string stripWaniExt(std::string base) {
    stripExt(base, ".wani");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeAnimation& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeAnimationLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wani\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeAnimation& c,
                     const std::string& base) {
    std::printf("Wrote %s.wani\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  animations : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterAnimations";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWaniExt(base);
    auto c = wowee::pipeline::WoweeAnimationLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-animations")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenCombat(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "CombatAnimations";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWaniExt(base);
    auto c = wowee::pipeline::WoweeAnimationLoader::makeCombat(name);
    if (!saveOrError(c, base, "gen-animations-combat")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenMovement(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "MovementAnimations";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWaniExt(base);
    auto c = wowee::pipeline::WoweeAnimationLoader::makeMovement(name);
    if (!saveOrError(c, base, "gen-animations-movement")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWaniExt(base);
    if (!wowee::pipeline::WoweeAnimationLoader::exists(base)) {
        std::fprintf(stderr, "WANI not found: %s.wani\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeAnimationLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wani"] = base + ".wani";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"animationId", e.animationId},
                {"name", e.name},
                {"description", e.description},
                {"fallbackId", e.fallbackId},
                {"behaviorId", e.behaviorId},
                {"behaviorTier", e.behaviorTier},
                {"behaviorTierName", wowee::pipeline::WoweeAnimation::behaviorTierName(e.behaviorTier)},
                {"flags", e.flags},
                {"weaponFlags", e.weaponFlags},
                {"loopDurationMs", e.loopDurationMs},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WANI: %s.wani\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  animations : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id    tier        flags       weapons     dur(ms)  fallback  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u    %-9s   0x%08x  0x%08x   %5u     %4u    %s\n",
                    e.animationId,
                    wowee::pipeline::WoweeAnimation::behaviorTierName(e.behaviorTier),
                    e.flags, e.weaponFlags, e.loopDurationMs,
                    e.fallbackId, e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWaniExt(base);
    if (!wowee::pipeline::WoweeAnimationLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wani: WANI not found: %s.wani\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeAnimationLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    // First pass — collect all animationIds for fallback
    // resolution.
    std::vector<uint32_t> allIds;
    for (const auto& e : c.entries) allIds.push_back(e.animationId);
    auto idExists = [&](uint32_t id) {
        for (uint32_t a : allIds) if (a == id) return true;
        return false;
    };
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.animationId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.behaviorTier > wowee::pipeline::WoweeAnimation::Swimming) {
            errors.push_back(ctx + ": behaviorTier " +
                std::to_string(e.behaviorTier) + " not in 0..4");
        }
        if (e.weaponFlags == 0) {
            warnings.push_back(ctx +
                ": weaponFlags=0 (animation never selected for any weapon)");
        }
        // fallbackId == animationId would create an infinite
        // loop. fallback to id 0 (Stand) is conventional and
        // always valid even if Stand isn't in this catalog.
        if (e.fallbackId == e.animationId &&
            e.animationId != 0) {
            errors.push_back(ctx +
                ": fallbackId equals animationId (infinite loop)");
        }
        if (e.fallbackId != 0 && !idExists(e.fallbackId)) {
            warnings.push_back(ctx + ": fallbackId=" +
                std::to_string(e.fallbackId) +
                " not found in this catalog (resolved at runtime)");
        }
        // Looped animations must have a non-zero duration —
        // otherwise the renderer divides by zero stepping the
        // animation cursor.
        if ((e.flags & wowee::pipeline::WoweeAnimation::kFlagLooped) &&
            e.loopDurationMs == 0) {
            errors.push_back(ctx +
                ": kFlagLooped set but loopDurationMs=0 "
                "(animation cursor would divide by zero)");
        }
        // Mutually exclusive — Looped + OneShot is contradictory.
        if ((e.flags & wowee::pipeline::WoweeAnimation::kFlagLooped) &&
            (e.flags & wowee::pipeline::WoweeAnimation::kFlagOneShot)) {
            errors.push_back(ctx +
                ": both kFlagLooped and kFlagOneShot set "
                "(mutually exclusive)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.animationId) {
                errors.push_back(ctx + ": duplicate animationId");
                break;
            }
        }
        idsSeen.push_back(e.animationId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wani"] = base + ".wani";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wani: %s.wani\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu animations, all animationIds unique\n",
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

bool handleAnimationsCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-animations") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-animations-combat") == 0 && i + 1 < argc) {
        outRc = handleGenCombat(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-animations-movement") == 0 && i + 1 < argc) {
        outRc = handleGenMovement(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wani") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wani") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
