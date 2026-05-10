#include "cli_currency_types_catalog.hpp"
#include "cli_arg_parse.hpp"
#include "cli_box_emitter.hpp"

#include "pipeline/wowee_currency_types.hpp"
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

std::string stripWctrExt(std::string base) {
    stripExt(base, ".wctr");
    return base;
}

bool saveOrError(const wowee::pipeline::WoweeCurrencyType& c,
                 const std::string& base, const char* cmd) {
    if (!wowee::pipeline::WoweeCurrencyTypeLoader::save(c, base)) {
        std::fprintf(stderr, "%s: failed to save %s.wctr\n",
                     cmd, base.c_str());
        return false;
    }
    return true;
}

void printGenSummary(const wowee::pipeline::WoweeCurrencyType& c,
                     const std::string& base) {
    std::printf("Wrote %s.wctr\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  currencies : %zu\n", c.entries.size());
}

int handleGenPvP(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "PvPCurrencies";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWctrExt(base);
    auto c = wowee::pipeline::WoweeCurrencyTypeLoader::makePvP(name);
    if (!saveOrError(c, base, "gen-ctr")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenPvE(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "PvECurrencies";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWctrExt(base);
    auto c = wowee::pipeline::WoweeCurrencyTypeLoader::makePvE(name);
    if (!saveOrError(c, base, "gen-ctr-pve")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleGenFactionTokens(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    std::string name = "FactionTokens";
    if (parseOptArg(i, argc, argv)) name = argv[++i];
    base = stripWctrExt(base);
    auto c = wowee::pipeline::WoweeCurrencyTypeLoader::makeFactionTokens(name);
    if (!saveOrError(c, base, "gen-ctr-faction")) return 1;
    printGenSummary(c, base);
    return 0;
}

int handleInfo(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWctrExt(base);
    if (!wowee::pipeline::WoweeCurrencyTypeLoader::exists(base)) {
        std::fprintf(stderr, "WCTR not found: %s.wctr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCurrencyTypeLoader::load(base);
    if (jsonOut) {
        nlohmann::json j;
        j["wctr"] = base + ".wctr";
        j["name"] = c.name;
        j["count"] = c.entries.size();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : c.entries) {
            arr.push_back({
                {"currencyId", e.currencyId},
                {"name", e.name},
                {"description", e.description},
                {"itemId", e.itemId},
                {"maxQuantity", e.maxQuantity},
                {"maxQuantityWeekly", e.maxQuantityWeekly},
                {"categoryId", e.categoryId},
                {"currencyKind", e.currencyKind},
                {"currencyKindName", wowee::pipeline::WoweeCurrencyType::currencyKindName(e.currencyKind)},
                {"isAccountWide", e.isAccountWide != 0},
                {"iconPath", e.iconPath},
                {"iconColorRGBA", e.iconColorRGBA},
            });
        }
        j["entries"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("WCTR: %s.wctr\n", base.c_str());
    std::printf("  catalog    : %s\n", c.name.c_str());
    std::printf("  currencies : %zu\n", c.entries.size());
    if (c.entries.empty()) return 0;
    std::printf("    id      kind            item    maxQ    maxWeek  cat   acct  name\n");
    for (const auto& e : c.entries) {
        std::printf("  %7u   %-13s   %5u   %6u    %6u  %4u   %s    %s\n",
                    e.currencyId,
                    wowee::pipeline::WoweeCurrencyType::currencyKindName(e.currencyKind),
                    e.itemId,
                    e.maxQuantity, e.maxQuantityWeekly,
                    e.categoryId,
                    e.isAccountWide ? "yes" : "no ",
                    e.name.c_str());
    }
    return 0;
}

int handleValidate(int& i, int argc, char** argv) {
    std::string base = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    base = stripWctrExt(base);
    if (!wowee::pipeline::WoweeCurrencyTypeLoader::exists(base)) {
        std::fprintf(stderr,
            "validate-wctr: WCTR not found: %s.wctr\n", base.c_str());
        return 1;
    }
    auto c = wowee::pipeline::WoweeCurrencyTypeLoader::load(base);
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    if (c.entries.empty()) {
        warnings.push_back("catalog has zero entries");
    }
    std::vector<uint32_t> idsSeen;
    for (size_t k = 0; k < c.entries.size(); ++k) {
        const auto& e = c.entries[k];
        std::string ctx = "entry " + std::to_string(k) +
                          " (id=" + std::to_string(e.currencyId);
        if (!e.name.empty()) ctx += " " + e.name;
        ctx += ")";
        if (e.currencyId == 0)
            errors.push_back(ctx + ": currencyId is 0");
        if (e.name.empty())
            errors.push_back(ctx + ": name is empty");
        if (e.currencyKind > wowee::pipeline::WoweeCurrencyType::Misc) {
            errors.push_back(ctx + ": currencyKind " +
                std::to_string(e.currencyKind) + " not in 0..5");
        }
        if (e.maxQuantity != 0 &&
            e.maxQuantityWeekly != 0 &&
            e.maxQuantityWeekly > e.maxQuantity) {
            warnings.push_back(ctx +
                ": maxQuantityWeekly " +
                std::to_string(e.maxQuantityWeekly) +
                " > maxQuantity " +
                std::to_string(e.maxQuantity) +
                " — weekly cap exceeds absolute cap, "
                "weekly cap will never be reached");
        }
        // Faction tokens with no categoryId can't reference
        // a faction — break the rep gate.
        if (e.currencyKind == wowee::pipeline::WoweeCurrencyType::FactionToken &&
            e.categoryId == 0) {
            warnings.push_back(ctx +
                ": FactionToken kind with categoryId=0 — "
                "no faction is associated, rep gate will not "
                "trigger");
        }
        // Currencies with no caps at all and no item backing
        // are likely misconfigured.
        if (e.maxQuantity == 0 && e.maxQuantityWeekly == 0 &&
            e.itemId == 0 && e.iconPath.empty()) {
            warnings.push_back(ctx +
                ": no caps + no itemId + no iconPath — "
                "currency has no display data and unbounded "
                "earn rate");
        }
        for (uint32_t prev : idsSeen) {
            if (prev == e.currencyId) {
                errors.push_back(ctx + ": duplicate currencyId");
                break;
            }
        }
        idsSeen.push_back(e.currencyId);
    }
    bool ok = errors.empty();
    if (jsonOut) {
        nlohmann::json j;
        j["wctr"] = base + ".wctr";
        j["ok"] = ok;
        j["errors"] = errors;
        j["warnings"] = warnings;
        std::printf("%s\n", j.dump(2).c_str());
        return ok ? 0 : 1;
    }
    std::printf("validate-wctr: %s.wctr\n", base.c_str());
    if (ok && warnings.empty()) {
        std::printf("  OK — %zu currencies, all currencyIds unique\n",
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

bool handleCurrencyTypesCatalog(int& i, int argc, char** argv,
                                int& outRc) {
    if (std::strcmp(argv[i], "--gen-ctr") == 0 && i + 1 < argc) {
        outRc = handleGenPvP(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-ctr-pve") == 0 && i + 1 < argc) {
        outRc = handleGenPvE(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-ctr-faction") == 0 && i + 1 < argc) {
        outRc = handleGenFactionTokens(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--info-wctr") == 0 && i + 1 < argc) {
        outRc = handleInfo(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--validate-wctr") == 0 && i + 1 < argc) {
        outRc = handleValidate(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
