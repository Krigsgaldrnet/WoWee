#include "cli_audits.hpp"
#include "cli_weld.hpp"

#include "pipeline/wowee_model.hpp"
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

// Walk every direct subdirectory of <projectDir> that contains a
// zone.json. Used by both --validate-project-packs and
// --info-project-deps to enumerate zones.
std::vector<std::string> enumerateZones(const std::string& projectDir) {
    std::vector<std::string> zones;
    namespace fs = std::filesystem;
    for (const auto& entry : fs::directory_iterator(projectDir)) {
        if (!entry.is_directory()) continue;
        if (!fs::exists(entry.path() / "zone.json")) continue;
        zones.push_back(entry.path().string());
    }
    std::sort(zones.begin(), zones.end());
    return zones;
}

// Run --<perZoneFlag> for each zone via subprocess and report
// per-zone PASS/FAIL plus a project rollup. Generic so both
// audits share it.
int runPerZoneAudit(const std::string& projectDir,
                    const std::string& cmdName,
                    const std::string& perZoneFlag,
                    const std::string& selfPath,
                    const std::string& failHint) {
    namespace fs = std::filesystem;
    if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
        std::fprintf(stderr,
            "%s: %s is not a directory\n",
            cmdName.c_str(), projectDir.c_str());
        return 1;
    }
    auto zones = enumerateZones(projectDir);
    if (zones.empty()) {
        std::fprintf(stderr,
            "%s: %s contains no zones\n",
            cmdName.c_str(), projectDir.c_str());
        return 1;
    }
    int passed = 0, failed = 0;
    std::printf("%s: %s\n", cmdName.c_str(), projectDir.c_str());
    std::printf("  zones: %zu\n\n", zones.size());
    for (const auto& z : zones) {
        std::string cmd = "\"" + selfPath + "\" " + perZoneFlag + " \"" +
                          z + "\" > /dev/null 2>&1";
        int rc = std::system(cmd.c_str());
        std::string name = fs::path(z).filename().string();
        if (rc == 0) {
            ++passed;
            std::printf("  PASS  %s\n", name.c_str());
        } else {
            ++failed;
            std::printf("  FAIL  %s\n", name.c_str());
        }
    }
    std::printf("\n  Total: %d passed, %d failed\n", passed, failed);
    if (failed == 0) {
        std::printf("  PROJECT PASS\n");
    } else {
        std::printf("  PROJECT FAIL%s%s\n",
                    failHint.empty() ? "" : " — ",
                    failHint.c_str());
    }
    return failed == 0 ? 0 : 1;
}

int handleZoneDeps(int& i, int argc, char** argv) {
    // Broken-reference audit: walk every WOM in the zone, collect
    // its texturePaths, and check whether each path resolves to a
    // real file via 4 candidate locations (as-is, relative to
    // zone, in textures/, alongside the WOM).
    std::string zoneDir = argv[++i];
    bool jsonOut = (i + 1 < argc &&
                    std::strcmp(argv[i + 1], "--json") == 0);
    if (jsonOut) i++;
    namespace fs = std::filesystem;
    if (!fs::exists(zoneDir + "/zone.json")) {
        std::fprintf(stderr,
            "info-zone-deps: %s has no zone.json\n", zoneDir.c_str());
        return 1;
    }
    struct DepRef {
        std::string womPath;
        std::string texPath;
        bool exists;
    };
    std::vector<DepRef> refs;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".wom") continue;
        std::string womRel = fs::relative(e.path(), zoneDir).string();
        std::string base = e.path().string();
        base = base.substr(0, base.size() - 4);
        auto wom = wowee::pipeline::WoweeModelLoader::load(base);
        for (const auto& tp : wom.texturePaths) {
            if (tp.empty()) continue;
            bool found = false;
            fs::path candidates[4] = {
                fs::path(tp),
                fs::path(zoneDir) / tp,
                fs::path(zoneDir) / "textures" / fs::path(tp).filename(),
                e.path().parent_path() / fs::path(tp).filename(),
            };
            for (const auto& c : candidates) {
                if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) {
                    found = true;
                    break;
                }
            }
            refs.push_back({womRel, tp, found});
        }
    }
    std::sort(refs.begin(), refs.end(),
              [](const DepRef& a, const DepRef& b) {
                  if (a.exists != b.exists) return !a.exists;
                  if (a.womPath != b.womPath) return a.womPath < b.womPath;
                  return a.texPath < b.texPath;
              });
    int total = static_cast<int>(refs.size());
    int missing = 0;
    for (const auto& r : refs) if (!r.exists) ++missing;
    if (jsonOut) {
        nlohmann::json j;
        j["zone"] = zoneDir;
        j["totalRefs"] = total;
        j["missingRefs"] = missing;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : refs) {
            arr.push_back({
                {"wom", r.womPath},
                {"texture", r.texPath},
                {"exists", r.exists},
            });
        }
        j["refs"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
        return missing == 0 ? 0 : 1;
    }
    std::printf("Zone deps: %s\n", zoneDir.c_str());
    std::printf("  total refs   : %d\n", total);
    std::printf("  missing refs : %d\n", missing);
    if (refs.empty()) {
        std::printf("  *no texture references in any WOM*\n");
        return 0;
    }
    std::printf("\n  exists  WOM                                 texture\n");
    for (const auto& r : refs) {
        std::printf("  %-6s  %-35s  %s\n",
                    r.exists ? "yes" : "NO",
                    r.womPath.c_str(),
                    r.texPath.c_str());
    }
    std::printf("\n  %s\n", missing == 0
        ? "PASS — all texture references resolve"
        : "FAIL — missing references above");
    return missing == 0 ? 0 : 1;
}

