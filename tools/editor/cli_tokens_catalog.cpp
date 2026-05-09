#include "cli_tokens_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_tokens.hpp"
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

std::string stripWtknExt(std::string base) {
    stripExt(base, ".wtkn");
    return base;
}

void appendTknFlagsStr(std::string& s, uint32_t flags) {
    if (flags & wowee::pipeline::WoweeToken::AccountWide)       s += "account ";
    if (flags & wowee::pipeline::WoweeToken::Tradeable)         s += "trade ";
    if (flags & wowee::pipeline::WoweeToken::HiddenUntilEarned) s += "hidden ";
    if (flags & wowee::pipeline::WoweeToken::ResetsOnLogout)    s += "resets ";
    if (flags & wowee::pipeline::WoweeToken::ConvertsToGold)    s += "to-gold ";
    if (s.empty()) s = "-";
    else if (s.back() == ' ') s.pop_back();
}

bool saveOrError(const wowee::pipeline::WoweeToken& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeTokenLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wtkn\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeToken& c,
                     const std::string& base) {
    std::printf("Wrote %s.wtkn\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  tokens  : %zu\n", c.entries.size());
}

int handleGenStarter(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "StarterTokens";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtknExt(base);
    auto c = wowee::pipeline::WoweeTokenLoader::makeStarter(name);
    if (!saveOrError(c, base, "gen-tokens")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenPvp(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "PvpTokens";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtknExt(base);
    auto c = wowee::pipeline::WoweeTokenLoader::makePvp(name);
    if (!saveOrError(c, base, "gen-tokens-pvp")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenSeasonal(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "SeasonalTokens";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWtknExt(base);
    auto c = wowee::pipeline::WoweeTokenLoader::makeSeasonal(name);
    if (!saveOrError(c, base, "gen-tokens-seasonal")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWtknExt(base);
    if (!wowee::pipeline::WoweeTokenLoader::exists(base)) {
        std::fprintf(stderr, "WTKN not found: %s.wtkn\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeTokenLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wtkn"] = base + ".wtkn";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            std::string fs;
            appendTknFlagsStr(fs, e.flags);
            arr.push_back({
                {"tokenId", e.tokenId},
                {"name", e.name},
                {"description", e.description},
                {"iconPath", e.iconPath},
                {"category", e.category},
                {"categoryName", wowee::pipeline::WoweeToken::categoryName(e.category)},
                {"maxBalance", e.maxBalance},
                {"weeklyCap", e.weeklyCap},
                {"flags", e.flags},
                {"flagsStr", fs},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WTKN: %s.wtkn\n", base.c_str());
    std::printf("  catalog : %s\n", c.name.c_str());
    std::printf("  tokens  : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id   category   maxBal      weekly   flags         name\n");
    for (const auto& e : c.entries) {
        std::string fs;
        appendTknFlagsStr(fs, e.flags);
        std::printf("  %4u   %-9s  %7u    %5u    %-12s  %s\n",
                    e.tokenId,
                    wowee::pipeline::WoweeToken::categoryName(e.category),
                    e.maxBalance, e.weeklyCap, fs.c_str(),
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWtknExt(base);
    if (!wowee::pipeline::WoweeTokenLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wtkn: WTKN not found: %s.wtkn\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeTokenLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.tokenId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.tokenId == 0) errors.push_back(ctx + ": tokenId is 0");
        if (e.name.empty()) errors.push_back(ctx + ": name is empty");
        if (e.category > wowee::pipeline::WoweeToken::Holiday) {
            errors.push_back(ctx + ": category " +
                std::to_string(e.category) + " not in 0..5");
        }
        if (e.weeklyCap > 0 && e.maxBalance > 0 &&
            e.weeklyCap > e.maxBalance) {
            warnings.push_back(ctx +
                ": weeklyCap exceeds maxBalance (cap is unreachable)");
        }
        if ((e.flags & wowee::pipeline::WoweeToken::ResetsOnLogout) &&
            (e.flags & wowee::pipeline::WoweeToken::AccountWide)) {
            errors.push_back(ctx +
                ": ResetsOnLogout and AccountWide both set (incoherent)");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.tokenId) {
                errors.push_back(ctx + ": duplicate tokenId");
                break;
            }
        }
        idsSeen.push_back(e.tokenId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wtkn"] = base + ".wtkn";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wtkn: %s.wtkn\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu tokens, all tokenIds unique\n",
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

bool handleTokensCatalog(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-tokens") == 0 && i + 1 < argc) {
        outRc = handleGenStarter(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-tokens-pvp") == 0 && i + 1 < argc) {
        outRc = handleGenPvp(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-tokens-seasonal") == 0 && i + 1 < argc) {
        outRc = handleGenSeasonal(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wtkn") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wtkn") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
