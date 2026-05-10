#include "cli_localization_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_localization.hpp"
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

std::string stripWlanExt(std::string base) {
    stripExt(base, ".wlan");
    return base;
}

const char* languageCodeName(uint8_t l) {
    using L = wowee::pipeline::WoweeLocalization;
    switch (l) {
        case L::enUS:    return "enUS";
        case L::enGB:    return "enGB";
        case L::deDE:    return "deDE";
        case L::esES:    return "esES";
        case L::frFR:    return "frFR";
        case L::itIT:    return "itIT";
        case L::koKR:    return "koKR";
        case L::ptBR:    return "ptBR";
        case L::ruRU:    return "ruRU";
        case L::zhCN:    return "zhCN";
        case L::zhTW:    return "zhTW";
        case L::Unknown: return "Unknown";
        default:         return "?";
    }
}

const char* namespaceName(uint8_t n) {
    using L = wowee::pipeline::WoweeLocalization;
    switch (n) {
        case L::UI:       return "ui";
        case L::Quest:    return "quest";
        case L::Item:     return "item";
        case L::Spell:    return "spell";
        case L::Creature: return "creature";
        case L::Tooltip:  return "tooltip";
        case L::Gossip:   return "gossip";
        case L::System:   return "system";
        default:          return "unknown";
    }
}

bool saveOrError(const wowee::pipeline::WoweeLocalization& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeLocalizationLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wlan\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeLocalization& c,
                     const std::string& base) {
    std::printf("Wrote %s.wlan\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  strings : %zu\n", c.entries.size());
}

int handleGenUI(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "UIBasicsLocalization";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWlanExt(base);
    auto c = wowee::pipeline::WoweeLocalizationLoader::makeUIBasics(name);
    if (!saveOrError(c, base, "gen-lan")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenQuest(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "QuestSampleLocalization";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWlanExt(base);
    auto c = wowee::pipeline::WoweeLocalizationLoader::makeQuestSample(name);
    if (!saveOrError(c, base, "gen-lan-quest")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenTooltip(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "TooltipSetLocalization";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWlanExt(base);
    auto c = wowee::pipeline::WoweeLocalizationLoader::makeTooltipSet(name);
    if (!saveOrError(c, base, "gen-lan-tooltip")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWlanExt(base);
    if (!wowee::pipeline::WoweeLocalizationLoader::exists(base)) {
        std::fprintf(stderr, "WLAN not found: %s.wlan\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeLocalizationLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wlan"] = base + ".wlan";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"stringId", e.stringId},
                {"name", e.name},
                {"description", e.description},
                {"languageCode", e.languageCode},
                {"languageCodeName",
                    languageCodeName(e.languageCode)},
                {"namespace", e.namespace_},
                {"namespaceName", namespaceName(e.namespace_)},
                {"originalKey", e.originalKey},
                {"localizedText", e.localizedText},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WLAN: %s.wlan\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  strings : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   lang  ns         key                       text\n");
    for (const auto& e : c.entries) {
        std::printf("  %4u   %-4s  %-9s  %-25s  %s\n",
                    e.stringId,
                    languageCodeName(e.languageCode),
                    namespaceName(e.namespace_),
                    e.originalKey.c_str(),
                    e.localizedText.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWlanExt(base);
    if (!wowee::pipeline::WoweeLocalizationLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wlan: WLAN not found: %s.wlan\n",
            base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeLocalizationLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::set<uint32_t> idsSeen;
    // Per-(originalKey, languageCode, namespace_) triple
    // uniqueness — two entries with all three matching
    // would tie at runtime when the locale-aware text
    // layer looks up an override.
    std::set<std::string> tripleSeen;
    auto tripleKey = [](const std::string& key, uint8_t lang,
                        uint8_t ns) {
        return std::to_string(lang) + "|" +
               std::to_string(ns) + "|" + key;
    };
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.stringId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.stringId == 0)
            errors.push_back(ctx + ": stringId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.languageCode > 10 && e.languageCode != 255) {
            errors.push_back(ctx + ": languageCode " +
                std::to_string(e.languageCode) +
                " out of range (must be 0..10 or 255 "
                "Unknown)");
        }
        if (e.namespace_ > 7) {
            errors.push_back(ctx + ": namespace " +
                std::to_string(e.namespace_) +
                " out of range (must be 0..7)");
        }
        if (e.originalKey.empty()) {
            errors.push_back(ctx +
                ": originalKey is empty — locale-aware "
                "text layer has nothing to look up");
        }
        if (e.localizedText.empty()) {
            warnings.push_back(ctx +
                ": localizedText is empty — override "
                "would render blank, possibly worse than "
                "falling through to default");
        }
        // Triple uniqueness check.
        if (!e.originalKey.empty()) {
            std::string key = tripleKey(e.originalKey,
                                          e.languageCode,
                                          e.namespace_);
            if (!tripleSeen.insert(key).second) {
                errors.push_back(ctx +
                    ": (originalKey='" + e.originalKey +
                    "', languageCode=" +
                    std::string(languageCodeName(e.languageCode)) +
                    ", namespace=" +
                    std::string(namespaceName(e.namespace_)) +
                    ") triple already bound by another "
                    "entry — locale lookup would tie "
                    "non-deterministically");
            }
        }
        if (!idsSeen.insert(e.stringId).second) {
            errors.push_back(ctx + ": duplicate stringId");
        }
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wlan"] = base + ".wlan";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wlan: %s.wlan\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu strings, all stringIds + "
                    "(key,lang,ns) triples unique\n",
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

bool handleLocalizationCatalog(int& i, int argc, char** argv,
                                int& outRc) {
    if (std::strcmp(argv[i], "--gen-lan") == 0 && i + 1 < argc) {
        outRc = handleGenUI(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-lan-quest") == 0 &&
        i + 1 < argc) {
        outRc = handleGenQuest(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-lan-tooltip") == 0 &&
        i + 1 < argc) {
        outRc = handleGenTooltip(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wlan") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wlan") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