int handleValidateZonePack(int& i, int argc, char** argv) {
    // Audit a zone's open-format asset pack. Reports counts and
    // total bytes per category (textures/, meshes/, audio/) plus
    // any malformed WOMs or invalid WAVs. Exit code 1 if any
    // check fails — useful in CI to gate that gen-zone-starter-pack
    // output is healthy.
    std::string zoneDir = argv[++i];
    bool jsonOut = (i + 1 < argc &&
                    std::strcmp(argv[i + 1], "--json") == 0);
    if (jsonOut) i++;
    namespace fs = std::filesystem;
    if (!fs::exists(zoneDir + "/zone.json")) {
        std::fprintf(stderr,
            "validate-zone-pack: %s has no zone.json\n", zoneDir.c_str());
        return 1;
    }
    struct CatStats {
        int count = 0;
        uint64_t bytes = 0;
        int invalid = 0;
        std::vector<std::string> invalidPaths;
    };
    CatStats tex, mesh, audio;
    std::error_code ec;
    // Textures: PNGs under textures/ (8-byte signature check)
    fs::path texDir = fs::path(zoneDir) / "textures";
    if (fs::exists(texDir)) {
        for (const auto& e : fs::recursive_directory_iterator(texDir, ec)) {
            if (!e.is_regular_file()) continue;
            if (e.path().extension() != ".png") continue;
            tex.count++;
            tex.bytes += e.file_size();
            FILE* f = std::fopen(e.path().c_str(), "rb");
            if (f) {
                unsigned char sig[8];
                bool ok = (std::fread(sig, 1, 8, f) == 8 &&
                           sig[0] == 0x89 && sig[1] == 'P' &&
                           sig[2] == 'N' && sig[3] == 'G');
                std::fclose(f);
                if (!ok) {
                    tex.invalid++;
                    tex.invalidPaths.push_back(
                        fs::relative(e.path(), zoneDir).string());
                }
            }
        }
    }
    // Meshes: WOMs under meshes/ (load & sanity check)
    fs::path meshDir = fs::path(zoneDir) / "meshes";
    if (fs::exists(meshDir)) {
        for (const auto& e : fs::recursive_directory_iterator(meshDir, ec)) {
            if (!e.is_regular_file()) continue;
            if (e.path().extension() != ".wom") continue;
            mesh.count++;
            mesh.bytes += e.file_size();
            std::string base = e.path().string();
            base = base.substr(0, base.size() - 4);
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            if (wom.vertices.empty() || wom.indices.empty() ||
                wom.batches.empty()) {
                mesh.invalid++;
                mesh.invalidPaths.push_back(
                    fs::relative(e.path(), zoneDir).string());
            }
        }
    }
    // Audio: WAVs under audio/ (RIFF header check)
    fs::path audDir = fs::path(zoneDir) / "audio";
    if (fs::exists(audDir)) {
        for (const auto& e : fs::recursive_directory_iterator(audDir, ec)) {
            if (!e.is_regular_file()) continue;
            if (e.path().extension() != ".wav") continue;
            audio.count++;
            audio.bytes += e.file_size();
            FILE* f = std::fopen(e.path().c_str(), "rb");
            if (f) {
                char hdr[12];
                bool ok = (std::fread(hdr, 1, 12, f) == 12 &&
                           std::memcmp(hdr, "RIFF", 4) == 0 &&
                           std::memcmp(hdr + 8, "WAVE", 4) == 0);
                std::fclose(f);
                if (!ok) {
                    audio.invalid++;
                    audio.invalidPaths.push_back(
                        fs::relative(e.path(), zoneDir).string());
                }
            }
        }
    }
    int totalCount = tex.count + mesh.count + audio.count;
    int totalInvalid = tex.invalid + mesh.invalid + audio.invalid;
    uint64_t totalBytes = tex.bytes + mesh.bytes + audio.bytes;
    bool pass = (totalInvalid == 0 && totalCount > 0);
    if (jsonOut) {
        auto catJ = [](const CatStats& c) {
            return nlohmann::json{
                {"count", c.count},
                {"bytes", c.bytes},
                {"invalid", c.invalid},
                {"invalidPaths", c.invalidPaths},
            };
        };
        nlohmann::json j;
        j["zone"] = zoneDir;
        j["pass"] = pass;
        j["totalCount"] = totalCount;
        j["totalBytes"] = totalBytes;
        j["totalInvalid"] = totalInvalid;
        j["textures"] = catJ(tex);
        j["meshes"] = catJ(mesh);
        j["audio"] = catJ(audio);
        std::printf("%s\n", j.dump(2).c_str());
        return pass ? 0 : 1;
    }
    std::printf("Zone pack audit: %s\n", zoneDir.c_str());
    std::printf("\n  category   count    bytes  invalid\n");
    std::printf("  textures   %5d  %7llu  %7d\n",
                tex.count,
                static_cast<unsigned long long>(tex.bytes),
                tex.invalid);
    std::printf("  meshes     %5d  %7llu  %7d\n",
                mesh.count,
                static_cast<unsigned long long>(mesh.bytes),
                mesh.invalid);
    std::printf("  audio      %5d  %7llu  %7d\n",
                audio.count,
                static_cast<unsigned long long>(audio.bytes),
                audio.invalid);
    std::printf("  ----------------------------------\n");
    std::printf("  TOTAL      %5d  %7llu  %7d\n",
                totalCount,
                static_cast<unsigned long long>(totalBytes),
                totalInvalid);
    for (const auto* cat : { &tex, &mesh, &audio }) {
        for (const auto& p : cat->invalidPaths) {
            std::printf("  INVALID    %s\n", p.c_str());
        }
    }
    std::printf("\n  %s\n", pass ? "PASS — pack is healthy"
                                  : "FAIL — see invalid paths above");
    return pass ? 0 : 1;
}

