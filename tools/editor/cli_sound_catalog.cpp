#include "cli_sound_catalog.hpp"

#include "pipeline/wowee_sound.hpp"
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

std::string stripWsndExt(std::string base) {
    if (base.size() >= 5 && base.substr(base.size() - 5) == ".wsnd")
        base = base.substr(0, base.size() - 5);
    return base;
}

void appendFlagsStr(std::string& s, uint32_t flags) {
    if (flags & wowee::pipeline::WoweeSound::Loop)   s += "loop ";
    if (flags & wowee::pipeline::WoweeSound::Is3D)   s += "3d ";
    if (flags & wowee::pipeline::WoweeSound::Stream) s += "stream ";
    if (s.empty()) s = "-";
    else if (s.back() == ' ') s.pop_back();
}

bool saveOrError(const wowee::pipeline::WoweeSound& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeSoundLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wsnd\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeSound& c,
                     const std::string& base) {
    std::printf("Wrote %s.wsnd\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  entries : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterCatalog";
    if (i + 1 < argc && argv[i + 1][0] != '-') name = argv[++i];
    base = stripWsndExt(base);
    auto c = wowee::pipeline::WoweeSoundLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-sound-catalog")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenAmbient(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "AmbientCatalog";
    if (i + 1 < argc && argv[i + 1][0] != '-') name = argv[++i];
    base = stripWsndExt(base);
    auto c = wowee::pipeline::WoweeSoundLoader::makeAmbient(name);
    if (!saveOrError(c, base, "gen-sound-catalog-ambient")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenTavern(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "TavernCatalog";
    if (i + 1 < argc && argv[i + 1][0] != '-') name = argv[++i];
    base = stripWsndExt(base);
    auto c = wowee::pipeline::WoweeSoundLoader::makeTavern(name);
    if (!saveOrError(c, base, "gen-sound-catalog-tavern")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = (i + 1 < argc &&
                    std::strcmp(argv[i + 1], "--json") == 0);
    if (jsonOut) i++;
    base = stripWsndExt(base);
    if (!wowee::pipeline::WoweeSoundLoader::exists(base)) {
        std::fprintf(stderr, "WSND not found: %s.wsnd\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSoundLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wsnd"] = base + ".wsnd";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            std::string fs;
            appendFlagsStr(fs, e.flags);
            arr.push_back({
                {"soundId", e.soundId},
                {"kind", e.kind},
                {"kindName", wowee::pipeline::WoweeSound::kindName(e.kind)},
                {"flags", e.flags},
                {"flagsStr", fs},
                {"volume", e.volume},
                {"minDistance", e.minDistance},
                {"maxDistance", e.maxDistance},
                {"filePath", e.filePath},
                {"label", e.label},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WSND: %s.wsnd\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  entries : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id  kind     flags         vol    min    max  label / file\n");
    for (const auto& e : c.entries) {
        std::string fs;
        appendFlagsStr(fs, e.flags);
        std::printf("  %4u  %-7s  %-12s  %4.2f  %5.1f  %5.1f  %s\n",
                    e.soundId,
                    wowee::pipeline::WoweeSound::kindName(e.kind),
                    fs.c_str(),
                    e.volume, e.minDistance, e.maxDistance,
                    e.label.empty() ? e.filePath.c_str()
                                    : (e.label + "  (" + e.filePath + ")").c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = (i + 1 < argc &&
                    std::strcmp(argv[i + 1], "--json") == 0);
    if (jsonOut) i++;
    base = stripWsndExt(base);
    if (!wowee::pipeline::WoweeSoundLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wsnd: WSND not found: %s.wsnd\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeSoundLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    // Per-entry checks plus a duplicate-soundId scan.
    std::vector<uint32_t> idsSeen;
    idsSeen.reserve(c.entries.size());
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.soundId) + ")";
        if (e.kind > wowee::pipeline::WoweeSound::Combat) {
            errors.push_back(ctx + ": kind " + std::to_string(e.kind) +
                             " not in known range 0..6");
        }
        // Reject NaN/inf early — these crash audio engines.
        if (!std::isfinite(e.volume) ||
            !std::isfinite(e.minDistance) ||
            !std::isfinite(e.maxDistance)) {
            errors.push_back(ctx + ": volume/min/max distance not finite");
        }
        if (e.volume < 0.0f || e.volume > 4.0f) {
            warnings.push_back(ctx + ": volume " +
                std::to_string(e.volume) +
                " outside typical 0..4 range");
        }
        // 3D sounds need min < max; non-3D sounds usually have
        // both at 0 (positional fields ignored at runtime).
        if (e.flags & wowee::pipeline::WoweeSound::Is3D) {
            if (e.minDistance < 0 || e.maxDistance <= e.minDistance) {
                errors.push_back(ctx +
                    ": 3d sound needs minDistance >= 0 and "
                    "maxDistance > minDistance");
            }
        }
        if (e.filePath.empty()) {
            errors.push_back(ctx + ": filePath is empty");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.soundId) {
                errors.push_back(ctx +
                    ": soundId already used by an earlier entry");
                break;
            }
        }
        idsSeen.push_back(e.soundId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wsnd"] = base + ".wsnd";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wsnd: %s.wsnd\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu entries, all sound IDs unique\n",
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

bool handleSoundCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-sound-catalog") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-sound-catalog-ambient") == 0 && i + 1 < argc) {
        outRc = handleGenAmbient(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-sound-catalog-tavern") == 0 && i + 1 < argc) {
        outRc = handleGenTavern(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wsnd") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wsnd") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