// Watertight check on a single WOM after the standard weld pass.
// Returns true if the welded mesh has zero boundary edges and zero
// non-manifold edges. Stats are written through outBoundary /
// outNonManifold for the per-zone audit's summary line. eps is the
// caller-supplied weld tolerance.
bool isWomWatertightAfterWeld(
        const std::string& womBase, float eps,
        std::size_t& outTris, std::size_t& outBoundary,
        std::size_t& outNonManifold) {
    auto wom = wowee::pipeline::WoweeModelLoader::load(womBase);
    if (!wom.isValid() || wom.indices.size() % 3 != 0) {
        outTris = outBoundary = outNonManifold = 0;
        return false;
    }
    outTris = wom.indices.size() / 3;
    std::vector<glm::vec3> positions;
    positions.reserve(wom.vertices.size());
    for (const auto& v : wom.vertices) positions.push_back(v.position);
    std::size_t uniq = 0;
    auto canon = buildWeldMap(positions, eps, uniq);
    auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        if (a > b) std::swap(a, b);
        return (uint64_t(a) << 32) | uint64_t(b);
    };
    std::unordered_map<uint64_t, uint32_t> edgeUses;
    edgeUses.reserve(outTris * 3);
    for (std::size_t t = 0; t < outTris; ++t) {
        uint32_t i0 = wom.indices[t * 3 + 0];
        uint32_t i1 = wom.indices[t * 3 + 1];
        uint32_t i2 = wom.indices[t * 3 + 2];
        if (i0 >= wom.vertices.size() || i1 >= wom.vertices.size() ||
            i2 >= wom.vertices.size()) {
            return false;
        }
        uint32_t c0 = canon[i0], c1 = canon[i1], c2 = canon[i2];
        if (c0 != c1) ++edgeUses[edgeKey(c0, c1)];
        if (c1 != c2) ++edgeUses[edgeKey(c1, c2)];
        if (c2 != c0) ++edgeUses[edgeKey(c2, c0)];
    }
    outBoundary = 0;
    outNonManifold = 0;
    for (const auto& [_k, count] : edgeUses) {
        if (count == 1) ++outBoundary;
        else if (count >= 3) ++outNonManifold;
    }
    return outBoundary == 0 && outNonManifold == 0;
}

int handleAuditWatertight(int& i, int argc, char** argv) {
    // Walk every .wom under <zoneDir|projectDir> and run the
    // welded-watertight check. Reports per-mesh PASS/FAIL plus a
    // rollup. Exit code is the number of failures, capped at 255 —
    // CI-friendly: zero on full success.
    std::string root = argv[++i];
    bool jsonOut = false;
    float weldEps = 1e-4f;
    while (i + 1 < argc && argv[i + 1][0] == '-') {
        if (std::strcmp(argv[i + 1], "--json") == 0) {
            jsonOut = true; ++i;
        } else if (std::strcmp(argv[i + 1], "--weld") == 0 && i + 2 < argc) {
            try { weldEps = std::stof(argv[i + 2]); } catch (...) {}
            i += 2;
        } else {
            break;
        }
    }
    namespace fs = std::filesystem;
    if (!fs::exists(root) || !fs::is_directory(root)) {
        std::fprintf(stderr,
            "audit-watertight: %s is not a directory\n", root.c_str());
        return 1;
    }
    struct Result {
        std::string rel;
        std::size_t tris;
        std::size_t boundary;
        std::size_t nonManifold;
        bool ok;
    };
    std::vector<Result> rows;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".wom") continue;
        std::string base = e.path().string();
        base = base.substr(0, base.size() - 4);
        Result r;
        r.rel = fs::relative(e.path(), root).string();
        r.ok = isWomWatertightAfterWeld(base, weldEps, r.tris,
                                         r.boundary, r.nonManifold);
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [](const Result& a, const Result& b) {
        return a.rel < b.rel;
    });
    int failCount = 0;
    for (const auto& r : rows) if (!r.ok) ++failCount;
    if (jsonOut) {
        nlohmann::json j;
        j["root"] = root;
        j["weldEps"] = weldEps;
        j["totalMeshes"] = rows.size();
        j["failures"] = failCount;
        nlohmann::json items = nlohmann::json::array();
        for (const auto& r : rows) {
            items.push_back({{"path", r.rel},
                              {"triangles", r.tris},
                              {"boundary", r.boundary},
                              {"nonManifold", r.nonManifold},
                              {"watertight", r.ok}});
        }
        j["meshes"] = items;
        std::printf("%s\n", j.dump(2).c_str());
        return std::min(failCount, 255);
    }
    std::printf("Watertight audit: %s (weld eps %.6f)\n",
                root.c_str(), weldEps);
    if (rows.empty()) {
        std::printf("  No .wom files found.\n");
        return 0;
    }
    for (const auto& r : rows) {
        std::printf("  %s  %s  (%zu tris", r.ok ? "PASS" : "FAIL",
                    r.rel.c_str(), r.tris);
        if (!r.ok) {
            std::printf(", %zu boundary, %zu non-manifold",
                        r.boundary, r.nonManifold);
        }
        std::printf(")\n");
    }
    std::printf("\n  TOTAL: %zu meshes, %d failure(s)\n",
                rows.size(), failCount);
    return std::min(failCount, 255);
}

}  // namespace

bool handleAudits(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--info-zone-deps") == 0 && i + 1 < argc) {
        outRc = handleZoneDeps(i, argc, argv);
        return true;
    }
    if (std::strcmp(argv[i], "--info-project-deps") == 0 && i + 1 < argc) {
        std::string projectDir = argv[++i];
        std::string self = (argc > 0) ? argv[0] : "wowee_editor";
        outRc = runPerZoneAudit(projectDir, "Project deps",
                                "--info-zone-deps", self,
                                "re-run --info-zone-deps on FAILing zones for detail");
        return true;
    }
    if (std::strcmp(argv[i], "--validate-zone-pack") == 0 && i + 1 < argc) {
        outRc = handleValidateZonePack(i, argc, argv);
        return true;
    }
    if (std::strcmp(argv[i], "--validate-project-packs") == 0 && i + 1 < argc) {
        std::string projectDir = argv[++i];
        std::string self = (argc > 0) ? argv[0] : "wowee_editor";
        outRc = runPerZoneAudit(projectDir, "Project pack audit",
                                "--validate-zone-pack", self, "");
        return true;
    }
    if (std::strcmp(argv[i], "--audit-watertight") == 0 && i + 1 < argc) {
        outRc = handleAuditWatertight(i, argc, argv);
        return true;
    }
    return false;
}

}  // namespace cli
}  // namespace editor
}  // namespace wowee
