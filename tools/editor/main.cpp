#include "editor_app.hpp"
#include "cli_gen_audio.hpp"
#include "cli_zone_packs.hpp"
#include "cli_audits.hpp"
#include "cli_readmes.hpp"
#include "cli_zone_inventory.hpp"
#include "cli_project_inventory.hpp"
#include "cli_help.hpp"
#include "cli_gen_texture.hpp"
#include "cli_gen_mesh.hpp"
#include "cli_mesh_io.hpp"
#include "cli_mesh_edit.hpp"
#include "cli_wom_info.hpp"
#include "cli_format_validate.hpp"
#include "cli_convert.hpp"
#include "cli_format_info.hpp"
#include "cli_pack.hpp"
#include "cli_content_info.hpp"
#include "cli_zone_info.hpp"
#include "cli_data_tree.hpp"
#include "cli_diff.hpp"
#include "content_pack.hpp"
#include "npc_spawner.hpp"
#include "object_placer.hpp"
#include "quest_editor.hpp"
#include "wowee_terrain.hpp"
#include "zone_manifest.hpp"
#include "terrain_editor.hpp"
#include "terrain_biomes.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "pipeline/wowee_model.hpp"
#include "pipeline/wowee_building.hpp"
#include "pipeline/wowee_collision.hpp"
#include "pipeline/wowee_terrain_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/custom_zone_discovery.hpp"
#include "core/logger.hpp"
#include <string>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <functional>
#include <memory>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "stb_image_write.h"
#include "stb_image.h"  // implementation in stb_image_impl.cpp

// ─── Open-format consistency checks ─────────────────────────────
// Both validators are called from the per-file CLI commands AND
// from --validate-all which walks a zone dir. Returning a vector
// of error strings (empty == passed) keeps callers simple.
// Minimal SHA-256 implementation (FIPS 180-4) used by --export-zone-checksum
// to produce hashes that interoperate with `sha256sum -c`. Not exposed beyond
// this file — about 90 LoC, no external deps. See RFC 6234 for the algorithm.
namespace wowee_sha256 {
struct State {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t totalBits = 0;
    uint8_t buf[64] = {};
    size_t bufLen = 0;
};
static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
static void compress(State& s, const uint8_t* block) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i*4]) << 24) | (uint32_t(block[i*4+1]) << 16) |
               (uint32_t(block[i*4+2]) << 8) | uint32_t(block[i*4+3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3];
    uint32_t e = s.h[4], f = s.h[5], g = s.h[6], h = s.h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d;
    s.h[4] += e; s.h[5] += f; s.h[6] += g; s.h[7] += h;
}
static void update(State& s, const uint8_t* data, size_t len) {
    s.totalBits += len * 8;
    while (len > 0) {
        size_t take = std::min(len, sizeof(s.buf) - s.bufLen);
        std::memcpy(s.buf + s.bufLen, data, take);
        s.bufLen += take; data += take; len -= take;
        if (s.bufLen == 64) { compress(s, s.buf); s.bufLen = 0; }
    }
}
static std::string hexFinal(State& s) {
    s.buf[s.bufLen++] = 0x80;
    if (s.bufLen > 56) {
        std::memset(s.buf + s.bufLen, 0, 64 - s.bufLen);
        compress(s, s.buf); s.bufLen = 0;
    }
    std::memset(s.buf + s.bufLen, 0, 56 - s.bufLen);
    for (int i = 7; i >= 0; --i) s.buf[56 + (7 - i)] = (s.totalBits >> (i * 8)) & 0xFF;
    compress(s, s.buf);
    char out[65] = {};
    for (int i = 0; i < 8; ++i) {
        std::snprintf(out + i * 8, 9, "%08x", s.h[i]);
    }
    return std::string(out);
}
static std::string fileHex(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    State s;
    char chunk[16384];
    while (in.read(chunk, sizeof(chunk)) || in.gcount() > 0) {
        update(s, reinterpret_cast<const uint8_t*>(chunk),
               static_cast<size_t>(in.gcount()));
    }
    return hexFinal(s);
}
static std::string hex(const uint8_t* data, size_t len) {
    State s;
    update(s, data, len);
    return hexFinal(s);
}
}  // namespace wowee_sha256



int main(int argc, char* argv[]) {
    std::string dataPath;
    std::string adtMap;
    int adtX = -1, adtY = -1;

    // Detect non-GUI options that are missing their argument and bail out
    // with a helpful message instead of silently dropping into the GUI.
    static const char* kArgRequired[] = {
        "--data", "--info", "--info-batches", "--info-textures", "--info-doodads",
        "--info-attachments", "--info-particles", "--info-sequences",
        "--info-bones", "--export-bones-dot",
        "--list-zone-meshes", "--list-zone-audio", "--list-zone-textures",
        "--list-project-meshes", "--list-project-audio",
        "--list-project-textures",
        "--info-zone-models-total", "--info-project-models-total",
        "--list-zone-meshes-detail", "--list-project-meshes-detail", "--info-mesh",
        "--info-mesh-storage-budget",
        "--info-wob", "--info-woc", "--info-wot",
        "--info-creatures", "--info-objects", "--info-quests",
        "--info-extract", "--info-extract-tree", "--info-extract-budget",
        "--list-missing-sidecars",
        "--info-png", "--info-jsondbc", "--info-blp", "--info-pack-budget",
        "--info-pack-tree",
        "--info-m2", "--info-wmo", "--info-adt",
        "--info-zone", "--info-zone-overview", "--info-project-overview",
        "--copy-project", "--info-wcp", "--list-wcp",
        "--list-creatures", "--list-objects", "--list-quests",
        "--list-quest-objectives", "--list-quest-rewards",
        "--info-creature", "--info-quest", "--info-object",
        "--info-quest-graph-stats",
        "--info-creatures-by-faction", "--info-creatures-by-level",
        "--info-objects-by-path", "--info-objects-by-type",
        "--info-quests-by-level", "--info-quests-by-xp",
        "--unpack-wcp", "--pack-wcp",
        "--validate", "--validate-wom", "--validate-wob", "--validate-woc",
        "--validate-whm", "--validate-all", "--validate-project",
        "--validate-project-open-only", "--audit-project", "--bench-audit-project",
        "--bench-validate-project", "--bench-bake-project",
        "--bench-migrate-data-tree", "--list-data-tree-largest",
        "--export-data-tree-md", "--gen-texture", "--gen-mesh", "--gen-mesh-textured",
        "--add-texture-to-mesh", "--add-texture-to-zone",
        "--gen-mesh-stairs", "--gen-mesh-grid", "--gen-mesh-disc",
        "--gen-mesh-tube", "--gen-mesh-capsule", "--gen-mesh-arch",
        "--gen-mesh-pyramid", "--gen-mesh-fence", "--gen-mesh-tree",
        "--gen-mesh-rock", "--gen-mesh-pillar", "--gen-mesh-bridge",
        "--gen-mesh-tower", "--gen-mesh-house", "--gen-mesh-fountain",
        "--gen-mesh-statue", "--gen-mesh-altar", "--gen-mesh-portal",
        "--gen-mesh-archway", "--gen-mesh-barrel", "--gen-mesh-chest",
        "--gen-mesh-anvil", "--gen-mesh-mushroom", "--gen-mesh-cart",
        "--gen-mesh-banner", "--gen-mesh-grave", "--gen-mesh-bench",
        "--gen-mesh-shrine",
        "--gen-texture-gradient",
        "--gen-mesh-from-heightmap", "--export-mesh-heightmap",
        "--displace-mesh",
        "--scale-mesh", "--translate-mesh", "--strip-mesh",
        "--gen-texture-noise", "--gen-texture-noise-color", "--rotate-mesh",
        "--center-mesh", "--flip-mesh-normals", "--mirror-mesh",
        "--smooth-mesh-normals",
        "--merge-meshes",
        "--gen-texture-radial", "--gen-texture-stripes", "--gen-texture-dots",
        "--gen-texture-rings", "--gen-texture-checker", "--gen-texture-brick",
        "--gen-texture-wood", "--gen-texture-grass", "--gen-texture-fabric",
        "--gen-texture-cobble", "--gen-texture-marble", "--gen-texture-metal",
        "--gen-texture-leather", "--gen-texture-sand", "--gen-texture-snow",
        "--gen-texture-lava", "--gen-texture-tile", "--gen-texture-bark",
        "--gen-texture-clouds", "--gen-texture-stars", "--gen-texture-vines",
        "--gen-texture-mosaic", "--gen-texture-rust", "--gen-texture-circuit",
        "--validate-glb", "--info-glb", "--info-glb-tree", "--info-glb-bytes",
        "--validate-jsondbc", "--check-glb-bounds", "--validate-stl",
        "--validate-png", "--validate-blp",
        "--zone-summary", "--info-zone-tree", "--info-project-tree",
        "--info-zone-bytes", "--info-project-bytes",
        "--info-zone-extents", "--info-project-extents",
        "--info-zone-water", "--info-project-water",
        "--info-zone-density", "--info-project-density",
        "--export-zone-summary-md", "--export-quest-graph",
        "--export-zone-csv", "--export-zone-html", "--export-project-html",
        "--export-project-md", "--export-zone-checksum", "--export-project-checksum",
        "--validate-project-checksum",
        "--scaffold-zone", "--mvp-zone", "--add-tile", "--remove-tile", "--list-tiles",
        "--for-each-zone", "--for-each-tile", "--zone-stats", "--info-tilemap",
        "--list-zone-deps", "--list-project-orphans", "--remove-project-orphans",
        "--check-zone-refs", "--check-zone-content",
        "--check-project-content", "--check-project-refs",
        "--export-zone-deps-md", "--export-zone-spawn-png",
        "--add-creature", "--add-object", "--add-quest", "--add-item",
        "--random-populate-zone", "--random-populate-items",
        "--info-zone-audio", "--snap-zone-to-ground", "--audit-zone-spawns",
        "--info-project-audio", "--snap-project-to-ground",
        "--audit-project-spawns", "--list-zone-spawns", "--list-project-spawns",
        "--gen-random-zone", "--gen-random-project", "--gen-zone-texture-pack",
        "--gen-zone-mesh-pack", "--gen-zone-starter-pack",
        "--gen-project-starter-pack", "--gen-audio-tone",
        "--gen-audio-noise", "--gen-audio-sweep", "--gen-zone-audio-pack",
        "--info-zone-summary", "--info-project-summary",
        "--info-zone-deps", "--info-project-deps",
        "--gen-zone-readme", "--gen-project-readme",
        "--validate-zone-pack", "--validate-project-packs", "--info-spawn",
        "--diff-zone-spawns",
        "--list-items", "--info-item", "--set-item", "--export-zone-items-md",
        "--export-project-items-md", "--export-project-items-csv",
        "--add-quest-objective", "--add-quest-reward-item", "--set-quest-reward",
        "--remove-quest-objective", "--clone-quest", "--clone-creature",
        "--clone-item", "--validate-items", "--validate-project-items",
        "--info-project-items",
        "--clone-object",
        "--remove-creature", "--remove-object", "--remove-quest", "--remove-item",
        "--copy-zone-items",
        "--copy-zone", "--rename-zone", "--remove-zone",
        "--clear-zone-content", "--strip-zone", "--strip-project",
        "--repair-zone", "--repair-project",
        "--gen-makefile", "--gen-project-makefile",
        "--build-woc", "--regen-collision", "--fix-zone",
        "--export-png", "--export-obj", "--import-obj",
        "--export-wob-obj", "--import-wob-obj",
        "--export-woc-obj", "--export-whm-obj",
        "--export-glb", "--export-wob-glb", "--export-whm-glb",
        "--export-stl", "--import-stl",
        "--bake-zone-glb", "--bake-zone-stl", "--bake-zone-obj",
        "--bake-project-obj", "--bake-project-stl", "--bake-project-glb",
        "--convert-m2", "--convert-m2-batch",
        "--convert-wmo", "--convert-wmo-batch",
        "--convert-dbc-json", "--convert-dbc-batch", "--convert-json-dbc",
        "--convert-blp-png", "--convert-blp-batch",
        "--migrate-wom", "--migrate-zone", "--migrate-project",
        "--migrate-data-tree", "--info-data-tree", "--strip-data-tree",
        "--audit-data-tree",
        "--migrate-jsondbc",
    };
    for (int i = 1; i < argc; i++) {
        for (const char* opt : kArgRequired) {
            if (std::strcmp(argv[i], opt) == 0 && i + 1 >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", opt);
                return 1;
            }
        }
        if (std::strcmp(argv[i], "--adt") == 0 && i + 3 >= argc) {
            std::fprintf(stderr, "--adt requires <map> <x> <y>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-zone") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-zone requires <zoneA> <zoneB>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-glb") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-glb requires <a.glb> <b.glb>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-wom") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-wom requires <a-base> <b-base>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-wob") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-wob requires <a-base> <b-base>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-whm") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-whm requires <a-base> <b-base>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-woc") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-woc requires <a.woc> <b.woc>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-jsondbc") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-jsondbc requires <a.json> <b.json>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-extract") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-extract requires <dirA> <dirB>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-checksum") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--diff-checksum requires <a.sha256> <b.sha256>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--diff-wcp") == 0 && i + 2 >= argc) {
            std::fprintf(stderr, "--diff-wcp requires two paths\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--add-creature") == 0 && i + 5 >= argc) {
            std::fprintf(stderr,
                "--add-creature requires <zoneDir> <name> <x> <y> <z>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--add-object") == 0 && i + 6 >= argc) {
            std::fprintf(stderr,
                "--add-object requires <zoneDir> <m2|wmo> <gamePath> <x> <y> <z>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--add-quest") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--add-quest requires <zoneDir> <title>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--add-quest-objective") == 0 && i + 4 >= argc) {
            std::fprintf(stderr,
                "--add-quest-objective requires <zoneDir> <questIdx> <type> <targetName>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--remove-quest-objective") == 0 && i + 3 >= argc) {
            std::fprintf(stderr,
                "--remove-quest-objective requires <zoneDir> <questIdx> <objIdx>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--clone-quest") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--clone-quest requires <zoneDir> <questIdx>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--clone-creature") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--clone-creature requires <zoneDir> <idx>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--clone-object") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--clone-object requires <zoneDir> <idx>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--add-quest-reward-item") == 0 && i + 3 >= argc) {
            std::fprintf(stderr,
                "--add-quest-reward-item requires <zoneDir> <questIdx> <itemPath>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--set-quest-reward") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--set-quest-reward requires <zoneDir> <questIdx> [--xp N] [--gold N] [--silver N] [--copper N]\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--add-tile") == 0 && i + 3 >= argc) {
            std::fprintf(stderr,
                "--add-tile requires <zoneDir> <tx> <ty>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--remove-tile") == 0 && i + 3 >= argc) {
            std::fprintf(stderr,
                "--remove-tile requires <zoneDir> <tx> <ty>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--copy-zone") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--copy-zone requires <srcDir> <newName>\n");
            return 1;
        }
        if (std::strcmp(argv[i], "--rename-zone") == 0 && i + 2 >= argc) {
            std::fprintf(stderr,
                "--rename-zone requires <srcDir> <newName>\n");
            return 1;
        }
        for (const char* opt : {"--remove-creature", "--remove-object",
                                 "--remove-quest"}) {
            if (std::strcmp(argv[i], opt) == 0 && i + 2 >= argc) {
                std::fprintf(stderr, "%s requires <zoneDir> <index>\n", opt);
                return 1;
            }
        }
    }

    for (int i = 1; i < argc; i++) {
        // Modular handler families: extracted from the in-line if/else
        // chain below to keep main.cpp from sprawling further. Each
        // family lives in its own .cpp; if it matches argv[i] it
        // sets outRc and we exit. Otherwise fall through to the
        // legacy in-line dispatch.
        {
            int outRc = 0;
            if (wowee::editor::cli::handleGenAudio(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleZonePacks(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleAudits(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleReadmes(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleZoneInventory(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleProjectInventory(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleGenTexture(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleGenMesh(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleMeshIO(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleMeshEdit(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleWomInfo(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleFormatValidate(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleConvert(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleFormatInfo(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handlePack(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleContentInfo(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleZoneInfo(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleDataTree(i, argc, argv, outRc)) {
                return outRc;
            }
            if (wowee::editor::cli::handleDiff(i, argc, argv, outRc)) {
                return outRc;
            }
        }
        if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            dataPath = argv[++i];
        } else if (std::strcmp(argv[i], "--adt") == 0 && i + 3 < argc) {
            adtMap = argv[++i];
            adtX = std::atoi(argv[++i]);
            adtY = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--info-zone-models-total") == 0 && i + 1 < argc) {
            // Aggregate WOM/WOB stats across every model in a zone.
            // Useful for capacity planning ('how many bones across all
            // my creatures?') and perf budgeting ('total triangles
            // per frame if all loaded?').
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr,
                    "info-zone-models-total: %s does not exist\n", zoneDir.c_str());
                return 1;
            }
            int womCount = 0, wobCount = 0;
            uint64_t womVerts = 0, womIndices = 0;
            uint64_t womBones = 0, womAnims = 0, womBatches = 0;
            uint64_t wobGroups = 0, wobVerts = 0, wobIndices = 0;
            uint64_t wobDoodads = 0, wobPortals = 0;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file()) continue;
                std::string ext = e.path().extension().string();
                std::string base = e.path().string();
                if (base.size() > ext.size())
                    base = base.substr(0, base.size() - ext.size());
                if (ext == ".wom") {
                    womCount++;
                    auto wom = wowee::pipeline::WoweeModelLoader::load(base);
                    womVerts += wom.vertices.size();
                    womIndices += wom.indices.size();
                    womBones += wom.bones.size();
                    womAnims += wom.animations.size();
                    womBatches += wom.batches.size();
                } else if (ext == ".wob") {
                    wobCount++;
                    auto wob = wowee::pipeline::WoweeBuildingLoader::load(base);
                    wobGroups += wob.groups.size();
                    for (const auto& g : wob.groups) {
                        wobVerts += g.vertices.size();
                        wobIndices += g.indices.size();
                    }
                    wobDoodads += wob.doodads.size();
                    wobPortals += wob.portals.size();
                }
            }
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["wom"] = {{"count", womCount},
                             {"vertices", womVerts},
                             {"indices", womIndices},
                             {"triangles", womIndices / 3},
                             {"bones", womBones},
                             {"animations", womAnims},
                             {"batches", womBatches}};
                j["wob"] = {{"count", wobCount},
                             {"groups", wobGroups},
                             {"vertices", wobVerts},
                             {"indices", wobIndices},
                             {"triangles", wobIndices / 3},
                             {"doodads", wobDoodads},
                             {"portals", wobPortals}};
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone models total: %s\n", zoneDir.c_str());
            std::printf("\n  WOM (open M2):\n");
            std::printf("    files     : %d\n", womCount);
            std::printf("    vertices  : %llu\n", static_cast<unsigned long long>(womVerts));
            std::printf("    triangles : %llu\n", static_cast<unsigned long long>(womIndices / 3));
            std::printf("    bones     : %llu\n", static_cast<unsigned long long>(womBones));
            std::printf("    anims     : %llu\n", static_cast<unsigned long long>(womAnims));
            std::printf("    batches   : %llu\n", static_cast<unsigned long long>(womBatches));
            std::printf("\n  WOB (open WMO):\n");
            std::printf("    files     : %d\n", wobCount);
            std::printf("    groups    : %llu\n", static_cast<unsigned long long>(wobGroups));
            std::printf("    vertices  : %llu\n", static_cast<unsigned long long>(wobVerts));
            std::printf("    triangles : %llu\n", static_cast<unsigned long long>(wobIndices / 3));
            std::printf("    doodads   : %llu\n", static_cast<unsigned long long>(wobDoodads));
            std::printf("    portals   : %llu\n", static_cast<unsigned long long>(wobPortals));
            std::printf("\n  Combined  :\n");
            std::printf("    vertices  : %llu\n", static_cast<unsigned long long>(womVerts + wobVerts));
            std::printf("    triangles : %llu\n", static_cast<unsigned long long>((womIndices + wobIndices) / 3));
            return 0;
        } else if (std::strcmp(argv[i], "--list-zone-meshes-detail") == 0 && i + 1 < argc) {
            // Per-mesh breakdown of every .wom file in <zoneDir>,
            // sorted by triangle count descending so the heaviest
            // meshes float to the top. Complements
            // --list-zone-meshes (per-zone summary) by surfacing
            // individual mesh metrics — useful for spotting
            // outliers ("which mesh is using 80% of my triangle
            // budget?") and for content audits.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr,
                    "list-zone-meshes-detail: %s does not exist\n", zoneDir.c_str());
                return 1;
            }
            struct Row {
                std::string path;
                size_t verts;
                size_t tris;
                size_t bones;
                size_t batches;
                size_t textures;
                uint64_t bytes;
                uint32_t version;
            };
            std::vector<Row> rows;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file()) continue;
                if (e.path().extension() != ".wom") continue;
                std::string base = e.path().string();
                if (base.size() >= 4) base = base.substr(0, base.size() - 4);
                auto wom = wowee::pipeline::WoweeModelLoader::load(base);
                Row r;
                r.path = fs::relative(e.path(), zoneDir, ec).string();
                if (ec) r.path = e.path().filename().string();
                r.verts = wom.vertices.size();
                r.tris = wom.indices.size() / 3;
                r.bones = wom.bones.size();
                r.batches = wom.batches.size();
                r.textures = wom.texturePaths.size();
                r.bytes = e.file_size(ec);
                if (ec) r.bytes = 0;
                r.version = wom.version;
                rows.push_back(r);
            }
            std::sort(rows.begin(), rows.end(),
                      [](const Row& a, const Row& b) { return a.tris > b.tris; });
            uint64_t totVerts = 0, totTris = 0, totBones = 0, totBytes = 0;
            for (const auto& r : rows) {
                totVerts += r.verts; totTris += r.tris;
                totBones += r.bones; totBytes += r.bytes;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["meshCount"] = rows.size();
                j["totals"] = {{"vertices", totVerts},
                                {"triangles", totTris},
                                {"bones", totBones},
                                {"bytes", totBytes}};
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& r : rows) {
                    arr.push_back({{"path", r.path},
                                    {"version", r.version},
                                    {"vertices", r.verts},
                                    {"triangles", r.tris},
                                    {"bones", r.bones},
                                    {"batches", r.batches},
                                    {"textures", r.textures},
                                    {"bytes", r.bytes}});
                }
                j["meshes"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone meshes: %s\n", zoneDir.c_str());
            std::printf("  meshes : %zu\n", rows.size());
            std::printf("  totals : %llu verts, %llu tris, %llu bones, %.1f KB\n",
                        static_cast<unsigned long long>(totVerts),
                        static_cast<unsigned long long>(totTris),
                        static_cast<unsigned long long>(totBones),
                        totBytes / 1024.0);
            if (rows.empty()) {
                std::printf("\n  *no .wom files in this zone*\n");
                return 0;
            }
            std::printf("\n  v   verts    tris   bones  batch  tex    bytes  path\n");
            for (const auto& r : rows) {
                std::printf("  v%u %6zu  %6zu  %5zu  %5zu  %3zu  %7llu  %s\n",
                            r.version, r.verts, r.tris, r.bones,
                            r.batches, r.textures,
                            static_cast<unsigned long long>(r.bytes),
                            r.path.c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-mesh") == 0 && i + 1 < argc) {
            // Single-mesh detail view aggregating bounds, version,
            // batches, bones, animations, and texture slots into one
            // report. Composite of what --info-batches / --info-bones
            // / --info-batches show separately. Useful authoring
            // command: pass a WOM and see everything about it without
            // running three sub-commands.
            std::string base = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom") {
                base = base.substr(0, base.size() - 4);
            }
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr,
                    "info-mesh: %s.wom does not exist\n", base.c_str());
                return 1;
            }
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            if (!wom.isValid()) {
                std::fprintf(stderr,
                    "info-mesh: failed to load %s.wom\n", base.c_str());
                return 1;
            }
            // Per-batch material summary.
            static const char* blendNames[] = {
                "opaque", "alpha-test", "alpha", "additive", "?", "?", "?", "?"
            };
            if (jsonOut) {
                nlohmann::json j;
                j["base"] = base;
                j["name"] = wom.name;
                j["version"] = wom.version;
                j["bounds"] = {{"min", {wom.boundMin.x, wom.boundMin.y, wom.boundMin.z}},
                                {"max", {wom.boundMax.x, wom.boundMax.y, wom.boundMax.z}},
                                {"radius", wom.boundRadius}};
                j["counts"] = {{"vertices", wom.vertices.size()},
                                {"indices", wom.indices.size()},
                                {"triangles", wom.indices.size() / 3},
                                {"bones", wom.bones.size()},
                                {"animations", wom.animations.size()},
                                {"batches", wom.batches.size()},
                                {"textures", wom.texturePaths.size()}};
                nlohmann::json bs = nlohmann::json::array();
                for (const auto& b : wom.batches) {
                    std::string tex;
                    if (b.textureIndex < wom.texturePaths.size())
                        tex = wom.texturePaths[b.textureIndex];
                    bs.push_back({{"indexStart", b.indexStart},
                                   {"indexCount", b.indexCount},
                                   {"triangles", b.indexCount / 3},
                                   {"textureIndex", b.textureIndex},
                                   {"texture", tex},
                                   {"blendMode", b.blendMode},
                                   {"flags", b.flags}});
                }
                j["batchDetail"] = bs;
                j["texturePaths"] = wom.texturePaths;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Mesh: %s.wom\n", base.c_str());
            std::printf("  name       : %s\n", wom.name.c_str());
            std::printf("  version    : v%u\n", wom.version);
            std::printf("\n  Counts:\n");
            std::printf("    vertices  : %zu\n", wom.vertices.size());
            std::printf("    triangles : %zu\n", wom.indices.size() / 3);
            std::printf("    bones     : %zu\n", wom.bones.size());
            std::printf("    anims     : %zu\n", wom.animations.size());
            std::printf("    batches   : %zu\n", wom.batches.size());
            std::printf("    textures  : %zu\n", wom.texturePaths.size());
            std::printf("\n  Bounds:\n");
            std::printf("    min       : (%.3f, %.3f, %.3f)\n",
                        wom.boundMin.x, wom.boundMin.y, wom.boundMin.z);
            std::printf("    max       : (%.3f, %.3f, %.3f)\n",
                        wom.boundMax.x, wom.boundMax.y, wom.boundMax.z);
            std::printf("    radius    : %.3f\n", wom.boundRadius);
            if (!wom.batches.empty()) {
                std::printf("\n  Batches:\n");
                std::printf("    idx  iStart  iCount  tris   blend       texture\n");
                for (size_t k = 0; k < wom.batches.size(); ++k) {
                    const auto& b = wom.batches[k];
                    std::string tex = "<oob>";
                    if (b.textureIndex < wom.texturePaths.size())
                        tex = wom.texturePaths[b.textureIndex];
                    if (tex.empty()) tex = "(empty)";
                    int blend = b.blendMode < 8 ? b.blendMode : 0;
                    std::printf("    %3zu  %6u  %6u  %4u   %-10s  %s\n",
                                k, b.indexStart, b.indexCount,
                                b.indexCount / 3, blendNames[blend],
                                tex.c_str());
                }
            }
            if (!wom.texturePaths.empty()) {
                std::printf("\n  Texture slots:\n");
                for (size_t k = 0; k < wom.texturePaths.size(); ++k) {
                    std::printf("    [%zu] %s\n", k,
                                wom.texturePaths[k].empty()
                                ? "(empty placeholder)"
                                : wom.texturePaths[k].c_str());
                }
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-mesh-storage-budget") == 0 && i + 1 < argc) {
            // Estimated bytes-per-category breakdown for a WOM.
            // Numbers are based on the in-memory struct sizes, not
            // the actual on-disk encoding (which has framing
            // overhead) — but the relative shares are accurate and
            // help users decide where shrinking efforts pay off.
            //
            // For example: a heightmap mesh's bytes are dominated by
            // vertices, so reducing vertex count is the lever to
            // pull. A skeletal mesh's animation keyframes can dwarf
            // the geometry itself — surfacing that lets the user
            // know to consider --strip-mesh --anims.
            std::string base = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom") {
                base = base.substr(0, base.size() - 4);
            }
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr,
                    "info-mesh-storage-budget: %s.wom does not exist\n",
                    base.c_str());
                return 1;
            }
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            if (!wom.isValid()) {
                std::fprintf(stderr,
                    "info-mesh-storage-budget: failed to load %s.wom\n",
                    base.c_str());
                return 1;
            }
            // Per-category byte estimates. Vertex is 12+12+8+4+4=40
            // bytes (pos/normal/uv/4 weights/4 indices). Index is
            // 4 bytes. Bone is 4+2+12+4=22 bytes. Batch is 4+4+4+2+
            // 2=16. Animation keyframe is 4+12+16+12=44 bytes.
            // Texture path is summed length plus a small per-string
            // overhead.
            uint64_t vertBytes = wom.vertices.size() * 40;
            uint64_t idxBytes = wom.indices.size() * 4;
            uint64_t boneBytes = wom.bones.size() * 22;
            uint64_t batchBytes = wom.batches.size() * 16;
            uint64_t animBytes = 0;
            size_t totalKeyframes = 0;
            for (const auto& a : wom.animations) {
                animBytes += 12;  // id + duration + movingSpeed
                for (const auto& bone : a.boneKeyframes) {
                    animBytes += bone.size() * 44;
                    totalKeyframes += bone.size();
                }
            }
            uint64_t texBytes = 0;
            for (const auto& t : wom.texturePaths) texBytes += t.size() + 8;
            namespace fs = std::filesystem;
            uint64_t actualBytes = fs::file_size(base + ".wom");
            uint64_t estBytes = vertBytes + idxBytes + boneBytes +
                                 batchBytes + animBytes + texBytes;
            struct Row { const char* name; uint64_t bytes; };
            std::vector<Row> rows = {
                {"vertices  ", vertBytes},
                {"indices   ", idxBytes},
                {"bones     ", boneBytes},
                {"animations", animBytes},
                {"batches   ", batchBytes},
                {"textures  ", texBytes},
            };
            if (jsonOut) {
                nlohmann::json j;
                j["base"] = base;
                j["fileBytes"] = actualBytes;
                j["estimatedBytes"] = estBytes;
                j["categories"] = nlohmann::json::object();
                for (const auto& r : rows) {
                    double share = estBytes > 0
                                   ? 100.0 * r.bytes / estBytes : 0.0;
                    j["categories"][r.name] = {{"bytes", r.bytes},
                                                {"share", share}};
                }
                j["counts"] = {{"vertices", wom.vertices.size()},
                                {"indices", wom.indices.size()},
                                {"bones", wom.bones.size()},
                                {"animations", wom.animations.size()},
                                {"keyframes", totalKeyframes},
                                {"batches", wom.batches.size()},
                                {"textures", wom.texturePaths.size()}};
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Mesh storage budget: %s.wom\n", base.c_str());
            std::printf("  on-disk    : %llu bytes (%.1f KB)\n",
                        static_cast<unsigned long long>(actualBytes),
                        actualBytes / 1024.0);
            std::printf("  estimated  : %llu bytes (sum of in-memory parts)\n",
                        static_cast<unsigned long long>(estBytes));
            std::printf("\n  Per-category (estimated):\n");
            for (const auto& r : rows) {
                if (r.bytes == 0) continue;
                double share = estBytes > 0
                               ? 100.0 * r.bytes / estBytes : 0.0;
                std::printf("    %s : %10llu bytes  (%5.1f%%)\n",
                            r.name,
                            static_cast<unsigned long long>(r.bytes),
                            share);
            }
            std::printf("\n  Tips:\n");
            if (animBytes > vertBytes && wom.animations.size() > 0) {
                std::printf("    - animations dominate; --strip-mesh "
                            "--anims would save %.1f KB\n",
                            animBytes / 1024.0);
            }
            if (boneBytes > vertBytes / 2 && wom.bones.size() > 0) {
                std::printf("    - bones non-trivial; consider "
                            "--strip-mesh --bones for static placement\n");
            }
            if (vertBytes > estBytes / 2) {
                std::printf("    - vertices dominate; check if a "
                            "lower-poly variant works for placement\n");
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-project-models-total") == 0 && i + 1 < argc) {
            // Multi-zone aggregate. Walks every zone in <projectDir>,
            // sums the same WOM/WOB metrics --info-zone-models-total
            // emits, and prints a per-zone breakdown table followed
            // by project-wide totals. Useful for capacity planning
            // across an entire content project.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "info-project-models-total: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            struct ZRow {
                std::string name;
                int womCount = 0, wobCount = 0;
                uint64_t womVerts = 0, womIndices = 0, womBones = 0;
                uint64_t womAnims = 0, womBatches = 0;
                uint64_t wobGroups = 0, wobVerts = 0, wobIndices = 0;
                uint64_t wobDoodads = 0, wobPortals = 0;
            };
            std::vector<ZRow> rows;
            ZRow tot;
            tot.name = "TOTAL";
            for (const auto& zoneDir : zones) {
                ZRow r;
                r.name = fs::path(zoneDir).filename().string();
                std::error_code ec;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    std::string ext = e.path().extension().string();
                    std::string base = e.path().string();
                    if (base.size() > ext.size())
                        base = base.substr(0, base.size() - ext.size());
                    if (ext == ".wom") {
                        r.womCount++;
                        auto wom = wowee::pipeline::WoweeModelLoader::load(base);
                        r.womVerts += wom.vertices.size();
                        r.womIndices += wom.indices.size();
                        r.womBones += wom.bones.size();
                        r.womAnims += wom.animations.size();
                        r.womBatches += wom.batches.size();
                    } else if (ext == ".wob") {
                        r.wobCount++;
                        auto wob = wowee::pipeline::WoweeBuildingLoader::load(base);
                        r.wobGroups += wob.groups.size();
                        for (const auto& g : wob.groups) {
                            r.wobVerts += g.vertices.size();
                            r.wobIndices += g.indices.size();
                        }
                        r.wobDoodads += wob.doodads.size();
                        r.wobPortals += wob.portals.size();
                    }
                }
                tot.womCount += r.womCount;
                tot.wobCount += r.wobCount;
                tot.womVerts += r.womVerts;
                tot.womIndices += r.womIndices;
                tot.womBones += r.womBones;
                tot.womAnims += r.womAnims;
                tot.womBatches += r.womBatches;
                tot.wobGroups += r.wobGroups;
                tot.wobVerts += r.wobVerts;
                tot.wobIndices += r.wobIndices;
                tot.wobDoodads += r.wobDoodads;
                tot.wobPortals += r.wobPortals;
                rows.push_back(r);
            }
            if (jsonOut) {
                nlohmann::json j;
                j["project"] = projectDir;
                j["zones"] = nlohmann::json::array();
                auto rowJson = [](const ZRow& r) {
                    nlohmann::json z;
                    z["name"] = r.name;
                    z["wom"] = {{"count", r.womCount},
                                 {"vertices", r.womVerts},
                                 {"indices", r.womIndices},
                                 {"triangles", r.womIndices / 3},
                                 {"bones", r.womBones},
                                 {"animations", r.womAnims},
                                 {"batches", r.womBatches}};
                    z["wob"] = {{"count", r.wobCount},
                                 {"groups", r.wobGroups},
                                 {"vertices", r.wobVerts},
                                 {"indices", r.wobIndices},
                                 {"triangles", r.wobIndices / 3},
                                 {"doodads", r.wobDoodads},
                                 {"portals", r.wobPortals}};
                    return z;
                };
                for (const auto& r : rows) j["zones"].push_back(rowJson(r));
                j["total"] = rowJson(tot);
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Project models total: %s\n", projectDir.c_str());
            std::printf("  zones : %zu\n\n", zones.size());
            std::printf("  zone                  WOMs  WOMtri  bones  WOBs  WOBtri  doodads\n");
            for (const auto& r : rows) {
                std::printf("  %-20s %5d %7llu %6llu %5d %7llu %8llu\n",
                            r.name.substr(0, 20).c_str(),
                            r.womCount,
                            static_cast<unsigned long long>(r.womIndices / 3),
                            static_cast<unsigned long long>(r.womBones),
                            r.wobCount,
                            static_cast<unsigned long long>(r.wobIndices / 3),
                            static_cast<unsigned long long>(r.wobDoodads));
            }
            std::printf("  %-20s %5d %7llu %6llu %5d %7llu %8llu\n",
                        tot.name.c_str(),
                        tot.womCount,
                        static_cast<unsigned long long>(tot.womIndices / 3),
                        static_cast<unsigned long long>(tot.womBones),
                        tot.wobCount,
                        static_cast<unsigned long long>(tot.wobIndices / 3),
                        static_cast<unsigned long long>(tot.wobDoodads));
            std::printf("\n  Combined verts/tris (WOM+WOB): %llu / %llu\n",
                        static_cast<unsigned long long>(tot.womVerts + tot.wobVerts),
                        static_cast<unsigned long long>((tot.womIndices + tot.wobIndices) / 3));
            return 0;
        } else if (std::strcmp(argv[i], "--info-wob") == 0 && i + 1 < argc) {
            std::string base = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wob")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeBuildingLoader::exists(base)) {
                std::fprintf(stderr, "WOB not found: %s.wob\n", base.c_str());
                return 1;
            }
            auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
            size_t totalVerts = 0, totalIdx = 0, totalMats = 0;
            for (const auto& g : bld.groups) {
                totalVerts += g.vertices.size();
                totalIdx += g.indices.size();
                totalMats += g.materials.size();
            }
            if (jsonOut) {
                nlohmann::json j;
                j["wob"] = base + ".wob";
                j["name"] = bld.name;
                j["groups"] = bld.groups.size();
                j["portals"] = bld.portals.size();
                j["doodads"] = bld.doodads.size();
                j["boundRadius"] = bld.boundRadius;
                j["totalVerts"] = totalVerts;
                j["totalTris"] = totalIdx / 3;
                j["totalMats"] = totalMats;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("WOB: %s.wob\n", base.c_str());
            std::printf("  name        : %s\n", bld.name.c_str());
            std::printf("  groups      : %zu\n", bld.groups.size());
            std::printf("  portals     : %zu\n", bld.portals.size());
            std::printf("  doodads     : %zu\n", bld.doodads.size());
            std::printf("  boundRadius : %.2f\n", bld.boundRadius);
            std::printf("  total verts : %zu\n", totalVerts);
            std::printf("  total tris  : %zu\n", totalIdx / 3);
            std::printf("  total mats  : %zu (across all groups)\n", totalMats);
            return 0;
        } else if (std::strcmp(argv[i], "--info-quests") == 0 && i + 1 < argc) {
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "Failed to load quests.json: %s\n", path.c_str());
                return 1;
            }
            const auto& quests = qe.getQuests();
            int chained = 0, withReward = 0, withItems = 0;
            int objKill = 0, objCollect = 0, objTalk = 0;
            uint32_t totalXp = 0;
            for (const auto& q : quests) {
                if (q.nextQuestId != 0) chained++;
                if (q.reward.xp > 0 || q.reward.gold > 0 ||
                    q.reward.silver > 0 || q.reward.copper > 0) withReward++;
                if (!q.reward.itemRewards.empty()) withItems++;
                totalXp += q.reward.xp;
                using OT = wowee::editor::QuestObjectiveType;
                for (const auto& obj : q.objectives) {
                    if (obj.type == OT::KillCreature) objKill++;
                    else if (obj.type == OT::CollectItem) objCollect++;
                    else if (obj.type == OT::TalkToNPC) objTalk++;
                }
            }
            std::vector<std::string> errors;
            qe.validateChains(errors);
            if (jsonOut) {
                nlohmann::json j;
                j["file"] = path;
                j["total"] = quests.size();
                j["chained"] = chained;
                j["withReward"] = withReward;
                j["withItems"] = withItems;
                j["totalXp"] = totalXp;
                j["avgXpPerQuest"] = quests.empty() ? 0.0
                                        : double(totalXp) / quests.size();
                j["objectives"] = {{"kill", objKill},
                                    {"collect", objCollect},
                                    {"talk", objTalk}};
                j["chainErrors"] = errors;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("quests.json: %s\n", path.c_str());
            std::printf("  total       : %zu\n", quests.size());
            std::printf("  chained     : %d (have nextQuestId)\n", chained);
            std::printf("  with reward : %d\n", withReward);
            std::printf("  with items  : %d\n", withItems);
            std::printf("  total XP    : %u (avg %.0f per quest)\n", totalXp,
                        quests.empty() ? 0.0 : double(totalXp) / quests.size());
            std::printf("  objectives  : %d kill, %d collect, %d talk\n",
                        objKill, objCollect, objTalk);
            if (!errors.empty()) {
                std::printf("  chain errors: %zu\n", errors.size());
                for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-objects") == 0 && i + 1 < argc) {
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            wowee::editor::ObjectPlacer placer;
            if (!placer.loadFromFile(path)) {
                std::fprintf(stderr, "Failed to load objects.json: %s\n", path.c_str());
                return 1;
            }
            const auto& objs = placer.getObjects();
            int m2Count = 0, wmoCount = 0;
            std::unordered_map<std::string, int> pathHist;
            float minScale = 1e30f, maxScale = -1e30f;
            for (const auto& o : objs) {
                if (o.type == wowee::editor::PlaceableType::M2) m2Count++;
                else if (o.type == wowee::editor::PlaceableType::WMO) wmoCount++;
                pathHist[o.path]++;
                if (o.scale < minScale) minScale = o.scale;
                if (o.scale > maxScale) maxScale = o.scale;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["file"] = path;
                j["total"] = objs.size();
                j["m2"] = m2Count;
                j["wmo"] = wmoCount;
                j["uniquePaths"] = pathHist.size();
                if (!objs.empty()) {
                    j["scaleMin"] = minScale;
                    j["scaleMax"] = maxScale;
                }
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("objects.json: %s\n", path.c_str());
            std::printf("  total       : %zu\n", objs.size());
            std::printf("  M2 doodads  : %d\n", m2Count);
            std::printf("  WMO buildings: %d\n", wmoCount);
            std::printf("  unique paths: %zu\n", pathHist.size());
            if (!objs.empty()) {
                std::printf("  scale range : [%.2f, %.2f]\n", minScale, maxScale);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-extract") == 0 && i + 1 < argc) {
            // Walk an extracted-asset directory and report counts by
            // extension + open-format coverage. Useful for seeing whether
            // a user ran asset_extract with --emit-open.
            std::string dataDir = argv[++i];
            // Optional --json after the dir for machine-readable output.
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(dataDir)) {
                std::fprintf(stderr, "info-extract: %s does not exist\n", dataDir.c_str());
                return 1;
            }
            // Per-format counts. Pair proprietary with open-format sidecar
            // so the report can show coverage percentages. Track bytes
            // separately for proprietary vs open so the user can see how
            // much disk a "purge proprietary after open conversion"
            // workflow would save (or cost — open formats are sometimes
            // larger, e.g. PNG vs DXT-compressed BLP).
            uint64_t blpCount = 0, pngSidecar = 0;
            uint64_t dbcCount = 0, jsonSidecar = 0;
            uint64_t m2Count  = 0, womSidecar = 0;
            uint64_t wmoCount = 0, wobSidecar = 0;
            uint64_t adtCount = 0, whmSidecar = 0;
            uint64_t totalBytes = 0;
            uint64_t propBytes = 0, openBytes = 0;
            for (auto& entry : fs::recursive_directory_iterator(dataDir)) {
                if (!entry.is_regular_file()) continue;
                uint64_t fsz = entry.file_size();
                totalBytes += fsz;
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                std::string base = entry.path().string();
                if (base.size() > ext.size()) base = base.substr(0, base.size() - ext.size());
                auto sidecarExists = [&](const char* sidecarExt) {
                    return fs::exists(base + sidecarExt);
                };
                if      (ext == ".blp") { blpCount++; propBytes += fsz; if (sidecarExists(".png"))  pngSidecar++; }
                else if (ext == ".dbc") { dbcCount++; propBytes += fsz; if (sidecarExists(".json")) jsonSidecar++; }
                else if (ext == ".m2")  { m2Count++;  propBytes += fsz; if (sidecarExists(".wom"))  womSidecar++; }
                else if (ext == ".wmo") {
                    propBytes += fsz;
                    std::string fname = entry.path().filename().string();
                    auto under = fname.rfind('_');
                    bool isGroup = (under != std::string::npos &&
                                    fname.size() - under == 8);
                    if (!isGroup) {
                        wmoCount++; if (sidecarExists(".wob")) wobSidecar++;
                    }
                }
                else if (ext == ".adt") { adtCount++; propBytes += fsz; if (sidecarExists(".whm")) whmSidecar++; }
                else if (ext == ".png" || ext == ".json" || ext == ".wom" ||
                         ext == ".wob" || ext == ".whm" || ext == ".wot" ||
                         ext == ".woc") {
                    openBytes += fsz;
                }
            }
            auto pct = [](uint64_t x, uint64_t total) {
                return total == 0 ? 0.0 : (100.0 * x) / total;
            };
            if (jsonOut) {
                // Machine-readable summary for CI scripts; matches the
                // structure of the human-readable lines below.
                nlohmann::json j;
                j["dir"] = dataDir;
                j["totalBytes"] = totalBytes;
                j["proprietaryBytes"] = propBytes;
                j["openBytes"] = openBytes;
                auto fmtFmt = [&](const char* name, uint64_t prop, uint64_t open) {
                    nlohmann::json f;
                    f["proprietary"] = prop;
                    f["sidecar"] = open;
                    f["coverage"] = pct(open, prop);
                    j[name] = f;
                };
                fmtFmt("blp_png",   blpCount, pngSidecar);
                fmtFmt("dbc_json",  dbcCount, jsonSidecar);
                fmtFmt("m2_wom",    m2Count,  womSidecar);
                fmtFmt("wmo_wob",   wmoCount, wobSidecar);
                fmtFmt("adt_whm",   adtCount, whmSidecar);
                uint64_t openTotal = pngSidecar + jsonSidecar + womSidecar +
                                     wobSidecar + whmSidecar;
                uint64_t propTotal = blpCount + dbcCount + m2Count +
                                     wmoCount + adtCount;
                j["overallCoverage"] = pct(openTotal, propTotal);
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Extracted asset tree: %s\n", dataDir.c_str());
            std::printf("  total bytes  : %.2f GB\n", totalBytes / (1024.0 * 1024.0 * 1024.0));
            std::printf("  BLP textures : %lu  (%lu PNG sidecar = %.1f%% open)\n",
                        blpCount, pngSidecar, pct(pngSidecar, blpCount));
            std::printf("  DBC tables   : %lu  (%lu JSON sidecar = %.1f%% open)\n",
                        dbcCount, jsonSidecar, pct(jsonSidecar, dbcCount));
            std::printf("  M2 models    : %lu  (%lu WOM sidecar = %.1f%% open)\n",
                        m2Count, womSidecar, pct(womSidecar, m2Count));
            std::printf("  WMO buildings: %lu  (%lu WOB sidecar = %.1f%% open)\n",
                        wmoCount, wobSidecar, pct(wobSidecar, wmoCount));
            std::printf("  ADT terrain  : %lu  (%lu WHM sidecar = %.1f%% open)\n",
                        adtCount, whmSidecar, pct(whmSidecar, adtCount));
            uint64_t openTotal = pngSidecar + jsonSidecar + womSidecar + wobSidecar + whmSidecar;
            uint64_t propTotal = blpCount + dbcCount + m2Count + wmoCount + adtCount;
            std::printf("  overall open-format coverage: %.1f%%\n", pct(openTotal, propTotal));
            // Disk-usage breakdown: shows roughly how big a purge-proprietary
            // workflow would shrink the tree (or how much extra a dual-format
            // extraction costs).
            const double mb = 1024.0 * 1024.0;
            std::printf("  proprietary bytes: %.1f MB\n", propBytes / mb);
            std::printf("  open-format bytes: %.1f MB", openBytes / mb);
            if (propBytes > 0) {
                std::printf(" (%.1f%% of proprietary)",
                            100.0 * static_cast<double>(openBytes) / propBytes);
            }
            std::printf("\n");
            std::printf("  (run `asset_extract --emit-open` to fill missing sidecars)\n");
            return 0;
        } else if (std::strcmp(argv[i], "--info-extract-tree") == 0 && i + 1 < argc) {
            // Hierarchical view of an extracted asset directory grouped
            // by top-level subdirectory and format. Useful for getting
            // oriented after asset_extract finishes — '17 dirs, 142k
            // files' is hard to reason about; this groups them for
            // at-a-glance comprehension.
            std::string dataDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(dataDir) || !fs::is_directory(dataDir)) {
                std::fprintf(stderr,
                    "info-extract-tree: %s is not a directory\n", dataDir.c_str());
                return 1;
            }
            // Per-top-level-dir aggregation: per-extension count + bytes.
            // Top-level discovery: every immediate child dir of dataDir.
            struct ExtStats { int count = 0; uint64_t bytes = 0; };
            struct DirStats {
                std::string name;
                int totalFiles = 0;
                uint64_t totalBytes = 0;
                std::map<std::string, ExtStats> byExt;
            };
            std::vector<DirStats> dirs;
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(dataDir, ec)) {
                if (entry.is_regular_file()) continue;  // skip top-level files
                if (!entry.is_directory()) continue;
                DirStats d;
                d.name = entry.path().filename().string();
                for (const auto& f : fs::recursive_directory_iterator(entry.path(), ec)) {
                    if (!f.is_regular_file()) continue;
                    std::string ext = f.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (ext.empty()) ext = "(no-ext)";
                    uint64_t sz = f.file_size(ec);
                    if (ec) continue;
                    d.totalFiles++;
                    d.totalBytes += sz;
                    auto& es = d.byExt[ext];
                    es.count++;
                    es.bytes += sz;
                }
                dirs.push_back(std::move(d));
            }
            std::sort(dirs.begin(), dirs.end(),
                      [](const DirStats& a, const DirStats& b) {
                          return a.totalBytes > b.totalBytes;
                      });
            int totalDirs = static_cast<int>(dirs.size());
            int totalFiles = 0;
            uint64_t totalBytes = 0;
            for (const auto& d : dirs) {
                totalFiles += d.totalFiles;
                totalBytes += d.totalBytes;
            }
            std::printf("%s/  (%d dirs, %d files, %.1f MB)\n",
                        dataDir.c_str(), totalDirs, totalFiles,
                        totalBytes / (1024.0 * 1024.0));
            for (size_t k = 0; k < dirs.size(); ++k) {
                bool lastDir = (k == dirs.size() - 1);
                const auto& d = dirs[k];
                const char* dBranch = lastDir ? "└─ " : "├─ ";
                const char* dCont   = lastDir ? "   " : "│  ";
                std::printf("%s%s/  (%d files, %.1f MB)\n",
                            dBranch, d.name.c_str(), d.totalFiles,
                            d.totalBytes / (1024.0 * 1024.0));
                // Sort extensions by byte size descending — heaviest first.
                std::vector<std::pair<std::string, ExtStats>> exts(
                    d.byExt.begin(), d.byExt.end());
                std::sort(exts.begin(), exts.end(),
                          [](const auto& a, const auto& b) {
                              return a.second.bytes > b.second.bytes;
                          });
                for (size_t e = 0; e < exts.size(); ++e) {
                    bool lastE = (e == exts.size() - 1);
                    const char* eBranch = lastE ? "└─ " : "├─ ";
                    const auto& [ext, st] = exts[e];
                    std::printf("%s%s%-10s  %5d files  %8.1f KB\n",
                                dCont, eBranch, ext.c_str(),
                                st.count, st.bytes / 1024.0);
                }
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-extract-budget") == 0 && i + 1 < argc) {
            // Per-extension byte breakdown of an extract dir, sorted
            // largest-first. Companion to --info-pack-budget (which
            // operates on .wcp archives) — this answers 'where did my
            // 31 GB extract go?' with a flat sortable table.
            std::string dataDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(dataDir) || !fs::is_directory(dataDir)) {
                std::fprintf(stderr,
                    "info-extract-budget: %s is not a directory\n",
                    dataDir.c_str());
                return 1;
            }
            std::map<std::string, std::pair<int, uint64_t>> byExt;
            uint64_t totalBytes = 0;
            int totalFiles = 0;
            std::error_code ec;
            for (const auto& entry : fs::recursive_directory_iterator(dataDir, ec)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                                [](unsigned char c) { return std::tolower(c); });
                if (ext.empty()) ext = "(no-ext)";
                uint64_t sz = entry.file_size(ec);
                if (ec) continue;
                byExt[ext].first++;
                byExt[ext].second += sz;
                totalBytes += sz;
                totalFiles++;
            }
            std::vector<std::pair<std::string, std::pair<int, uint64_t>>> sorted(
                byExt.begin(), byExt.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) {
                          return a.second.second > b.second.second;
                      });
            if (jsonOut) {
                nlohmann::json j;
                j["dir"] = dataDir;
                j["totalFiles"] = totalFiles;
                j["totalBytes"] = totalBytes;
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& [ext, cb] : sorted) {
                    arr.push_back({{"ext", ext},
                                    {"count", cb.first},
                                    {"bytes", cb.second}});
                }
                j["byExtension"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Extract budget: %s\n", dataDir.c_str());
            std::printf("  total: %d file(s), %.2f MB\n",
                        totalFiles, totalBytes / (1024.0 * 1024.0));
            std::printf("\n  ext           count        bytes        MB    share\n");
            // Cap to top 30 to keep output manageable on huge extracts;
            // suppressed entries roll into 'other'.
            const size_t kTopN = 30;
            uint64_t otherBytes = 0;
            int otherCount = 0;
            for (size_t k = 0; k < sorted.size(); ++k) {
                if (k < kTopN) {
                    const auto& [ext, cb] = sorted[k];
                    double pct = totalBytes > 0
                        ? 100.0 * cb.second / totalBytes : 0.0;
                    std::printf("  %-12s %6d  %11llu  %8.1f  %5.1f%%\n",
                                ext.c_str(), cb.first,
                                static_cast<unsigned long long>(cb.second),
                                cb.second / (1024.0 * 1024.0), pct);
                } else {
                    otherBytes += sorted[k].second.second;
                    otherCount += sorted[k].second.first;
                }
            }
            if (otherCount > 0) {
                double pct = totalBytes > 0 ? 100.0 * otherBytes / totalBytes : 0.0;
                std::printf("  %-12s %6d  %11llu  %8.1f  %5.1f%%  (%zu more extensions)\n",
                            "(other)", otherCount,
                            static_cast<unsigned long long>(otherBytes),
                            otherBytes / (1024.0 * 1024.0), pct,
                            sorted.size() - kTopN);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--list-missing-sidecars") == 0 && i + 1 < argc) {
            // Actionable counterpart to --info-extract: emit one line per
            // proprietary file lacking its open-format sidecar. Pipe into
            // xargs to drive a targeted re-extract:
            //   wowee_editor --list-missing-sidecars Data/ |
            //     awk '/\.blp$/ {print}' |
            //     xargs asset_extract --emit-png-only
            std::string dataDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(dataDir)) {
                std::fprintf(stderr, "list-missing-sidecars: %s does not exist\n",
                             dataDir.c_str());
                return 1;
            }
            std::vector<std::string> missingPng, missingJson, missingWom,
                                     missingWob, missingWhm;
            for (auto& entry : fs::recursive_directory_iterator(dataDir)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                std::string base = entry.path().string();
                if (base.size() > ext.size())
                    base = base.substr(0, base.size() - ext.size());
                auto missing = [&](const char* sidecarExt) {
                    return !fs::exists(base + sidecarExt);
                };
                if (ext == ".blp" && missing(".png"))
                    missingPng.push_back(entry.path().string());
                else if (ext == ".dbc" && missing(".json"))
                    missingJson.push_back(entry.path().string());
                else if (ext == ".m2" && missing(".wom"))
                    missingWom.push_back(entry.path().string());
                else if (ext == ".wmo") {
                    // Group files (Foo_NNN.wmo) don't get individual sidecars
                    // — only the parent file gets a .wob.
                    std::string fname = entry.path().filename().string();
                    auto under = fname.rfind('_');
                    bool isGroup = (under != std::string::npos &&
                                    fname.size() - under == 8);
                    if (!isGroup && missing(".wob"))
                        missingWob.push_back(entry.path().string());
                }
                else if (ext == ".adt" && missing(".whm"))
                    missingWhm.push_back(entry.path().string());
            }
            size_t total = missingPng.size() + missingJson.size() +
                           missingWom.size() + missingWob.size() +
                           missingWhm.size();
            if (jsonOut) {
                nlohmann::json j;
                j["dir"] = dataDir;
                j["totalMissing"] = total;
                j["missing"] = {
                    {"png",  missingPng},
                    {"json", missingJson},
                    {"wom",  missingWom},
                    {"wob",  missingWob},
                    {"whm",  missingWhm},
                };
                std::printf("%s\n", j.dump(2).c_str());
                return total == 0 ? 0 : 1;
            }
            // Plain mode: one path per line, sorted by group, prefixed with
            // the missing extension so awk/grep can filter.
            auto emit = [](const char* tag, const std::vector<std::string>& files) {
                for (const auto& f : files) std::printf("%s\t%s\n", tag, f.c_str());
            };
            emit("png",  missingPng);
            emit("json", missingJson);
            emit("wom",  missingWom);
            emit("wob",  missingWob);
            emit("whm",  missingWhm);
            std::fprintf(stderr,
                "%zu missing (PNG=%zu JSON=%zu WOM=%zu WOB=%zu WHM=%zu)\n",
                total, missingPng.size(), missingJson.size(),
                missingWom.size(), missingWob.size(), missingWhm.size());
            return total == 0 ? 0 : 1;
        } else if (std::strcmp(argv[i], "--copy-project") == 0 && i + 2 < argc) {
            // Recursively copy an entire project tree. Refuses to
            // overwrite an existing destination so a typo doesn't
            // silently merge into the wrong project.
            std::string fromDir = argv[++i];
            std::string toDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(fromDir) || !fs::is_directory(fromDir)) {
                std::fprintf(stderr,
                    "copy-project: %s is not a directory\n", fromDir.c_str());
                return 1;
            }
            if (fs::exists(toDir)) {
                std::fprintf(stderr,
                    "copy-project: destination %s already exists "
                    "(delete it first if intentional)\n", toDir.c_str());
                return 1;
            }
            std::error_code ec;
            fs::copy(fromDir, toDir,
                      fs::copy_options::recursive | fs::copy_options::copy_symlinks,
                      ec);
            if (ec) {
                std::fprintf(stderr,
                    "copy-project: copy failed (%s)\n", ec.message().c_str());
                return 1;
            }
            // Count what was copied for the report.
            int zoneCount = 0, fileCount = 0;
            uint64_t totalBytes = 0;
            for (const auto& entry : fs::directory_iterator(toDir, ec)) {
                if (entry.is_directory() &&
                    fs::exists(entry.path() / "zone.json")) zoneCount++;
            }
            for (const auto& e : fs::recursive_directory_iterator(toDir, ec)) {
                if (e.is_regular_file()) {
                    fileCount++;
                    totalBytes += e.file_size(ec);
                }
            }
            std::printf("Copied %s -> %s\n", fromDir.c_str(), toDir.c_str());
            std::printf("  zones        : %d\n", zoneCount);
            std::printf("  files        : %d\n", fileCount);
            std::printf("  total bytes  : %llu (%.1f MB)\n",
                        static_cast<unsigned long long>(totalBytes),
                        totalBytes / (1024.0 * 1024.0));
            return 0;
        } else if (std::strcmp(argv[i], "--info-wot") == 0 && i + 1 < argc) {
            std::string base = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            // Accept "/path/file.wot", "/path/file.whm", or "/path/file"; the
            // loader pairs both extensions from the same base path.
            for (const char* ext : {".wot", ".whm"}) {
                if (base.size() >= 4 && base.substr(base.size() - 4) == ext) {
                    base = base.substr(0, base.size() - 4);
                    break;
                }
            }
            if (!wowee::pipeline::WoweeTerrainLoader::exists(base)) {
                std::fprintf(stderr, "WOT/WHM not found at base: %s\n", base.c_str());
                return 1;
            }
            wowee::pipeline::ADTTerrain terrain;
            if (!wowee::pipeline::WoweeTerrainLoader::load(base, terrain)) {
                std::fprintf(stderr, "Failed to load WOT/WHM: %s\n", base.c_str());
                return 1;
            }
            int chunksWithHeights = 0, chunksWithLayers = 0, chunksWithWater = 0;
            float minH = 1e30f, maxH = -1e30f;
            for (int ci = 0; ci < 256; ci++) {
                const auto& c = terrain.chunks[ci];
                if (c.hasHeightMap()) {
                    chunksWithHeights++;
                    for (float h : c.heightMap.heights) {
                        float total = c.position[2] + h;
                        if (total < minH) minH = total;
                        if (total > maxH) maxH = total;
                    }
                }
                if (!c.layers.empty()) chunksWithLayers++;
                if (terrain.waterData[ci].hasWater()) chunksWithWater++;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["base"] = base;
                j["tileX"] = terrain.coord.x;
                j["tileY"] = terrain.coord.y;
                j["chunks"] = {{"withHeightmap", chunksWithHeights},
                                {"withLayers", chunksWithLayers},
                                {"withWater", chunksWithWater}};
                j["textures"] = terrain.textures.size();
                j["doodads"] = terrain.doodadPlacements.size();
                j["wmos"] = terrain.wmoPlacements.size();
                if (chunksWithHeights > 0) {
                    j["heightMin"] = minH;
                    j["heightMax"] = maxH;
                }
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("WOT/WHM: %s\n", base.c_str());
            std::printf("  tile         : (%d, %d)\n", terrain.coord.x, terrain.coord.y);
            std::printf("  chunks       : %d/256 with heightmap\n", chunksWithHeights);
            std::printf("  layers       : %d/256 chunks with texture layers\n", chunksWithLayers);
            std::printf("  water        : %d/256 chunks with water\n", chunksWithWater);
            std::printf("  textures     : %zu\n", terrain.textures.size());
            std::printf("  doodads      : %zu\n", terrain.doodadPlacements.size());
            std::printf("  WMOs         : %zu\n", terrain.wmoPlacements.size());
            if (chunksWithHeights > 0) {
                std::printf("  height range : [%.2f, %.2f]\n", minH, maxH);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-woc") == 0 && i + 1 < argc) {
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            if (path.size() < 4 || path.substr(path.size() - 4) != ".woc")
                path += ".woc";
            auto col = wowee::pipeline::WoweeCollisionBuilder::load(path);
            if (!col.isValid()) {
                std::fprintf(stderr, "WOC not found or invalid: %s\n", path.c_str());
                return 1;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["woc"] = path;
                j["tileX"] = col.tileX;
                j["tileY"] = col.tileY;
                j["triangles"] = col.triangles.size();
                j["walkable"] = col.walkableCount();
                j["steep"] = col.steepCount();
                j["boundsMin"] = {col.bounds.min.x, col.bounds.min.y, col.bounds.min.z};
                j["boundsMax"] = {col.bounds.max.x, col.bounds.max.y, col.bounds.max.z};
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("WOC: %s\n", path.c_str());
            std::printf("  tile        : (%u, %u)\n", col.tileX, col.tileY);
            std::printf("  triangles   : %zu\n", col.triangles.size());
            std::printf("  walkable    : %zu\n", col.walkableCount());
            std::printf("  steep       : %zu\n", col.steepCount());
            std::printf("  bounds.min  : (%.1f, %.1f, %.1f)\n",
                        col.bounds.min.x, col.bounds.min.y, col.bounds.min.z);
            std::printf("  bounds.max  : (%.1f, %.1f, %.1f)\n",
                        col.bounds.max.x, col.bounds.max.y, col.bounds.max.z);
            return 0;
        } else if (std::strcmp(argv[i], "--zone-summary") == 0 && i + 1 < argc) {
            // One-shot zone overview: validate + creature/object/quest counts.
            // Collapses the most common multi-step inspection into a single
            // command; useful for CI reports and quick sanity checks.
            std::string zoneDir = argv[++i];
            // Optional --json after the dir for machine-readable output.
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr, "zone-summary: %s does not exist\n", zoneDir.c_str());
                return 1;
            }
            auto v = wowee::editor::ContentPacker::validateZone(zoneDir);

            // Read creature/object/quest data once so both human and JSON
            // outputs share the same numbers.
            int creatureTotal = 0, hostile = 0, qg = 0, vendor = 0;
            int objectTotal = 0, m2Count = 0, wmoCount = 0;
            int questTotal = 0, chainWarnings = 0;
            std::string creaturesPath = zoneDir + "/creatures.json";
            if (fs::exists(creaturesPath)) {
                wowee::editor::NpcSpawner sp;
                if (sp.loadFromFile(creaturesPath)) {
                    creatureTotal = static_cast<int>(sp.getSpawns().size());
                    for (const auto& s : sp.getSpawns()) {
                        if (s.hostile) hostile++;
                        if (s.questgiver) qg++;
                        if (s.vendor) vendor++;
                    }
                }
            }
            std::string objectsPath = zoneDir + "/objects.json";
            if (fs::exists(objectsPath)) {
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile(objectsPath)) {
                    objectTotal = static_cast<int>(op.getObjects().size());
                    for (const auto& o : op.getObjects()) {
                        if (o.type == wowee::editor::PlaceableType::M2) m2Count++;
                        else wmoCount++;
                    }
                }
            }
            std::string questsPath = zoneDir + "/quests.json";
            if (fs::exists(questsPath)) {
                wowee::editor::QuestEditor qe;
                if (qe.loadFromFile(questsPath)) {
                    questTotal = static_cast<int>(qe.getQuests().size());
                    std::vector<std::string> errors;
                    qe.validateChains(errors);
                    chainWarnings = static_cast<int>(errors.size());
                }
            }

            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["score"] = v.openFormatScore();
                j["maxScore"] = 7;
                j["formats"] = v.summary();
                j["counts"] = {
                    {"wot", v.wotCount}, {"whm", v.whmCount},
                    {"wom", v.womCount}, {"wob", v.wobCount},
                    {"woc", v.wocCount}, {"png", v.pngCount},
                };
                j["creatures"] = {
                    {"total", creatureTotal},
                    {"hostile", hostile},
                    {"questgiver", qg},
                    {"vendor", vendor},
                };
                j["objects"] = {
                    {"total", objectTotal},
                    {"m2", m2Count},
                    {"wmo", wmoCount},
                };
                j["quests"] = {
                    {"total", questTotal},
                    {"chainWarnings", chainWarnings},
                };
                std::printf("%s\n", j.dump(2).c_str());
                return v.openFormatScore() == 7 ? 0 : 1;
            }
            std::printf("Zone: %s\n", zoneDir.c_str());
            std::printf("  open formats : %d/7  (%s)\n",
                        v.openFormatScore(), v.summary().c_str());
            std::printf("  WOT/WHM      : %d/%d   WOM: %d   WOB: %d   WOC: %d   PNG: %d\n",
                        v.wotCount, v.whmCount, v.womCount, v.wobCount,
                        v.wocCount, v.pngCount);
            if (creatureTotal > 0) {
                std::printf("  creatures    : %d  (%d hostile, %d quest, %d vendor)\n",
                            creatureTotal, hostile, qg, vendor);
            }
            if (objectTotal > 0) {
                std::printf("  objects      : %d  (%d M2, %d WMO)\n",
                            objectTotal, m2Count, wmoCount);
            }
            if (questTotal > 0) {
                std::printf("  quests       : %d  (%d chain warnings)\n",
                            questTotal, chainWarnings);
            }
            return v.openFormatScore() == 7 ? 0 : 1;
        } else if (std::strcmp(argv[i], "--info-zone-tree") == 0 && i + 1 < argc) {
            // Pretty `tree`-style hierarchical view of a zone's contents.
            // Designed for at-a-glance comprehension — what creatures,
            // what objects, what quests, what tiles, what files. No
            // --json flag because the structured equivalent is just
            // running --info-* per category and concatenating.
            std::string zoneDir = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "info-zone-tree: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "info-zone-tree: parse failed\n");
                return 1;
            }
            wowee::editor::NpcSpawner sp;
            sp.loadFromFile(zoneDir + "/creatures.json");
            wowee::editor::ObjectPlacer op;
            op.loadFromFile(zoneDir + "/objects.json");
            wowee::editor::QuestEditor qe;
            qe.loadFromFile(zoneDir + "/quests.json");
            // Walk on-disk files for the 'Files' branch.
            std::vector<std::string> diskFiles;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(zoneDir, ec)) {
                if (e.is_regular_file()) {
                    diskFiles.push_back(e.path().filename().string());
                }
            }
            std::sort(diskFiles.begin(), diskFiles.end());
            // Tree-drawing helpers — Unix box characters since most
            // terminals support UTF-8 by default. Pre-compute prefix
            // strings so leaf vs branch alignment looks right.
            auto branch = [](bool last) { return last ? "└─ " : "├─ "; };
            auto cont   = [](bool last) { return last ? "   " : "│  "; };
            std::printf("%s/\n",
                        zm.displayName.empty() ? zm.mapName.c_str()
                                                : zm.displayName.c_str());
            // Manifest section
            std::printf("├─ Manifest\n");
            std::printf("│  ├─ mapName     : %s\n", zm.mapName.c_str());
            std::printf("│  ├─ mapId       : %u\n", zm.mapId);
            std::printf("│  ├─ baseHeight  : %.1f\n", zm.baseHeight);
            std::printf("│  ├─ biome       : %s\n",
                        zm.biome.empty() ? "(unset)" : zm.biome.c_str());
            std::printf("│  └─ flags       : %s%s%s%s\n",
                        zm.allowFlying ? "fly " : "",
                        zm.pvpEnabled  ? "pvp " : "",
                        zm.isIndoor    ? "indoor " : "",
                        zm.isSanctuary ? "sanctuary " : "");
            // Tiles
            std::printf("├─ Tiles (%zu)\n", zm.tiles.size());
            for (size_t k = 0; k < zm.tiles.size(); ++k) {
                bool last = (k == zm.tiles.size() - 1);
                std::printf("│  %s(%d, %d)\n", branch(last),
                            zm.tiles[k].first, zm.tiles[k].second);
            }
            // Creatures
            std::printf("├─ Creatures (%zu)\n", sp.spawnCount());
            for (size_t k = 0; k < sp.spawnCount(); ++k) {
                bool last = (k == sp.spawnCount() - 1);
                const auto& s = sp.getSpawns()[k];
                std::printf("│  %slvl %u  %s%s\n",
                            branch(last), s.level, s.name.c_str(),
                            s.hostile ? " [hostile]" : "");
            }
            // Objects
            std::printf("├─ Objects (%zu)\n", op.getObjects().size());
            for (size_t k = 0; k < op.getObjects().size(); ++k) {
                bool last = (k == op.getObjects().size() - 1);
                const auto& o = op.getObjects()[k];
                std::printf("│  %s%s  %s\n", branch(last),
                            o.type == wowee::editor::PlaceableType::M2 ? "m2 " : "wmo",
                            o.path.c_str());
            }
            // Quests with sub-tree of objectives
            std::printf("├─ Quests (%zu)\n", qe.questCount());
            using OT = wowee::editor::QuestObjectiveType;
            auto typeName = [](OT t) {
                switch (t) {
                    case OT::KillCreature: return "kill";
                    case OT::CollectItem:  return "collect";
                    case OT::TalkToNPC:    return "talk";
                    case OT::ExploreArea:  return "explore";
                    case OT::EscortNPC:    return "escort";
                    case OT::UseObject:    return "use";
                }
                return "?";
            };
            for (size_t k = 0; k < qe.questCount(); ++k) {
                bool lastQ = (k == qe.questCount() - 1);
                const auto& q = qe.getQuests()[k];
                std::printf("│  %s[%u] %s (lvl %u, %u XP)\n",
                            branch(lastQ), q.id, q.title.c_str(),
                            q.requiredLevel, q.reward.xp);
                // Objectives indented under the quest. Use 'cont' for
                // the prior column so vertical bars align.
                for (size_t o = 0; o < q.objectives.size(); ++o) {
                    bool lastO = (o == q.objectives.size() - 1 &&
                                  q.reward.itemRewards.empty());
                    const auto& obj = q.objectives[o];
                    std::printf("│  %s%s%s ×%u %s\n",
                                cont(lastQ), branch(lastO),
                                typeName(obj.type), obj.targetCount,
                                obj.targetName.c_str());
                }
                for (size_t r = 0; r < q.reward.itemRewards.size(); ++r) {
                    bool lastR = (r == q.reward.itemRewards.size() - 1);
                    std::printf("│  %s%sreward: %s\n",
                                cont(lastQ), branch(lastR),
                                q.reward.itemRewards[r].c_str());
                }
            }
            // Files (last top-level branch — uses └─)
            std::printf("└─ Files (%zu)\n", diskFiles.size());
            for (size_t k = 0; k < diskFiles.size(); ++k) {
                bool last = (k == diskFiles.size() - 1);
                std::printf("   %s%s\n", branch(last), diskFiles[k].c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-project-tree") == 0 && i + 1 < argc) {
            // Project-level tree view: every zone with quick counts +
            // bake/viewer status. --info-zone-tree drills into one zone;
            // this gives the bird's-eye view across the whole project.
            std::string projectDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "info-project-tree: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            struct ZE {
                std::string name, dir, mapName;
                int tiles = 0, creatures = 0, objects = 0, quests = 0;
                bool hasGlb = false, hasObj = false, hasStl = false;
                bool hasHtml = false, hasZoneMd = false;
            };
            std::vector<ZE> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                wowee::editor::ZoneManifest zm;
                if (!zm.load((entry.path() / "zone.json").string())) continue;
                ZE z;
                z.name = zm.displayName.empty() ? zm.mapName : zm.displayName;
                z.dir = entry.path().filename().string();
                z.mapName = zm.mapName;
                z.tiles = static_cast<int>(zm.tiles.size());
                wowee::editor::NpcSpawner sp;
                if (sp.loadFromFile((entry.path() / "creatures.json").string())) {
                    z.creatures = static_cast<int>(sp.spawnCount());
                }
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile((entry.path() / "objects.json").string())) {
                    z.objects = static_cast<int>(op.getObjects().size());
                }
                wowee::editor::QuestEditor qe;
                if (qe.loadFromFile((entry.path() / "quests.json").string())) {
                    z.quests = static_cast<int>(qe.questCount());
                }
                z.hasGlb  = fs::exists(entry.path() / (zm.mapName + ".glb"));
                z.hasObj  = fs::exists(entry.path() / (zm.mapName + ".obj"));
                z.hasStl  = fs::exists(entry.path() / (zm.mapName + ".stl"));
                z.hasHtml = fs::exists(entry.path() / (zm.mapName + ".html"));
                z.hasZoneMd = fs::exists(entry.path() / "ZONE.md");
                zones.push_back(std::move(z));
            }
            std::sort(zones.begin(), zones.end(),
                      [](const ZE& a, const ZE& b) { return a.name < b.name; });
            int totalTiles = 0, totalCreat = 0, totalObj = 0, totalQuest = 0;
            for (const auto& z : zones) {
                totalTiles += z.tiles; totalCreat += z.creatures;
                totalObj += z.objects; totalQuest += z.quests;
            }
            std::printf("%s/  (%zu zones, %d tiles, %d creatures, %d objects, %d quests)\n",
                        projectDir.c_str(), zones.size(),
                        totalTiles, totalCreat, totalObj, totalQuest);
            for (size_t k = 0; k < zones.size(); ++k) {
                bool lastZ = (k == zones.size() - 1);
                const auto& z = zones[k];
                const char* zBranch = lastZ ? "└─ " : "├─ ";
                const char* zCont   = lastZ ? "   " : "│  ";
                std::printf("%s%s/  (tiles=%d, creat=%d, obj=%d, quest=%d)\n",
                            zBranch, z.dir.c_str(),
                            z.tiles, z.creatures, z.objects, z.quests);
                // Artifact status row — quick visual of what's been baked.
                std::printf("%s├─ name      : %s\n", zCont, z.name.c_str());
                std::printf("%s├─ mapName   : %s\n", zCont, z.mapName.c_str());
                std::printf("%s├─ artifacts : %s%s%s%s%s%s\n", zCont,
                            z.hasGlb  ? ".glb "  : "",
                            z.hasObj  ? ".obj "  : "",
                            z.hasStl  ? ".stl "  : "",
                            z.hasHtml ? ".html " : "",
                            z.hasZoneMd ? "ZONE.md " : "",
                            (!z.hasGlb && !z.hasObj && !z.hasStl &&
                             !z.hasHtml && !z.hasZoneMd) ? "(none)" : "");
                std::printf("%s└─ status    : %s\n", zCont,
                            (z.creatures || z.objects || z.quests) ?
                                "populated" : "empty (only terrain)");
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-zone-bytes") == 0 && i + 1 < argc) {
            // Per-file size breakdown grouped by category, sorted by size
            // descending. Useful for capacity planning ('which file is
            // 80% of my zone?') and pre-strip-zone audits ('how much
            // would --strip-zone free?'). --zone-stats aggregates across
            // multiple zones; this drills into one zone's contents.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr,
                    "info-zone-bytes: %s does not exist\n", zoneDir.c_str());
                return 1;
            }
            // Categorize by extension into source vs derived buckets so
            // the breakdown surfaces what would be stripped.
            struct Entry {
                std::string path;  // relative to zoneDir
                uint64_t bytes;
                std::string category;
            };
            std::vector<Entry> entries;
            uint64_t totalBytes = 0;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file()) continue;
                std::string ext = e.path().extension().string();
                std::string name = e.path().filename().string();
                std::string rel = fs::relative(e.path(), zoneDir, ec).string();
                if (ec) rel = e.path().string();
                std::string cat;
                if (ext == ".whm" || ext == ".wot" || ext == ".woc") cat = "terrain";
                else if (ext == ".wom") cat = "model (open)";
                else if (ext == ".wob") cat = "building (open)";
                else if (ext == ".m2" || ext == ".skin") cat = "model (proprietary)";
                else if (ext == ".wmo") cat = "building (proprietary)";
                else if (ext == ".blp") cat = "texture (proprietary)";
                else if (ext == ".png") cat = "texture (open/derived)";
                else if (ext == ".dbc") cat = "DBC (proprietary)";
                else if (ext == ".json") cat = "json (source)";
                else if (ext == ".glb" || ext == ".obj" || ext == ".stl") cat = "3D export (derived)";
                else if (ext == ".html" || ext == ".dot" || ext == ".csv") cat = "doc (derived)";
                else if (name == "ZONE.md" || name == "DEPS.md") cat = "doc (derived)";
                else cat = "other";
                uint64_t sz = e.file_size(ec);
                if (ec) continue;
                totalBytes += sz;
                entries.push_back({rel, sz, cat});
            }
            // Sort largest first so the heaviest contributors are at the
            // top of the table.
            std::sort(entries.begin(), entries.end(),
                      [](const Entry& a, const Entry& b) { return a.bytes > b.bytes; });
            // Aggregate per-category for the summary footer.
            std::map<std::string, std::pair<uint64_t, int>> byCategory;
            for (const auto& e : entries) {
                byCategory[e.category].first += e.bytes;
                byCategory[e.category].second++;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["totalBytes"] = totalBytes;
                j["fileCount"] = entries.size();
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& e : entries) {
                    arr.push_back({{"path", e.path},
                                   {"bytes", e.bytes},
                                   {"category", e.category}});
                }
                j["files"] = arr;
                nlohmann::json catObj;
                for (const auto& [c, p] : byCategory) {
                    catObj[c] = {{"bytes", p.first}, {"count", p.second}};
                }
                j["byCategory"] = catObj;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone bytes: %s\n", zoneDir.c_str());
            std::printf("  total: %llu bytes (%.1f KB) across %zu file(s)\n",
                        static_cast<unsigned long long>(totalBytes),
                        totalBytes / 1024.0, entries.size());
            std::printf("\n  Per-file (largest first):\n");
            std::printf("  %-50s %12s  category\n", "path", "bytes");
            for (const auto& e : entries) {
                std::printf("  %-50s %12llu  %s\n",
                            e.path.substr(0, 50).c_str(),
                            static_cast<unsigned long long>(e.bytes),
                            e.category.c_str());
            }
            std::printf("\n  Per-category:\n");
            for (const auto& [c, p] : byCategory) {
                std::printf("  %-26s %4d files  %12llu bytes  (%5.1f%%)\n",
                            c.c_str(), p.second,
                            static_cast<unsigned long long>(p.first),
                            totalBytes ? (100.0 * p.first / totalBytes) : 0.0);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-project-bytes") == 0 && i + 1 < argc) {
            // Project-wide byte audit. Walks every zone in projectDir,
            // re-uses --info-zone-bytes' categorization, and prints a
            // per-zone breakdown table plus aggregated category totals.
            // The headline number is the proprietary-vs-open size split
            // — surfaces how much disk a project still spends on .m2/
            // .wmo/.blp/.dbc payloads vs the open WOM/WOB/PNG/JSON
            // replacements.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "info-project-bytes: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            // Same categorizer used by --info-zone-bytes — keep in sync
            // if categories evolve there.
            auto categorize = [](const fs::path& p) -> std::string {
                std::string ext = p.extension().string();
                std::string name = p.filename().string();
                if (ext == ".whm" || ext == ".wot" || ext == ".woc") return "terrain";
                if (ext == ".wom") return "model (open)";
                if (ext == ".wob") return "building (open)";
                if (ext == ".m2" || ext == ".skin") return "model (proprietary)";
                if (ext == ".wmo") return "building (proprietary)";
                if (ext == ".blp") return "texture (proprietary)";
                if (ext == ".png") return "texture (open/derived)";
                if (ext == ".dbc") return "DBC (proprietary)";
                if (ext == ".json") return "json (source)";
                if (ext == ".glb" || ext == ".obj" || ext == ".stl") return "3D export (derived)";
                if (ext == ".html" || ext == ".dot" || ext == ".csv") return "doc (derived)";
                if (name == "ZONE.md" || name == "DEPS.md") return "doc (derived)";
                return "other";
            };
            // The proprietary-vs-open split is a key quality metric for
            // the open-format migration push. Anything tagged "(open)"
            // or "(open/derived)" counts toward open; anything tagged
            // "(proprietary)" counts toward proprietary; everything
            // else ("terrain" / "json (source)" / derived docs) is
            // neutral.
            auto isOpen = [](const std::string& cat) {
                return cat.find("(open") != std::string::npos;
            };
            auto isProprietary = [](const std::string& cat) {
                return cat.find("(proprietary)") != std::string::npos;
            };
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            struct ZRow {
                std::string name;
                uint64_t totalBytes = 0;
                int fileCount = 0;
                uint64_t openBytes = 0;
                uint64_t propBytes = 0;
            };
            std::vector<ZRow> rows;
            std::map<std::string, std::pair<uint64_t, int>> globalCat;
            uint64_t projectBytes = 0;
            int projectFiles = 0;
            for (const auto& zoneDir : zones) {
                ZRow r;
                r.name = fs::path(zoneDir).filename().string();
                std::error_code ec;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    uint64_t sz = e.file_size(ec);
                    if (ec) continue;
                    std::string cat = categorize(e.path());
                    r.totalBytes += sz;
                    r.fileCount++;
                    if (isOpen(cat)) r.openBytes += sz;
                    else if (isProprietary(cat)) r.propBytes += sz;
                    globalCat[cat].first += sz;
                    globalCat[cat].second++;
                }
                projectBytes += r.totalBytes;
                projectFiles += r.fileCount;
                rows.push_back(r);
            }
            uint64_t globalOpen = 0, globalProp = 0;
            for (const auto& [c, p] : globalCat) {
                if (isOpen(c)) globalOpen += p.first;
                else if (isProprietary(c)) globalProp += p.first;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["project"] = projectDir;
                j["totalBytes"] = projectBytes;
                j["fileCount"] = projectFiles;
                j["openBytes"] = globalOpen;
                j["proprietaryBytes"] = globalProp;
                nlohmann::json zarr = nlohmann::json::array();
                for (const auto& r : rows) {
                    zarr.push_back({{"name", r.name},
                                    {"totalBytes", r.totalBytes},
                                    {"fileCount", r.fileCount},
                                    {"openBytes", r.openBytes},
                                    {"proprietaryBytes", r.propBytes}});
                }
                j["zones"] = zarr;
                nlohmann::json catObj;
                for (const auto& [c, p] : globalCat) {
                    catObj[c] = {{"bytes", p.first}, {"count", p.second}};
                }
                j["byCategory"] = catObj;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Project bytes: %s\n", projectDir.c_str());
            std::printf("  total : %llu bytes (%.1f KB) across %d file(s) in %zu zone(s)\n",
                        static_cast<unsigned long long>(projectBytes),
                        projectBytes / 1024.0, projectFiles, zones.size());
            std::printf("\n  zone                   files       bytes   open(B)  prop(B)\n");
            for (const auto& r : rows) {
                std::printf("  %-22s %5d  %10llu  %8llu  %7llu\n",
                            r.name.substr(0, 22).c_str(),
                            r.fileCount,
                            static_cast<unsigned long long>(r.totalBytes),
                            static_cast<unsigned long long>(r.openBytes),
                            static_cast<unsigned long long>(r.propBytes));
            }
            std::printf("\n  Per-category (project-wide):\n");
            for (const auto& [c, p] : globalCat) {
                std::printf("  %-26s %4d files  %12llu bytes  (%5.1f%%)\n",
                            c.c_str(), p.second,
                            static_cast<unsigned long long>(p.first),
                            projectBytes ? (100.0 * p.first / projectBytes) : 0.0);
            }
            std::printf("\n  Open-vs-proprietary split:\n");
            std::printf("    open         : %12llu bytes\n",
                        static_cast<unsigned long long>(globalOpen));
            std::printf("    proprietary  : %12llu bytes\n",
                        static_cast<unsigned long long>(globalProp));
            uint64_t denom = globalOpen + globalProp;
            if (denom > 0) {
                std::printf("    open share   : %5.1f%%\n", 100.0 * globalOpen / denom);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-zone-extents") == 0 && i + 1 < argc) {
            // Compute the zone's spatial bounding box. XY from manifest
            // tile coords (each tile is 533.33 yards); Z from height
            // range across all loaded chunks. Useful for sizing the
            // camera frustum, planning where new tiles can fit
            // contiguously, or quick sanity-checks ('this zone is 4km
            // across? that seems wrong').
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "info-zone-extents: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "info-zone-extents: parse failed\n");
                return 1;
            }
            // Tile XY range — straightforward integer min/max.
            int tileMinX = 64, tileMaxX = -1;
            int tileMinY = 64, tileMaxY = -1;
            for (const auto& [tx, ty] : zm.tiles) {
                tileMinX = std::min(tileMinX, tx);
                tileMaxX = std::max(tileMaxX, tx);
                tileMinY = std::min(tileMinY, ty);
                tileMaxY = std::max(tileMaxY, ty);
            }
            // Z range from loaded chunks. Walk every WHM tile; this is
            // the same scan --info-whm does per-tile but rolled up.
            float zMin = 1e30f, zMax = -1e30f;
            int loadedTiles = 0, missingTiles = 0;
            for (const auto& [tx, ty] : zm.tiles) {
                std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                        std::to_string(tx) + "_" + std::to_string(ty);
                if (!wowee::pipeline::WoweeTerrainLoader::exists(tileBase)) {
                    missingTiles++;
                    continue;
                }
                wowee::pipeline::ADTTerrain terrain;
                wowee::pipeline::WoweeTerrainLoader::load(tileBase, terrain);
                loadedTiles++;
                for (const auto& chunk : terrain.chunks) {
                    if (!chunk.heightMap.isLoaded()) continue;
                    float baseZ = chunk.position[2];
                    for (float h : chunk.heightMap.heights) {
                        if (!std::isfinite(h)) continue;
                        zMin = std::min(zMin, baseZ + h);
                        zMax = std::max(zMax, baseZ + h);
                    }
                }
            }
            if (zMin > zMax) { zMin = 0; zMax = 0; }
            // Convert tile coords to world-space yards. WoW grid centers
            // tile (32, 32) at world origin; +X tile = -X world (north),
            // +Y tile = -Y world (west).
            constexpr float kTileSize = 533.33333f;
            float worldMinX = (32.0f - tileMaxY - 1) * kTileSize;
            float worldMaxX = (32.0f - tileMinY)     * kTileSize;
            float worldMinY = (32.0f - tileMaxX - 1) * kTileSize;
            float worldMaxY = (32.0f - tileMinX)     * kTileSize;
            float widthX = worldMaxX - worldMinX;
            float widthY = worldMaxY - worldMinY;
            float heightZ = zMax - zMin;
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["tileCount"] = zm.tiles.size();
                j["loadedTiles"] = loadedTiles;
                j["missingTiles"] = missingTiles;
                j["tileRange"] = {{"x", {tileMinX, tileMaxX}},
                                   {"y", {tileMinY, tileMaxY}}};
                j["worldBox"] = {{"min", {worldMinX, worldMinY, zMin}},
                                  {"max", {worldMaxX, worldMaxY, zMax}}};
                j["sizeYards"] = {widthX, widthY, heightZ};
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone extents: %s\n", zoneDir.c_str());
            std::printf("  tile count   : %zu (%d loaded, %d missing on disk)\n",
                        zm.tiles.size(), loadedTiles, missingTiles);
            if (zm.tiles.empty()) {
                std::printf("  *no tiles in manifest*\n");
                return 0;
            }
            std::printf("  tile range   : x=[%d, %d]  y=[%d, %d]\n",
                        tileMinX, tileMaxX, tileMinY, tileMaxY);
            std::printf("  world box    : (%.1f, %.1f, %.1f) - (%.1f, %.1f, %.1f) yards\n",
                        worldMinX, worldMinY, zMin,
                        worldMaxX, worldMaxY, zMax);
            std::printf("  size         : %.1f x %.1f x %.1f yards (%.0fm x %.0fm x %.1fm)\n",
                        widthX, widthY, heightZ,
                        widthX * 0.9144f, widthY * 0.9144f, heightZ * 0.9144f);
            return 0;
        } else if (std::strcmp(argv[i], "--info-project-extents") == 0 && i + 1 < argc) {
            // Combined spatial bounding box across every zone in
            // <projectDir>. Per-zone XY tile range + Z height range,
            // unioned into a project-wide world box. Useful for
            // understanding total project area, sizing the world map
            // overview, or sanity-checking that zones don't overlap
            // (the union should equal the sum of disjoint per-zone
            // boxes).
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "info-project-extents: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            constexpr float kTileSize = 533.33333f;
            struct ZBox {
                std::string name;
                int tileCount = 0;
                float wMinX = 1e30f, wMaxX = -1e30f;
                float wMinY = 1e30f, wMaxY = -1e30f;
                float zMin = 1e30f, zMax = -1e30f;
            };
            std::vector<ZBox> rows;
            float gMinX = 1e30f, gMaxX = -1e30f;
            float gMinY = 1e30f, gMaxY = -1e30f;
            float gZMin = 1e30f, gZMax = -1e30f;
            int totalTiles = 0;
            for (const auto& zoneDir : zones) {
                ZBox b;
                b.name = fs::path(zoneDir).filename().string();
                wowee::editor::ZoneManifest zm;
                if (!zm.load(zoneDir + "/zone.json")) {
                    rows.push_back(b);
                    continue;
                }
                b.tileCount = static_cast<int>(zm.tiles.size());
                if (zm.tiles.empty()) {
                    rows.push_back(b);
                    continue;
                }
                int tMinX = 64, tMaxX = -1, tMinY = 64, tMaxY = -1;
                for (const auto& [tx, ty] : zm.tiles) {
                    tMinX = std::min(tMinX, tx);
                    tMaxX = std::max(tMaxX, tx);
                    tMinY = std::min(tMinY, ty);
                    tMaxY = std::max(tMaxY, ty);
                }
                b.wMinX = (32.0f - tMaxY - 1) * kTileSize;
                b.wMaxX = (32.0f - tMinY)     * kTileSize;
                b.wMinY = (32.0f - tMaxX - 1) * kTileSize;
                b.wMaxY = (32.0f - tMinX)     * kTileSize;
                for (const auto& [tx, ty] : zm.tiles) {
                    std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                            std::to_string(tx) + "_" + std::to_string(ty);
                    if (!wowee::pipeline::WoweeTerrainLoader::exists(tileBase)) continue;
                    wowee::pipeline::ADTTerrain terrain;
                    wowee::pipeline::WoweeTerrainLoader::load(tileBase, terrain);
                    for (const auto& chunk : terrain.chunks) {
                        if (!chunk.heightMap.isLoaded()) continue;
                        float baseZ = chunk.position[2];
                        for (float h : chunk.heightMap.heights) {
                            if (!std::isfinite(h)) continue;
                            b.zMin = std::min(b.zMin, baseZ + h);
                            b.zMax = std::max(b.zMax, baseZ + h);
                        }
                    }
                }
                if (b.zMin > b.zMax) { b.zMin = 0; b.zMax = 0; }
                gMinX = std::min(gMinX, b.wMinX);
                gMaxX = std::max(gMaxX, b.wMaxX);
                gMinY = std::min(gMinY, b.wMinY);
                gMaxY = std::max(gMaxY, b.wMaxY);
                gZMin = std::min(gZMin, b.zMin);
                gZMax = std::max(gZMax, b.zMax);
                totalTiles += b.tileCount;
                rows.push_back(b);
            }
            if (totalTiles == 0) {
                gMinX = gMaxX = gMinY = gMaxY = gZMin = gZMax = 0.0f;
            }
            float gWidthX = gMaxX - gMinX;
            float gWidthY = gMaxY - gMinY;
            float gHeightZ = gZMax - gZMin;
            if (jsonOut) {
                nlohmann::json j;
                j["project"] = projectDir;
                j["zoneCount"] = zones.size();
                j["totalTiles"] = totalTiles;
                j["worldBox"] = {{"min", {gMinX, gMinY, gZMin}},
                                  {"max", {gMaxX, gMaxY, gZMax}}};
                j["sizeYards"] = {gWidthX, gWidthY, gHeightZ};
                nlohmann::json zarr = nlohmann::json::array();
                for (const auto& b : rows) {
                    zarr.push_back({{"name", b.name},
                                    {"tileCount", b.tileCount},
                                    {"worldBox", {{"min", {b.wMinX, b.wMinY, b.zMin}},
                                                   {"max", {b.wMaxX, b.wMaxY, b.zMax}}}}});
                }
                j["zones"] = zarr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Project extents: %s\n", projectDir.c_str());
            std::printf("  zones        : %zu\n", zones.size());
            std::printf("  total tiles  : %d\n", totalTiles);
            if (totalTiles == 0) {
                std::printf("  *no tiles in any zone manifest*\n");
                return 0;
            }
            std::printf("  world union  : (%.1f, %.1f, %.1f) - (%.1f, %.1f, %.1f) yards\n",
                        gMinX, gMinY, gZMin, gMaxX, gMaxY, gZMax);
            std::printf("  total size   : %.1f x %.1f x %.1f yards (%.0fm x %.0fm x %.1fm)\n",
                        gWidthX, gWidthY, gHeightZ,
                        gWidthX * 0.9144f, gWidthY * 0.9144f, gHeightZ * 0.9144f);
            std::printf("\n  zone                  tiles      worldX (min..max)        worldY (min..max)\n");
            for (const auto& b : rows) {
                if (b.tileCount == 0) {
                    std::printf("  %-20s  %5d  (no tiles)\n",
                                b.name.substr(0, 20).c_str(), b.tileCount);
                    continue;
                }
                std::printf("  %-20s  %5d  %9.1f .. %9.1f   %9.1f .. %9.1f\n",
                            b.name.substr(0, 20).c_str(), b.tileCount,
                            b.wMinX, b.wMaxX, b.wMinY, b.wMaxY);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-zone-water") == 0 && i + 1 < argc) {
            // Aggregate water-layer stats across all tiles in a zone.
            // Useful for confirming a 'lake zone' actually has water,
            // or for budget planning ('how many MH2O cells does my
            // archipelago zone carry?'). Liquid types: 0=water,
            // 1=ocean, 2=magma, 3=slime.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "info-zone-water: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "info-zone-water: parse failed\n");
                return 1;
            }
            int waterChunks = 0, totalLayers = 0;
            std::map<uint16_t, int> typeHist;  // liquidType -> chunk count
            float minH = 1e30f, maxH = -1e30f;
            int loadedTiles = 0;
            for (const auto& [tx, ty] : zm.tiles) {
                std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                        std::to_string(tx) + "_" + std::to_string(ty);
                if (!wowee::pipeline::WoweeTerrainLoader::exists(tileBase)) continue;
                wowee::pipeline::ADTTerrain terrain;
                wowee::pipeline::WoweeTerrainLoader::load(tileBase, terrain);
                loadedTiles++;
                for (size_t c = 0; c < terrain.waterData.size(); ++c) {
                    const auto& w = terrain.waterData[c];
                    if (!w.hasWater()) continue;
                    waterChunks++;
                    totalLayers += static_cast<int>(w.layers.size());
                    for (const auto& layer : w.layers) {
                        typeHist[layer.liquidType]++;
                        minH = std::min(minH, layer.minHeight);
                        maxH = std::max(maxH, layer.maxHeight);
                    }
                }
            }
            if (waterChunks == 0) { minH = 0; maxH = 0; }
            auto typeName = [](uint16_t t) {
                switch (t) {
                    case 0: return "water";
                    case 1: return "ocean";
                    case 2: return "magma";
                    case 3: return "slime";
                }
                return "?";
            };
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["loadedTiles"] = loadedTiles;
                j["waterChunks"] = waterChunks;
                j["totalLayers"] = totalLayers;
                j["heightRange"] = {minH, maxH};
                nlohmann::json types = nlohmann::json::array();
                for (const auto& [t, c] : typeHist) {
                    types.push_back({{"type", t}, {"name", typeName(t)}, {"layerCount", c}});
                }
                j["types"] = types;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone water: %s\n", zoneDir.c_str());
            std::printf("  loaded tiles : %d\n", loadedTiles);
            std::printf("  water chunks : %d (out of %d possible)\n",
                        waterChunks, loadedTiles * 256);
            std::printf("  total layers : %d\n", totalLayers);
            if (waterChunks > 0) {
                std::printf("  height range : %.2f to %.2f\n", minH, maxH);
                std::printf("\n  By liquid type:\n");
                for (const auto& [t, c] : typeHist) {
                    std::printf("    %s (%u): %d layer(s)\n",
                                typeName(t), t, c);
                }
            } else {
                std::printf("  (no water in this zone)\n");
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-project-water") == 0 && i + 1 < argc) {
            // Project-wide water rollup. Walks every zone in projectDir,
            // sums water chunks/layers/types per zone, then totals
            // across the project. Useful for "do my coastal zones
            // actually carry ocean data" sanity checks and for budget
            // planning when many zones share liquid types.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "info-project-water: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            auto typeName = [](uint16_t t) {
                switch (t) {
                    case 0: return "water";
                    case 1: return "ocean";
                    case 2: return "magma";
                    case 3: return "slime";
                }
                return "?";
            };
            struct ZRow {
                std::string name;
                int loadedTiles = 0, waterChunks = 0, totalLayers = 0;
                std::map<uint16_t, int> typeHist;
            };
            std::vector<ZRow> rows;
            int gLoadedTiles = 0, gWaterChunks = 0, gTotalLayers = 0;
            std::map<uint16_t, int> gTypeHist;
            float gMinH = 1e30f, gMaxH = -1e30f;
            for (const auto& zoneDir : zones) {
                ZRow r;
                r.name = fs::path(zoneDir).filename().string();
                wowee::editor::ZoneManifest zm;
                if (!zm.load(zoneDir + "/zone.json")) {
                    rows.push_back(r);
                    continue;
                }
                for (const auto& [tx, ty] : zm.tiles) {
                    std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                            std::to_string(tx) + "_" + std::to_string(ty);
                    if (!wowee::pipeline::WoweeTerrainLoader::exists(tileBase)) continue;
                    wowee::pipeline::ADTTerrain terrain;
                    wowee::pipeline::WoweeTerrainLoader::load(tileBase, terrain);
                    r.loadedTiles++;
                    for (const auto& w : terrain.waterData) {
                        if (!w.hasWater()) continue;
                        r.waterChunks++;
                        r.totalLayers += static_cast<int>(w.layers.size());
                        for (const auto& layer : w.layers) {
                            r.typeHist[layer.liquidType]++;
                            gMinH = std::min(gMinH, layer.minHeight);
                            gMaxH = std::max(gMaxH, layer.maxHeight);
                        }
                    }
                }
                gLoadedTiles += r.loadedTiles;
                gWaterChunks += r.waterChunks;
                gTotalLayers += r.totalLayers;
                for (const auto& [t, c] : r.typeHist) gTypeHist[t] += c;
                rows.push_back(r);
            }
            if (gWaterChunks == 0) { gMinH = 0; gMaxH = 0; }
            if (jsonOut) {
                nlohmann::json j;
                j["project"] = projectDir;
                j["zoneCount"] = zones.size();
                j["loadedTiles"] = gLoadedTiles;
                j["waterChunks"] = gWaterChunks;
                j["totalLayers"] = gTotalLayers;
                j["heightRange"] = {gMinH, gMaxH};
                nlohmann::json zarr = nlohmann::json::array();
                for (const auto& r : rows) {
                    nlohmann::json types = nlohmann::json::array();
                    for (const auto& [t, c] : r.typeHist) {
                        types.push_back({{"type", t}, {"name", typeName(t)},
                                         {"layerCount", c}});
                    }
                    zarr.push_back({{"name", r.name},
                                    {"loadedTiles", r.loadedTiles},
                                    {"waterChunks", r.waterChunks},
                                    {"totalLayers", r.totalLayers},
                                    {"types", types}});
                }
                j["zones"] = zarr;
                nlohmann::json gtypes = nlohmann::json::array();
                for (const auto& [t, c] : gTypeHist) {
                    gtypes.push_back({{"type", t}, {"name", typeName(t)},
                                      {"layerCount", c}});
                }
                j["types"] = gtypes;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Project water: %s\n", projectDir.c_str());
            std::printf("  zones        : %zu\n", zones.size());
            std::printf("  loaded tiles : %d\n", gLoadedTiles);
            std::printf("  water chunks : %d (out of %d possible)\n",
                        gWaterChunks, gLoadedTiles * 256);
            std::printf("  total layers : %d\n", gTotalLayers);
            if (gWaterChunks > 0) {
                std::printf("  height range : %.2f to %.2f\n", gMinH, gMaxH);
                std::printf("\n  By liquid type (project-wide):\n");
                for (const auto& [t, c] : gTypeHist) {
                    std::printf("    %s (%u): %d layer(s)\n",
                                typeName(t), t, c);
                }
            }
            std::printf("\n  zone                  tiles  water-chunks  layers\n");
            for (const auto& r : rows) {
                std::printf("  %-20s  %5d  %12d  %6d\n",
                            r.name.substr(0, 20).c_str(),
                            r.loadedTiles, r.waterChunks, r.totalLayers);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-zone-density") == 0 && i + 1 < argc) {
            // Per-tile content density. Catches sparse zones (5 mobs
            // across 16 tiles → boring) and over-stuffed ones (200 mobs
            // in 1 tile → frame-rate bomb). Per-tile bucket uses tile
            // (tx, ty) computed from world position by reversing the
            // WoW grid transform.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "info-zone-density: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "info-zone-density: parse failed\n");
                return 1;
            }
            // Per-(tx, ty) bucket of counts.
            struct TileBucket { int creatures = 0, objects = 0; };
            std::map<std::pair<int,int>, TileBucket> tiles;
            for (const auto& [tx, ty] : zm.tiles) tiles[{tx, ty}] = {};
            // Reverse the WoW grid transform: world (X, Y) -> tile (tx, ty).
            // From --info-zone-extents:
            //   worldX = (32 - tileY) * 533.33 - subX
            //   worldY = (32 - tileX) * 533.33 - subY
            // So:
            //   tileX = floor(32 - worldY / 533.33)
            //   tileY = floor(32 - worldX / 533.33)
            constexpr float kTileSize = 533.33333f;
            auto worldToTile = [](float wx, float wy) -> std::pair<int,int> {
                int tx = static_cast<int>(std::floor(32.0f - wy / kTileSize));
                int ty = static_cast<int>(std::floor(32.0f - wx / kTileSize));
                return {tx, ty};
            };
            wowee::editor::NpcSpawner sp;
            int totalCreat = 0;
            if (sp.loadFromFile(zoneDir + "/creatures.json")) {
                totalCreat = static_cast<int>(sp.spawnCount());
                for (const auto& s : sp.getSpawns()) {
                    auto t = worldToTile(s.position.x, s.position.y);
                    auto it = tiles.find(t);
                    if (it != tiles.end()) it->second.creatures++;
                    // Out-of-zone spawns silently dropped — they'll
                    // surface in --check-zone-refs / --check-zone-content.
                }
            }
            wowee::editor::ObjectPlacer op;
            int totalObj = 0;
            if (op.loadFromFile(zoneDir + "/objects.json")) {
                totalObj = static_cast<int>(op.getObjects().size());
                for (const auto& o : op.getObjects()) {
                    auto t = worldToTile(o.position.x, o.position.y);
                    auto it = tiles.find(t);
                    if (it != tiles.end()) it->second.objects++;
                }
            }
            wowee::editor::QuestEditor qe;
            int totalQ = 0;
            if (qe.loadFromFile(zoneDir + "/quests.json")) {
                totalQ = static_cast<int>(qe.questCount());
            }
            int tileCount = static_cast<int>(tiles.size());
            double avgCreatPerTile = tileCount > 0 ? double(totalCreat) / tileCount : 0.0;
            double avgObjPerTile = tileCount > 0 ? double(totalObj) / tileCount : 0.0;
            double questsPerTile = tileCount > 0 ? double(totalQ) / tileCount : 0.0;
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["tileCount"] = tileCount;
                j["totals"] = {{"creatures", totalCreat},
                                {"objects", totalObj},
                                {"quests", totalQ}};
                j["averages"] = {{"creaturesPerTile", avgCreatPerTile},
                                  {"objectsPerTile", avgObjPerTile},
                                  {"questsPerTile", questsPerTile}};
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& [coord, b] : tiles) {
                    arr.push_back({{"tile", {coord.first, coord.second}},
                                    {"creatures", b.creatures},
                                    {"objects", b.objects}});
                }
                j["perTile"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone density: %s\n", zoneDir.c_str());
            std::printf("  tiles      : %d\n", tileCount);
            std::printf("  totals     : %d creatures, %d objects, %d quests\n",
                        totalCreat, totalObj, totalQ);
            std::printf("  per-tile   : %.2f creatures, %.2f objects, %.2f quests\n",
                        avgCreatPerTile, avgObjPerTile, questsPerTile);
            std::printf("\n  Per-tile breakdown:\n");
            std::printf("    tile        creatures  objects\n");
            for (const auto& [coord, b] : tiles) {
                std::printf("    (%2d, %2d)         %5d    %5d\n",
                            coord.first, coord.second, b.creatures, b.objects);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-project-density") == 0 && i + 1 < argc) {
            // Project-wide content density. Sums creatures/objects/
            // quests across every zone, computes per-tile averages
            // both per-zone and project-wide. Helps spot zones that
            // are abnormally sparse vs the project median, and
            // surfaces the project's overall content footprint.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "info-project-density: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            struct ZRow {
                std::string name;
                int tileCount = 0;
                int creatures = 0, objects = 0, quests = 0;
            };
            std::vector<ZRow> rows;
            int gTiles = 0, gCreat = 0, gObj = 0, gQ = 0;
            for (const auto& zoneDir : zones) {
                ZRow r;
                r.name = fs::path(zoneDir).filename().string();
                wowee::editor::ZoneManifest zm;
                if (zm.load(zoneDir + "/zone.json")) {
                    r.tileCount = static_cast<int>(zm.tiles.size());
                }
                wowee::editor::NpcSpawner sp;
                if (sp.loadFromFile(zoneDir + "/creatures.json")) {
                    r.creatures = static_cast<int>(sp.spawnCount());
                }
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile(zoneDir + "/objects.json")) {
                    r.objects = static_cast<int>(op.getObjects().size());
                }
                wowee::editor::QuestEditor qe;
                if (qe.loadFromFile(zoneDir + "/quests.json")) {
                    r.quests = static_cast<int>(qe.questCount());
                }
                gTiles += r.tileCount;
                gCreat += r.creatures;
                gObj += r.objects;
                gQ += r.quests;
                rows.push_back(r);
            }
            double gAvgCreat = gTiles > 0 ? double(gCreat) / gTiles : 0.0;
            double gAvgObj = gTiles > 0 ? double(gObj) / gTiles : 0.0;
            double gAvgQ = gTiles > 0 ? double(gQ) / gTiles : 0.0;
            if (jsonOut) {
                nlohmann::json j;
                j["project"] = projectDir;
                j["zoneCount"] = zones.size();
                j["totalTiles"] = gTiles;
                j["totals"] = {{"creatures", gCreat},
                                {"objects", gObj},
                                {"quests", gQ}};
                j["averages"] = {{"creaturesPerTile", gAvgCreat},
                                  {"objectsPerTile", gAvgObj},
                                  {"questsPerTile", gAvgQ}};
                nlohmann::json zarr = nlohmann::json::array();
                for (const auto& r : rows) {
                    double zCreat = r.tileCount > 0 ? double(r.creatures) / r.tileCount : 0.0;
                    double zObj = r.tileCount > 0 ? double(r.objects) / r.tileCount : 0.0;
                    zarr.push_back({{"name", r.name},
                                    {"tileCount", r.tileCount},
                                    {"creatures", r.creatures},
                                    {"objects", r.objects},
                                    {"quests", r.quests},
                                    {"creaturesPerTile", zCreat},
                                    {"objectsPerTile", zObj}});
                }
                j["zones"] = zarr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Project density: %s\n", projectDir.c_str());
            std::printf("  zones        : %zu\n", zones.size());
            std::printf("  total tiles  : %d\n", gTiles);
            std::printf("  totals       : %d creatures, %d objects, %d quests\n",
                        gCreat, gObj, gQ);
            std::printf("  per-tile     : %.2f creatures, %.2f objects, %.2f quests\n",
                        gAvgCreat, gAvgObj, gAvgQ);
            std::printf("\n  zone                  tiles   creat   obj  quest   creat/tile  obj/tile\n");
            for (const auto& r : rows) {
                double zCreat = r.tileCount > 0 ? double(r.creatures) / r.tileCount : 0.0;
                double zObj = r.tileCount > 0 ? double(r.objects) / r.tileCount : 0.0;
                std::printf("  %-20s  %5d  %5d  %4d  %5d   %9.2f   %7.2f\n",
                            r.name.substr(0, 20).c_str(),
                            r.tileCount, r.creatures, r.objects, r.quests,
                            zCreat, zObj);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--export-zone-summary-md") == 0 && i + 1 < argc) {
            // Render a Markdown documentation page for a zone. Useful for
            // designers tracking changes between versions, generating
            // GitHub Pages docs, or reviewing zones in PRs without
            // round-tripping through the GUI.
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "export-zone-summary-md: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr,
                    "export-zone-summary-md: failed to parse %s\n", manifestPath.c_str());
                return 1;
            }
            // Default output: ZONE.md sitting next to zone.json.
            if (outPath.empty()) outPath = zoneDir + "/ZONE.md";
            // Load content sub-files; missing ones contribute 0 entries.
            wowee::editor::NpcSpawner sp;
            sp.loadFromFile(zoneDir + "/creatures.json");
            wowee::editor::ObjectPlacer op;
            op.loadFromFile(zoneDir + "/objects.json");
            wowee::editor::QuestEditor qe;
            qe.loadFromFile(zoneDir + "/quests.json");
            std::ofstream md(outPath);
            if (!md) {
                std::fprintf(stderr,
                    "export-zone-summary-md: cannot write %s\n", outPath.c_str());
                return 1;
            }
            md << "# " << (zm.displayName.empty() ? zm.mapName : zm.displayName) << "\n\n";
            md << "*Auto-generated by `wowee_editor --export-zone-summary-md`. "
                  "Do not edit by hand.*\n\n";
            md << "## Manifest\n\n";
            md << "| Field | Value |\n";
            md << "|---|---|\n";
            md << "| Map name | `" << zm.mapName << "` |\n";
            md << "| Display name | " << zm.displayName << " |\n";
            md << "| Map ID | " << zm.mapId << " |\n";
            if (!zm.biome.empty())     md << "| Biome | " << zm.biome << " |\n";
            md << "| Base height | " << zm.baseHeight << " |\n";
            md << "| Tile count | " << zm.tiles.size() << " |\n";
            md << "| Allow flying | " << (zm.allowFlying ? "yes" : "no") << " |\n";
            md << "| PvP enabled | " << (zm.pvpEnabled ? "yes" : "no") << " |\n";
            md << "| Indoor | " << (zm.isIndoor ? "yes" : "no") << " |\n";
            md << "| Sanctuary | " << (zm.isSanctuary ? "yes" : "no") << " |\n";
            if (!zm.musicTrack.empty())   md << "| Music | `" << zm.musicTrack << "` |\n";
            if (!zm.ambienceDay.empty())  md << "| Ambient (day) | `" << zm.ambienceDay << "` |\n";
            if (!zm.ambienceNight.empty())md << "| Ambient (night) | `" << zm.ambienceNight << "` |\n";
            if (!zm.description.empty()) {
                md << "\n### Description\n\n" << zm.description << "\n";
            }
            md << "\n## Tiles\n\n";
            md << "| tx | ty |\n|---|---|\n";
            for (const auto& [tx, ty] : zm.tiles) {
                md << "| " << tx << " | " << ty << " |\n";
            }
            md << "\n## Creatures (" << sp.spawnCount() << ")\n\n";
            if (sp.spawnCount() == 0) {
                md << "*No creature spawns.*\n";
            } else {
                md << "| # | Name | Lvl | DisplayId | Pos (x, y, z) | Flags |\n";
                md << "|---|---|---|---|---|---|\n";
                for (size_t k = 0; k < sp.spawnCount(); ++k) {
                    const auto& s = sp.getSpawns()[k];
                    md << "| " << k << " | " << s.name << " | " << s.level << " | "
                       << s.displayId << " | ("
                       << s.position.x << ", " << s.position.y << ", " << s.position.z
                       << ") |";
                    if (s.hostile)    md << " hostile";
                    if (s.questgiver) md << " quest";
                    if (s.vendor)     md << " vendor";
                    if (s.trainer)    md << " trainer";
                    md << " |\n";
                }
            }
            md << "\n## Objects (" << op.getObjects().size() << ")\n\n";
            if (op.getObjects().empty()) {
                md << "*No object placements.*\n";
            } else {
                md << "| # | Type | Path | Pos | Scale |\n";
                md << "|---|---|---|---|---|\n";
                for (size_t k = 0; k < op.getObjects().size(); ++k) {
                    const auto& o = op.getObjects()[k];
                    md << "| " << k << " | "
                       << (o.type == wowee::editor::PlaceableType::M2 ? "m2" : "wmo")
                       << " | `" << o.path << "` | ("
                       << o.position.x << ", " << o.position.y << ", " << o.position.z
                       << ") | " << o.scale << " |\n";
                }
            }
            md << "\n## Quests (" << qe.questCount() << ")\n\n";
            if (qe.questCount() == 0) {
                md << "*No quests.*\n";
            } else {
                using OT = wowee::editor::QuestObjectiveType;
                auto typeName = [](OT t) {
                    switch (t) {
                        case OT::KillCreature: return "kill";
                        case OT::CollectItem:  return "collect";
                        case OT::TalkToNPC:    return "talk";
                        case OT::ExploreArea:  return "explore";
                        case OT::EscortNPC:    return "escort";
                        case OT::UseObject:    return "use";
                    }
                    return "?";
                };
                for (size_t k = 0; k < qe.questCount(); ++k) {
                    const auto& q = qe.getQuests()[k];
                    md << "### " << k << ". " << q.title << "\n\n";
                    md << "- Required level: " << q.requiredLevel << "\n";
                    md << "- Quest giver NPC ID: " << q.questGiverNpcId << "\n";
                    md << "- Turn-in NPC ID: " << q.turnInNpcId << "\n";
                    md << "- XP: " << q.reward.xp << "\n";
                    if (q.reward.gold || q.reward.silver || q.reward.copper) {
                        md << "- Coin: " << q.reward.gold << "g "
                           << q.reward.silver << "s " << q.reward.copper << "c\n";
                    }
                    if (!q.objectives.empty()) {
                        md << "- Objectives:\n";
                        for (const auto& obj : q.objectives) {
                            md << "  - **" << typeName(obj.type) << "** "
                               << obj.targetName << " ×" << obj.targetCount;
                            if (!obj.description.empty()) {
                                md << " — *" << obj.description << "*";
                            }
                            md << "\n";
                        }
                    }
                    if (!q.reward.itemRewards.empty()) {
                        md << "- Item rewards:\n";
                        for (const auto& it : q.reward.itemRewards) {
                            md << "  - `" << it << "`\n";
                        }
                    }
                    md << "\n";
                }
            }
            md.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  zone=%s, %zu tiles, %zu creatures, %zu objects, %zu quests\n",
                        zm.mapName.c_str(), zm.tiles.size(), sp.spawnCount(),
                        op.getObjects().size(), qe.questCount());
            return 0;
        } else if (std::strcmp(argv[i], "--export-zone-csv") == 0 && i + 1 < argc) {
            // Emit creatures.csv / objects.csv / quests.csv for designers
            // who prefer spreadsheets over JSON. Round-trip back into the
            // editor isn't supported yet, but for read-only analysis (sort
            // by XP, group by faction, pivot tables in LibreOffice) CSV is
            // the lingua franca of design data.
            std::string zoneDir = argv[++i];
            std::string outDir;
            if (i + 1 < argc && argv[i + 1][0] != '-') outDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "export-zone-csv: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            if (outDir.empty()) outDir = zoneDir;
            // CSV-escape: wrap any field containing comma/quote/newline in
            // double quotes; double up internal quotes per RFC 4180.
            auto csvEsc = [](const std::string& s) {
                bool needs = s.find(',') != std::string::npos ||
                             s.find('"') != std::string::npos ||
                             s.find('\n') != std::string::npos;
                if (!needs) return s;
                std::string out = "\"";
                for (char c : s) {
                    if (c == '"') out += "\"\"";
                    else out += c;
                }
                out += "\"";
                return out;
            };
            int filesWritten = 0;
            // Creatures
            wowee::editor::NpcSpawner sp;
            if (sp.loadFromFile(zoneDir + "/creatures.json")) {
                std::string out = outDir + "/creatures.csv";
                std::ofstream f(out);
                if (!f) {
                    std::fprintf(stderr, "cannot write %s\n", out.c_str());
                    return 1;
                }
                f << "index,id,name,displayId,level,health,mana,faction,"
                     "x,y,z,orientation,scale,hostile,questgiver,vendor,trainer\n";
                for (size_t k = 0; k < sp.spawnCount(); ++k) {
                    const auto& s = sp.getSpawns()[k];
                    f << k << "," << s.id << "," << csvEsc(s.name) << ","
                      << s.displayId << "," << s.level << ","
                      << s.health << "," << s.mana << "," << s.faction << ","
                      << s.position.x << "," << s.position.y << ","
                      << s.position.z << "," << s.orientation << ","
                      << s.scale << ","
                      << (s.hostile ? 1 : 0) << ","
                      << (s.questgiver ? 1 : 0) << ","
                      << (s.vendor ? 1 : 0) << ","
                      << (s.trainer ? 1 : 0) << "\n";
                }
                std::printf("  wrote %s (%zu rows)\n", out.c_str(), sp.spawnCount());
                filesWritten++;
            }
            // Objects
            wowee::editor::ObjectPlacer op;
            if (op.loadFromFile(zoneDir + "/objects.json")) {
                std::string out = outDir + "/objects.csv";
                std::ofstream f(out);
                if (!f) return 1;
                f << "index,type,path,x,y,z,rotX,rotY,rotZ,scale\n";
                for (size_t k = 0; k < op.getObjects().size(); ++k) {
                    const auto& o = op.getObjects()[k];
                    f << k << ","
                      << (o.type == wowee::editor::PlaceableType::M2 ? "m2" : "wmo") << ","
                      << csvEsc(o.path) << ","
                      << o.position.x << "," << o.position.y << "," << o.position.z << ","
                      << o.rotation.x << "," << o.rotation.y << "," << o.rotation.z << ","
                      << o.scale << "\n";
                }
                std::printf("  wrote %s (%zu rows)\n", out.c_str(),
                            op.getObjects().size());
                filesWritten++;
            }
            // Quests — flatten to one row per quest. Objectives + items
            // are joined into a single semicolon-separated cell so the
            // CSV stays one-row-per-quest (designer-friendly for sorting).
            wowee::editor::QuestEditor qe;
            if (qe.loadFromFile(zoneDir + "/quests.json")) {
                std::string out = outDir + "/quests.csv";
                std::ofstream f(out);
                if (!f) return 1;
                f << "index,id,title,requiredLevel,giverNpcId,turnInNpcId,"
                     "xp,gold,silver,copper,nextQuestId,objectiveCount,"
                     "objectives,itemRewards\n";
                using OT = wowee::editor::QuestObjectiveType;
                auto typeName = [](OT t) {
                    switch (t) {
                        case OT::KillCreature: return "kill";
                        case OT::CollectItem:  return "collect";
                        case OT::TalkToNPC:    return "talk";
                        case OT::ExploreArea:  return "explore";
                        case OT::EscortNPC:    return "escort";
                        case OT::UseObject:    return "use";
                    }
                    return "?";
                };
                for (size_t k = 0; k < qe.questCount(); ++k) {
                    const auto& q = qe.getQuests()[k];
                    std::string objs;
                    for (size_t o = 0; o < q.objectives.size(); ++o) {
                        if (o) objs += "; ";
                        objs += std::string(typeName(q.objectives[o].type)) + ":" +
                                q.objectives[o].targetName + "x" +
                                std::to_string(q.objectives[o].targetCount);
                    }
                    std::string items;
                    for (size_t r = 0; r < q.reward.itemRewards.size(); ++r) {
                        if (r) items += "; ";
                        items += q.reward.itemRewards[r];
                    }
                    f << k << "," << q.id << "," << csvEsc(q.title) << ","
                      << q.requiredLevel << ","
                      << q.questGiverNpcId << "," << q.turnInNpcId << ","
                      << q.reward.xp << "," << q.reward.gold << ","
                      << q.reward.silver << "," << q.reward.copper << ","
                      << q.nextQuestId << ","
                      << q.objectives.size() << ","
                      << csvEsc(objs) << "," << csvEsc(items) << "\n";
                }
                std::printf("  wrote %s (%zu rows)\n", out.c_str(), qe.questCount());
                filesWritten++;
            }
            // Items — read items.json inline since the items pipeline
            // doesn't have a dedicated editor class yet.
            std::string itemsPath = zoneDir + "/items.json";
            if (fs::exists(itemsPath)) {
                nlohmann::json doc;
                try {
                    std::ifstream in(itemsPath);
                    in >> doc;
                } catch (...) {}
                if (doc.contains("items") && doc["items"].is_array()) {
                    std::string out = outDir + "/items.csv";
                    std::ofstream f(out);
                    if (f) {
                        f << "index,id,name,quality,itemLevel,displayId,stackable\n";
                        const auto& arr = doc["items"];
                        for (size_t k = 0; k < arr.size(); ++k) {
                            const auto& it = arr[k];
                            f << k << ","
                              << it.value("id", 0u) << ","
                              << csvEsc(it.value("name", std::string())) << ","
                              << it.value("quality", 1u) << ","
                              << it.value("itemLevel", 1u) << ","
                              << it.value("displayId", 0u) << ","
                              << it.value("stackable", 1u) << "\n";
                        }
                        std::printf("  wrote %s (%zu rows)\n", out.c_str(), arr.size());
                        filesWritten++;
                    }
                }
            }
            if (filesWritten == 0) {
                std::fprintf(stderr,
                    "export-zone-csv: zone has no creatures/objects/quests/items to emit\n");
                return 1;
            }
            std::printf("Exported %d CSV file(s) to %s\n", filesWritten, outDir.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--export-zone-checksum") == 0 && i + 1 < argc) {
            // SHA-256 manifest of every source file in a zone, in the
            // standard sha256sum format ('<hex>  <relpath>'). Lets users
            // verify zone integrity after a download or transfer with the
            // standard system tool:
            //   wowee_editor --export-zone-checksum custom_zones/MyZone
            //   sha256sum -c custom_zones/MyZone/SHA256SUMS
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "export-zone-checksum: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/SHA256SUMS";
            // Source files only — derived outputs (.glb/.obj/.stl/.html/
            // ZONE.md/DEPS.md/quests.dot/SHA256SUMS itself) are excluded
            // since they're regeneratable and would invalidate the
            // checksum on every rebuild.
            auto isDerived = [](const fs::path& p) {
                std::string ext = p.extension().string();
                std::string name = p.filename().string();
                if (ext == ".glb" || ext == ".obj" || ext == ".stl" ||
                    ext == ".html" || ext == ".dot" || ext == ".csv") return true;
                if (name == "ZONE.md" || name == "DEPS.md" ||
                    name == "SHA256SUMS" || name == "Makefile") return true;
                if (ext == ".png") return true;  // BLP→PNG renders at root
                return false;
            };
            std::vector<std::pair<std::string, std::string>> entries;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file()) continue;
                if (isDerived(e.path())) continue;
                std::string hex = wowee_sha256::fileHex(e.path().string());
                if (hex.empty()) continue;
                std::string rel = fs::relative(e.path(), zoneDir, ec).string();
                if (ec) rel = e.path().string();
                entries.push_back({hex, rel});
            }
            std::sort(entries.begin(), entries.end(),
                      [](const auto& a, const auto& b) { return a.second < b.second; });
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "export-zone-checksum: cannot write %s\n", outPath.c_str());
                return 1;
            }
            for (const auto& [hash, path] : entries) {
                // sha256sum format: 64-char hex, two spaces, path.
                out << hash << "  " << path << "\n";
            }
            out.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  %zu file(s) hashed (source only, derived excluded)\n",
                        entries.size());
            std::printf("  verify with: sha256sum -c %s\n", outPath.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--export-project-checksum") == 0 && i + 1 < argc) {
            // Project-wide manifest in the same sha256sum format, with
            // paths kept relative to <projectDir> (so entries look like
            // "<hex>  <zoneName>/<file>"). Also emits a single SHA-256
            // fingerprint over the manifest itself — a one-line
            // identity for the whole project, handy for CI release
            // gates and reproducibility checks.
            //
            //   wowee_editor --export-project-checksum custom_zones
            //   sha256sum -c custom_zones/PROJECT_SHA256SUMS
            std::string projectDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "export-project-checksum: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = projectDir + "/PROJECT_SHA256SUMS";
            // Same derived-output filter as --export-zone-checksum.
            auto isDerived = [](const fs::path& p) {
                std::string ext = p.extension().string();
                std::string name = p.filename().string();
                if (ext == ".glb" || ext == ".obj" || ext == ".stl" ||
                    ext == ".html" || ext == ".dot" || ext == ".csv") return true;
                if (name == "ZONE.md" || name == "DEPS.md" ||
                    name == "SHA256SUMS" || name == "PROJECT_SHA256SUMS" ||
                    name == "Makefile") return true;
                if (ext == ".png") return true;
                return false;
            };
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            std::vector<std::pair<std::string, std::string>> entries;
            for (const auto& zoneDir : zones) {
                std::error_code ec;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    if (isDerived(e.path())) continue;
                    std::string hex = wowee_sha256::fileHex(e.path().string());
                    if (hex.empty()) continue;
                    std::string rel = fs::relative(e.path(), projectDir, ec).string();
                    if (ec) rel = e.path().string();
                    entries.push_back({hex, rel});
                }
            }
            std::sort(entries.begin(), entries.end(),
                      [](const auto& a, const auto& b) { return a.second < b.second; });
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "export-project-checksum: cannot write %s\n", outPath.c_str());
                return 1;
            }
            // Hash the manifest body inline so the project fingerprint
            // is byte-identical to what `sha256sum PROJECT_SHA256SUMS`
            // would yield on the written file.
            std::string body;
            body.reserve(entries.size() * 80);
            for (const auto& [hash, path] : entries) {
                body += hash;
                body += "  ";
                body += path;
                body += "\n";
            }
            out << body;
            out.close();
            std::string fingerprint = wowee_sha256::hex(
                reinterpret_cast<const uint8_t*>(body.data()), body.size());
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  zones        : %zu\n", zones.size());
            std::printf("  files hashed : %zu\n", entries.size());
            std::printf("  fingerprint  : %s\n", fingerprint.c_str());
            std::printf("  verify with  : sha256sum -c %s\n", outPath.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--validate-project-checksum") == 0 && i + 1 < argc) {
            // In-tool verification of the manifest produced by
            // --export-project-checksum. Equivalent to 'sha256sum -c
            // PROJECT_SHA256SUMS' but cross-platform — Windows and
            // CI runners without coreutils don't need an external tool.
            // Exit 1 if any file is missing or its hash drifted.
            std::string projectDir = argv[++i];
            std::string inPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') inPath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "validate-project-checksum: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            if (inPath.empty()) inPath = projectDir + "/PROJECT_SHA256SUMS";
            std::ifstream in(inPath);
            if (!in) {
                std::fprintf(stderr,
                    "validate-project-checksum: cannot read %s\n", inPath.c_str());
                return 1;
            }
            int ok = 0, missing = 0, mismatched = 0;
            std::vector<std::string> failures;
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                // sha256sum format: 64-char hex, two spaces, path.
                if (line.size() < 66 || line[64] != ' ' || line[65] != ' ') {
                    std::fprintf(stderr,
                        "  malformed line (skipped): %s\n", line.c_str());
                    continue;
                }
                std::string expected = line.substr(0, 64);
                std::string rel = line.substr(66);
                std::string full = projectDir + "/" + rel;
                if (!fs::exists(full)) {
                    missing++;
                    failures.push_back(rel + " (missing)");
                    continue;
                }
                std::string actual = wowee_sha256::fileHex(full);
                if (actual != expected) {
                    mismatched++;
                    failures.push_back(rel + " (hash mismatch)");
                    continue;
                }
                ok++;
            }
            std::printf("validate-project-checksum: %s\n", inPath.c_str());
            std::printf("  ok         : %d\n", ok);
            std::printf("  missing    : %d\n", missing);
            std::printf("  mismatched : %d\n", mismatched);
            if (!failures.empty()) {
                std::printf("\n  Failures:\n");
                for (const auto& f : failures) std::printf("    - %s\n", f.c_str());
            }
            return (missing == 0 && mismatched == 0) ? 0 : 1;
        } else if (std::strcmp(argv[i], "--export-zone-html") == 0 && i + 1 < argc) {
            // Generate a single-file HTML viewer next to the zone .glb.
            // Anyone with a modern browser can open it — no installs, no
            // CDN-mining the user's network. Uses model-viewer (Google's
            // web component) bundled from the unpkg CDN since it's
            // standards-based and doesn't require a build step.
            //
            // Usage flow:
            //   wowee_editor --bake-zone-glb custom_zones/MyZone
            //   wowee_editor --export-zone-html custom_zones/MyZone
            //   open custom_zones/MyZone/MyZone.html  # opens in browser
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "export-zone-html: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "export-zone-html: parse failed\n");
                return 1;
            }
            std::string glbName = zm.mapName + ".glb";
            std::string glbPath = zoneDir + "/" + glbName;
            if (!fs::exists(glbPath)) {
                std::fprintf(stderr,
                    "export-zone-html: %s does not exist — run --bake-zone-glb first\n",
                    glbPath.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/" + zm.mapName + ".html";
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "export-zone-html: cannot write %s\n", outPath.c_str());
                return 1;
            }
            // Compute relative path from html file's parent dir to the
            // .glb so the viewer loads it. Default same-dir → just basename.
            std::string glbHref = glbName;
            // If outPath is in a different dir than the .glb, the user is
            // responsible for moving things; leaving glbHref as the
            // basename is a sensible default that fails loudly in the
            // browser console rather than producing a wrong-but-silent
            // page.
            std::string title = zm.displayName.empty()
                ? zm.mapName : zm.displayName;
            // Single-file template with model-viewer. The version pin
            // (^4.0.0) keeps the page from breaking when the unpkg
            // 'latest' silently bumps a major version.
            out << "<!doctype html>\n"
                   "<html lang=\"en\">\n"
                   "<head>\n"
                   "  <meta charset=\"utf-8\">\n"
                   "  <title>" << title << " — Wowee Zone Viewer</title>\n"
                   "  <script type=\"module\" "
                       "src=\"https://unpkg.com/@google/model-viewer@^4.0.0/dist/model-viewer.min.js\">"
                   "</script>\n"
                   "  <style>\n"
                   "    body { margin:0; font-family: sans-serif; background:#1a1a1a; color:#eee; }\n"
                   "    header { padding:12px 20px; background:#2a2a2a; border-bottom:1px solid #444; }\n"
                   "    h1 { margin:0; font-size:18px; font-weight:500; }\n"
                   "    .meta { color:#aaa; font-size:13px; margin-top:4px; }\n"
                   "    model-viewer { width:100vw; height:calc(100vh - 60px); background:#1a1a1a; }\n"
                   "    .footer { position:fixed; bottom:8px; right:12px; color:#666; font-size:11px; }\n"
                   "  </style>\n"
                   "</head>\n"
                   "<body>\n"
                   "  <header>\n"
                   "    <h1>" << title << "</h1>\n"
                   "    <div class=\"meta\">Map: <code>" << zm.mapName
                << "</code> · Tiles: " << zm.tiles.size()
                << " · MapId: " << zm.mapId << "</div>\n"
                   "  </header>\n"
                   "  <model-viewer\n"
                   "    src=\"" << glbHref << "\"\n"
                   "    alt=\"" << title << " terrain\"\n"
                   "    camera-controls\n"
                   "    auto-rotate\n"
                   "    rotation-per-second=\"15deg\"\n"
                   "    shadow-intensity=\"1\"\n"
                   "    exposure=\"1.2\"\n"
                   "    environment-image=\"neutral\">\n"
                   "  </model-viewer>\n"
                   "  <div class=\"footer\">Generated by wowee_editor --export-zone-html</div>\n"
                   "</body>\n"
                   "</html>\n";
            out.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  references %s (must sit next to .html)\n", glbHref.c_str());
            std::printf("  open in any modern browser — no install required\n");
            return 0;
        } else if (std::strcmp(argv[i], "--export-project-html") == 0 && i + 1 < argc) {
            // Project-level index page linking every zone's HTML viewer.
            // Pairs with --export-zone-html (single zone) and
            // --bake-zone-glb (terrain bake). Designed for github-pages
            // style 'all my zones' showcase.
            std::string projectDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "export-project-html: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = projectDir + "/index.html";
            // Walk for zones (dirs with zone.json). For each, record:
            //   - display name
            //   - relative path to its .html viewer (or null if not generated)
            //   - tile count, content counts
            struct ZoneEntry {
                std::string name, dirRel, htmlRel, glbRel;
                bool htmlExists = false, glbExists = false;
                int tiles = 0, creatures = 0, objects = 0, quests = 0;
            };
            std::vector<ZoneEntry> entries;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                wowee::editor::ZoneManifest zm;
                if (!zm.load((entry.path() / "zone.json").string())) continue;
                ZoneEntry ze;
                ze.name = zm.displayName.empty() ? zm.mapName : zm.displayName;
                ze.dirRel = entry.path().filename().string();
                ze.htmlRel = ze.dirRel + "/" + zm.mapName + ".html";
                ze.glbRel = ze.dirRel + "/" + zm.mapName + ".glb";
                ze.htmlExists = fs::exists(entry.path() / (zm.mapName + ".html"));
                ze.glbExists = fs::exists(entry.path() / (zm.mapName + ".glb"));
                ze.tiles = static_cast<int>(zm.tiles.size());
                wowee::editor::NpcSpawner sp;
                if (sp.loadFromFile((entry.path() / "creatures.json").string())) {
                    ze.creatures = static_cast<int>(sp.spawnCount());
                }
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile((entry.path() / "objects.json").string())) {
                    ze.objects = static_cast<int>(op.getObjects().size());
                }
                wowee::editor::QuestEditor qe;
                if (qe.loadFromFile((entry.path() / "quests.json").string())) {
                    ze.quests = static_cast<int>(qe.questCount());
                }
                entries.push_back(ze);
            }
            std::sort(entries.begin(), entries.end(),
                      [](const ZoneEntry& a, const ZoneEntry& b) {
                          return a.name < b.name;
                      });
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "export-project-html: cannot write %s\n", outPath.c_str());
                return 1;
            }
            out << "<!doctype html>\n"
                   "<html lang=\"en\">\n"
                   "<head>\n"
                   "  <meta charset=\"utf-8\">\n"
                   "  <title>Wowee Project — Zone Index</title>\n"
                   "  <style>\n"
                   "    body { margin:0; font-family: sans-serif; background:#1a1a1a; color:#eee; padding:20px; }\n"
                   "    h1 { margin:0 0 8px; font-size:22px; }\n"
                   "    .count { color:#aaa; font-size:14px; margin-bottom:24px; }\n"
                   "    .zones { display:grid; grid-template-columns: repeat(auto-fill, minmax(300px, 1fr)); gap:16px; }\n"
                   "    .zone { background:#2a2a2a; border:1px solid #444; border-radius:6px; padding:14px; }\n"
                   "    .zone h3 { margin:0 0 6px; font-size:16px; }\n"
                   "    .zone .stats { color:#aaa; font-size:13px; }\n"
                   "    .zone a { color:#7af; text-decoration:none; font-size:13px; display:inline-block; margin-top:8px; }\n"
                   "    .zone a:hover { text-decoration:underline; }\n"
                   "    .zone .nolink { color:#666; font-style:italic; font-size:13px; margin-top:8px; }\n"
                   "    .footer { margin-top:30px; color:#666; font-size:11px; }\n"
                   "  </style>\n"
                   "</head>\n"
                   "<body>\n"
                   "  <h1>Wowee Project — Zone Index</h1>\n"
                   "  <div class=\"count\">" << entries.size() << " zone(s) found in <code>"
                << projectDir << "</code></div>\n"
                   "  <div class=\"zones\">\n";
            for (const auto& z : entries) {
                out << "    <div class=\"zone\">\n"
                       "      <h3>" << z.name << "</h3>\n"
                       "      <div class=\"stats\">"
                    << z.tiles << " tile" << (z.tiles == 1 ? "" : "s") << " · "
                    << z.creatures << " creature" << (z.creatures == 1 ? "" : "s") << " · "
                    << z.objects << " object" << (z.objects == 1 ? "" : "s") << " · "
                    << z.quests << " quest" << (z.quests == 1 ? "" : "s") << "</div>\n";
                if (z.htmlExists) {
                    out << "      <a href=\"" << z.htmlRel << "\">Open viewer →</a>\n";
                } else if (z.glbExists) {
                    out << "      <div class=\"nolink\">No HTML viewer (run --export-zone-html)</div>\n";
                } else {
                    out << "      <div class=\"nolink\">No .glb (run --bake-zone-glb)</div>\n";
                }
                out << "    </div>\n";
            }
            out << "  </div>\n"
                   "  <div class=\"footer\">Generated by wowee_editor --export-project-html</div>\n"
                   "</body>\n"
                   "</html>\n";
            out.close();
            int withViewer = 0;
            for (const auto& z : entries) if (z.htmlExists) withViewer++;
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  %zu zone(s) listed, %d with viewable HTML\n",
                        entries.size(), withViewer);
            return 0;
        } else if (std::strcmp(argv[i], "--export-project-md") == 0 && i + 1 < argc) {
            // Markdown counterpart to --export-project-html. Generates a
            // README.md indexing every zone with counts + bake/viewer
            // status. GitHub renders it natively at the project root.
            // Pairs with --export-zone-summary-md (per-zone) — the project
            // README links to each zone's per-zone .md.
            std::string projectDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "export-project-md: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = projectDir + "/README.md";
            // Per-zone collection: name + counts + which artifacts exist.
            struct Row {
                std::string name, dirRel, mapName;
                int tiles = 0, creatures = 0, objects = 0, quests = 0;
                bool hasGlb = false, hasHtml = false, hasZoneMd = false;
            };
            std::vector<Row> rows;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                wowee::editor::ZoneManifest zm;
                if (!zm.load((entry.path() / "zone.json").string())) continue;
                Row r;
                r.name = zm.displayName.empty() ? zm.mapName : zm.displayName;
                r.dirRel = entry.path().filename().string();
                r.mapName = zm.mapName;
                r.tiles = static_cast<int>(zm.tiles.size());
                wowee::editor::NpcSpawner sp;
                if (sp.loadFromFile((entry.path() / "creatures.json").string())) {
                    r.creatures = static_cast<int>(sp.spawnCount());
                }
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile((entry.path() / "objects.json").string())) {
                    r.objects = static_cast<int>(op.getObjects().size());
                }
                wowee::editor::QuestEditor qe;
                if (qe.loadFromFile((entry.path() / "quests.json").string())) {
                    r.quests = static_cast<int>(qe.questCount());
                }
                r.hasGlb = fs::exists(entry.path() / (zm.mapName + ".glb"));
                r.hasHtml = fs::exists(entry.path() / (zm.mapName + ".html"));
                r.hasZoneMd = fs::exists(entry.path() / "ZONE.md");
                rows.push_back(std::move(r));
            }
            std::sort(rows.begin(), rows.end(),
                      [](const Row& a, const Row& b) { return a.name < b.name; });
            int totalT = 0, totalC = 0, totalO = 0, totalQ = 0;
            for (const auto& r : rows) {
                totalT += r.tiles; totalC += r.creatures;
                totalO += r.objects; totalQ += r.quests;
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "export-project-md: cannot write %s\n", outPath.c_str());
                return 1;
            }
            out << "# Wowee Project — Zone Index\n\n";
            out << "*Auto-generated. " << rows.size()
                << " zone(s) discovered in `" << projectDir << "`.*\n\n";
            out << "## Summary\n\n";
            out << "| Metric | Total |\n|---|---:|\n";
            out << "| Zones      | " << rows.size() << " |\n";
            out << "| Tiles      | " << totalT << " |\n";
            out << "| Creatures  | " << totalC << " |\n";
            out << "| Objects    | " << totalO << " |\n";
            out << "| Quests     | " << totalQ << " |\n\n";
            out << "## Zones\n\n";
            out << "| Zone | Tiles | Creatures | Objects | Quests | Bake | Viewer | Docs |\n";
            out << "|---|---:|---:|---:|---:|:---:|:---:|:---:|\n";
            for (const auto& r : rows) {
                out << "| ";
                if (r.hasZoneMd) {
                    out << "[" << r.name << "](" << r.dirRel << "/ZONE.md)";
                } else {
                    out << r.name;
                }
                out << " | " << r.tiles << " | " << r.creatures << " | "
                    << r.objects << " | " << r.quests << " | "
                    << (r.hasGlb ? "✓" : "—") << " | "
                    << (r.hasHtml ? "[view](" + r.dirRel + "/" + r.mapName + ".html)" : "—") << " | "
                    << (r.hasZoneMd ? "[md](" + r.dirRel + "/ZONE.md)" : "—") << " |\n";
            }
            out.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  %zu zone(s) indexed (%d tiles, %d creatures, %d objects, %d quests)\n",
                        rows.size(), totalT, totalC, totalO, totalQ);
            return 0;
        } else if (std::strcmp(argv[i], "--export-quest-graph") == 0 && i + 1 < argc) {
            // Render quest chains as a Graphviz DOT graph. Visualizing
            // quest dependencies in plain text rapidly becomes unreadable
            // past ~10 quests; piping this through 'dot -Tpng -o q.png'
            // makes complex chains immediately legible.
            //
            //   wowee_editor --export-quest-graph custom_zones/MyZone
            //   dot -Tpng custom_zones/MyZone/quests.dot -o quests.png
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr,
                    "export-quest-graph: %s not found\n", path.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/quests.dot";
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr,
                    "export-quest-graph: failed to parse %s\n", path.c_str());
                return 1;
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "export-quest-graph: cannot write %s\n", outPath.c_str());
                return 1;
            }
            // DOT-escape strings (just quotes and backslashes) — quest
            // titles can include arbitrary punctuation that breaks DOT
            // parsing if not escaped.
            auto dotEsc = [](const std::string& s) {
                std::string out;
                for (char c : s) {
                    if (c == '"' || c == '\\') out += '\\';
                    out += c;
                }
                return out;
            };
            const auto& quests = qe.getQuests();
            // Build an index of valid quest IDs so dangling chain
            // pointers can be styled differently (red, dashed).
            std::unordered_set<uint32_t> validIds;
            for (const auto& q : quests) validIds.insert(q.id);
            out << "digraph QuestChains {\n";
            out << "  // Generated by wowee_editor --export-quest-graph\n";
            out << "  rankdir=LR;\n";
            out << "  node [shape=box, style=filled, fontname=\"sans-serif\"];\n";
            // Nodes: one per quest, colored by completion-readiness:
            //   green = has objectives + reward + valid NPCs
            //   yellow = missing some non-fatal field (description, etc.)
            //   gray = no objectives (won't actually complete in-game)
            for (const auto& q : quests) {
                bool hasObjs = !q.objectives.empty();
                bool hasReward = (q.reward.xp > 0 || !q.reward.itemRewards.empty());
                std::string color = hasObjs ? (hasReward ? "lightgreen" : "lightyellow")
                                             : "lightgray";
                std::string label = "[" + std::to_string(q.id) + "] " + dotEsc(q.title);
                if (q.requiredLevel > 1) {
                    label += "\\nlvl " + std::to_string(q.requiredLevel);
                }
                if (q.reward.xp > 0) {
                    label += "  " + std::to_string(q.reward.xp) + " XP";
                }
                out << "  q" << q.id << " [label=\"" << label
                    << "\", fillcolor=" << color << "];\n";
            }
            // Edges: quest -> nextQuestId. Style chain-pointers to
            // missing quests differently so they stand out visually.
            int chainEdges = 0, brokenEdges = 0;
            for (const auto& q : quests) {
                if (q.nextQuestId == 0) continue;
                if (validIds.count(q.nextQuestId) == 0) {
                    out << "  q" << q.id << " -> q" << q.nextQuestId
                        << " [color=red, style=dashed, label=\"missing\"];\n";
                    out << "  q" << q.nextQuestId
                        << " [label=\"<missing> [" << q.nextQuestId
                        << "]\", fillcolor=mistyrose, style=\"filled,dashed\"];\n";
                    brokenEdges++;
                } else {
                    out << "  q" << q.id << " -> q" << q.nextQuestId << ";\n";
                    chainEdges++;
                }
            }
            out << "}\n";
            out.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  %zu quests, %d chain edges, %d broken (red/dashed)\n",
                        quests.size(), chainEdges, brokenEdges);
            std::printf("  next: dot -Tpng %s -o quests.png\n", outPath.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--bench-bake-project") == 0 && i + 1 < argc) {
            // Time WHM/WOT load (the dominant cost in --bake-zone-glb/obj/
            // stl) per zone. The actual write side adds ~constant cost
            // proportional to vertex count, so load time is a strong
            // proxy. Useful for tracking 'has my latest geometry change
            // made baking 3× slower?' across releases.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "bench-bake-project: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            struct Timing {
                std::string name;
                int tiles;
                double loadMs;
                int chunks;
            };
            std::vector<Timing> timings;
            double totalMs = 0;
            for (const auto& zoneDir : zones) {
                wowee::editor::ZoneManifest zm;
                if (!zm.load(zoneDir + "/zone.json")) continue;
                Timing t{fs::path(zoneDir).filename().string(), 0, 0.0, 0};
                auto t0 = std::chrono::steady_clock::now();
                for (const auto& [tx, ty] : zm.tiles) {
                    std::string base = zoneDir + "/" + zm.mapName + "_" +
                                        std::to_string(tx) + "_" + std::to_string(ty);
                    if (!wowee::pipeline::WoweeTerrainLoader::exists(base)) continue;
                    wowee::pipeline::ADTTerrain terrain;
                    wowee::pipeline::WoweeTerrainLoader::load(base, terrain);
                    t.tiles++;
                    for (const auto& chunk : terrain.chunks) {
                        if (chunk.heightMap.isLoaded()) t.chunks++;
                    }
                }
                auto t1 = std::chrono::steady_clock::now();
                t.loadMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                totalMs += t.loadMs;
                timings.push_back(t);
            }
            double avgMs = !timings.empty() ? totalMs / timings.size() : 0.0;
            double minMs = 1e30, maxMs = 0;
            std::string slowest;
            for (const auto& t : timings) {
                if (t.loadMs < minMs) minMs = t.loadMs;
                if (t.loadMs > maxMs) { maxMs = t.loadMs; slowest = t.name; }
            }
            if (timings.empty()) { minMs = 0; maxMs = 0; }
            if (jsonOut) {
                nlohmann::json j;
                j["projectDir"] = projectDir;
                j["totalMs"] = totalMs;
                j["zoneCount"] = timings.size();
                j["avgMs"] = avgMs;
                j["minMs"] = minMs;
                j["maxMs"] = maxMs;
                j["slowestZone"] = slowest;
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& t : timings) {
                    arr.push_back({{"zone", t.name},
                                    {"loadMs", t.loadMs},
                                    {"tiles", t.tiles},
                                    {"chunks", t.chunks}});
                }
                j["perZone"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Bench bake (load-only): %s\n", projectDir.c_str());
            std::printf("  zones    : %zu\n", timings.size());
            std::printf("  total    : %.2f ms (terrain load)\n", totalMs);
            std::printf("  per zone : avg=%.2f min=%.2f max=%.2f ms\n",
                        avgMs, minMs, maxMs);
            if (!slowest.empty()) {
                std::printf("  slowest  : %s (%.2f ms)\n", slowest.c_str(), maxMs);
            }
            std::printf("\n  Per-zone:\n");
            std::printf("    zone                       ms     tiles  chunks  ms/tile\n");
            for (const auto& t : timings) {
                double mspt = t.tiles > 0 ? t.loadMs / t.tiles : 0.0;
                std::printf("    %-26s %7.2f  %5d   %5d   %6.2f\n",
                            t.name.substr(0, 26).c_str(),
                            t.loadMs, t.tiles, t.chunks, mspt);
            }
            return 0;
        } else if ((std::strcmp(argv[i], "--validate-glb") == 0 ||
                    std::strcmp(argv[i], "--info-glb") == 0) && i + 1 < argc) {
            // Shared handler: --validate-glb errors out on broken structure;
            // --info-glb prints the same metadata but exits 0 unless the
            // file is unreadable. Same parser, different verdict policy.
            bool isValidate = (std::strcmp(argv[i], "--validate-glb") == 0);
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr,
                    "%s: cannot open %s\n",
                    isValidate ? "validate-glb" : "info-glb", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            std::vector<std::string> errors;
            // 12-byte header: 'glTF' magic, version=2, total length.
            uint32_t magic = 0, version = 0, totalLen = 0;
            if (bytes.size() < 12) {
                errors.push_back("file too short for glTF header (need 12 bytes)");
            } else {
                std::memcpy(&magic,    &bytes[0], 4);
                std::memcpy(&version,  &bytes[4], 4);
                std::memcpy(&totalLen, &bytes[8], 4);
                if (magic != 0x46546C67) {
                    errors.push_back("magic is not 'glTF' (0x46546C67)");
                }
                if (version != 2) {
                    errors.push_back("version " + std::to_string(version) +
                                     " not supported (only glTF 2.0)");
                }
                if (totalLen != bytes.size()) {
                    errors.push_back("totalLength=" + std::to_string(totalLen) +
                                     " != file size " + std::to_string(bytes.size()));
                }
            }
            // JSON chunk follows: 4-byte length, 4-byte type ('JSON'),
            // then payload. Then BIN chunk same shape.
            uint32_t jsonLen = 0, jsonType = 0;
            uint32_t binLen = 0, binType = 0;
            std::string jsonStr;
            std::vector<uint8_t> binData;
            if (errors.empty()) {
                if (bytes.size() < 20) {
                    errors.push_back("missing JSON chunk header");
                } else {
                    std::memcpy(&jsonLen, &bytes[12], 4);
                    std::memcpy(&jsonType, &bytes[16], 4);
                    if (jsonType != 0x4E4F534A) {
                        errors.push_back("first chunk type is not 'JSON' (0x4E4F534A)");
                    }
                    if (20 + jsonLen > bytes.size()) {
                        errors.push_back("JSON chunk extends past file end");
                    } else {
                        jsonStr.assign(bytes.begin() + 20,
                                        bytes.begin() + 20 + jsonLen);
                    }
                }
                size_t binOff = 20 + jsonLen;
                if (binOff + 8 <= bytes.size()) {
                    std::memcpy(&binLen, &bytes[binOff], 4);
                    std::memcpy(&binType, &bytes[binOff + 4], 4);
                    if (binType != 0x004E4942) {
                        errors.push_back("second chunk type is not 'BIN\\0' (0x004E4942)");
                    }
                    if (binOff + 8 + binLen > bytes.size()) {
                        errors.push_back("BIN chunk extends past file end");
                    } else {
                        binData.assign(bytes.begin() + binOff + 8,
                                        bytes.begin() + binOff + 8 + binLen);
                    }
                }
                // BIN chunk is optional in spec; only flag missing if
                // accessors below reference a buffer.
            }
            // Parse JSON and validate structure.
            nlohmann::json gj;
            int meshCount = 0, primitiveCount = 0, accessorCount = 0,
                bufferViewCount = 0, bufferCount = 0;
            std::string assetVersion;
            if (errors.empty() && !jsonStr.empty()) {
                try {
                    gj = nlohmann::json::parse(jsonStr);
                    assetVersion = gj.value("/asset/version"_json_pointer, std::string{});
                    if (assetVersion != "2.0") {
                        errors.push_back("asset.version is '" + assetVersion +
                                         "', not '2.0'");
                    }
                    if (gj.contains("meshes") && gj["meshes"].is_array()) {
                        meshCount = static_cast<int>(gj["meshes"].size());
                        for (const auto& m : gj["meshes"]) {
                            if (m.contains("primitives") && m["primitives"].is_array()) {
                                primitiveCount += static_cast<int>(m["primitives"].size());
                            }
                        }
                    }
                    if (gj.contains("accessors") && gj["accessors"].is_array()) {
                        accessorCount = static_cast<int>(gj["accessors"].size());
                        // Verify each accessor's bufferView exists.
                        for (size_t a = 0; a < gj["accessors"].size(); ++a) {
                            const auto& acc = gj["accessors"][a];
                            if (acc.contains("bufferView")) {
                                int bv = acc["bufferView"];
                                if (!gj.contains("bufferViews") ||
                                    bv >= static_cast<int>(gj["bufferViews"].size())) {
                                    errors.push_back("accessor " + std::to_string(a) +
                                                     " bufferView=" + std::to_string(bv) +
                                                     " out of range");
                                }
                            }
                        }
                    }
                    if (gj.contains("bufferViews") && gj["bufferViews"].is_array()) {
                        bufferViewCount = static_cast<int>(gj["bufferViews"].size());
                        for (size_t b = 0; b < gj["bufferViews"].size(); ++b) {
                            const auto& bv = gj["bufferViews"][b];
                            uint32_t bo = bv.value("byteOffset", 0u);
                            uint32_t bl = bv.value("byteLength", 0u);
                            uint64_t end = uint64_t(bo) + bl;
                            if (end > binLen) {
                                errors.push_back("bufferView " + std::to_string(b) +
                                                 " range [" + std::to_string(bo) +
                                                 ", " + std::to_string(end) +
                                                 ") past BIN chunk length " +
                                                 std::to_string(binLen));
                            }
                        }
                    }
                    if (gj.contains("buffers") && gj["buffers"].is_array()) {
                        bufferCount = static_cast<int>(gj["buffers"].size());
                    }
                } catch (const std::exception& e) {
                    errors.push_back(std::string("JSON parse error: ") + e.what());
                }
            }
            int errorCount = static_cast<int>(errors.size());
            if (jsonOut) {
                nlohmann::json j;
                j["glb"] = path;
                j["fileSize"] = bytes.size();
                j["version"] = version;
                j["assetVersion"] = assetVersion;
                j["totalLength"] = totalLen;
                j["jsonLength"] = jsonLen;
                j["binLength"] = binLen;
                j["meshes"] = meshCount;
                j["primitives"] = primitiveCount;
                j["accessors"] = accessorCount;
                j["bufferViews"] = bufferViewCount;
                j["buffers"] = bufferCount;
                j["errorCount"] = errorCount;
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return (isValidate && errorCount > 0) ? 1 : 0;
            }
            std::printf("GLB: %s\n", path.c_str());
            std::printf("  file bytes  : %zu\n", bytes.size());
            std::printf("  glTF version: %u (asset.version=%s)\n",
                        version, assetVersion.empty() ? "?" : assetVersion.c_str());
            std::printf("  totalLength : %u\n", totalLen);
            std::printf("  JSON chunk  : %u bytes\n", jsonLen);
            std::printf("  BIN chunk   : %u bytes\n", binLen);
            std::printf("  meshes      : %d (%d primitives)\n",
                        meshCount, primitiveCount);
            std::printf("  accessors   : %d  bufferViews: %d  buffers: %d\n",
                        accessorCount, bufferViewCount, bufferCount);
            if (errors.empty()) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %d error(s):\n", errorCount);
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return isValidate ? 1 : 0;
        } else if (std::strcmp(argv[i], "--info-glb-tree") == 0 && i + 1 < argc) {
            // Pretty `tree`-style view of glTF structure. --info-glb gives
            // counts; this shows the actual scene→node→mesh→primitive
            // hierarchy with names. Useful when debugging 'why is this
            // imported model showing up empty in three.js?' (often
            // because the scene's nodes[] array references the wrong node).
            std::string path = argv[++i];
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr,
                    "info-glb-tree: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            if (bytes.size() < 28) {
                std::fprintf(stderr, "info-glb-tree: file too short\n");
                return 1;
            }
            uint32_t magic, version;
            std::memcpy(&magic, &bytes[0], 4);
            std::memcpy(&version, &bytes[4], 4);
            if (magic != 0x46546C67 || version != 2) {
                std::fprintf(stderr, "info-glb-tree: not glTF 2.0\n");
                return 1;
            }
            uint32_t jsonLen;
            std::memcpy(&jsonLen, &bytes[12], 4);
            std::string jsonStr(bytes.begin() + 20, bytes.begin() + 20 + jsonLen);
            nlohmann::json gj;
            try { gj = nlohmann::json::parse(jsonStr); }
            catch (const std::exception& e) {
                std::fprintf(stderr, "info-glb-tree: JSON parse failed: %s\n", e.what());
                return 1;
            }
            // Tree drawing
            auto branch = [](bool last) { return last ? "└─ " : "├─ "; };
            auto cont = [](bool last) { return last ? "   " : "│  "; };
            std::printf("%s\n", path.c_str());
            // Asset section
            std::string genName = gj.value("/asset/version"_json_pointer, std::string{});
            std::string gen = gj.value("/asset/generator"_json_pointer, std::string{});
            std::printf("├─ asset (v%s, %s)\n",
                        genName.c_str(),
                        gen.empty() ? "no generator" : gen.c_str());
            // Buffers
            int nBuf = (gj.contains("buffers") && gj["buffers"].is_array())
                        ? static_cast<int>(gj["buffers"].size()) : 0;
            std::printf("├─ buffers (%d)\n", nBuf);
            if (nBuf > 0) {
                for (int b = 0; b < nBuf; ++b) {
                    bool last = (b == nBuf - 1);
                    uint64_t bl = gj["buffers"][b].value("byteLength", 0u);
                    std::printf("│  %s[%d] %llu bytes\n", branch(last), b,
                                static_cast<unsigned long long>(bl));
                }
            }
            // BufferViews
            int nBV = (gj.contains("bufferViews") && gj["bufferViews"].is_array())
                       ? static_cast<int>(gj["bufferViews"].size()) : 0;
            std::printf("├─ bufferViews (%d)\n", nBV);
            for (int v = 0; v < nBV; ++v) {
                bool last = (v == nBV - 1);
                const auto& bv = gj["bufferViews"][v];
                uint32_t bo = bv.value("byteOffset", 0u);
                uint32_t bl = bv.value("byteLength", 0u);
                int target = bv.value("target", 0);
                std::printf("│  %s[%d] off=%u len=%u%s\n",
                            branch(last), v, bo, bl,
                            target == 34962 ? " (vertex)"
                          : target == 34963 ? " (index)"
                          : "");
            }
            // Accessors
            int nAcc = (gj.contains("accessors") && gj["accessors"].is_array())
                        ? static_cast<int>(gj["accessors"].size()) : 0;
            std::printf("├─ accessors (%d)\n", nAcc);
            for (int a = 0; a < nAcc; ++a) {
                bool last = (a == nAcc - 1);
                const auto& acc = gj["accessors"][a];
                int ct = acc.value("componentType", 0);
                std::string type = acc.value("type", std::string{});
                uint32_t count = acc.value("count", 0u);
                int bv = acc.value("bufferView", -1);
                const char* ctName =
                    ct == 5120 ? "i8" :
                    ct == 5121 ? "u8" :
                    ct == 5122 ? "i16" :
                    ct == 5123 ? "u16" :
                    ct == 5125 ? "u32" :
                    ct == 5126 ? "f32" : "?";
                std::printf("│  %s[%d] %s %s ×%u (bv=%d)\n",
                            branch(last), a, ctName, type.c_str(), count, bv);
            }
            // Meshes (with primitives nested)
            int nMesh = (gj.contains("meshes") && gj["meshes"].is_array())
                         ? static_cast<int>(gj["meshes"].size()) : 0;
            std::printf("├─ meshes (%d)\n", nMesh);
            for (int m = 0; m < nMesh; ++m) {
                bool lastM = (m == nMesh - 1);
                const auto& mesh = gj["meshes"][m];
                std::string name = mesh.value("name", std::string{});
                int nPrim = (mesh.contains("primitives") && mesh["primitives"].is_array())
                             ? static_cast<int>(mesh["primitives"].size()) : 0;
                std::printf("│  %s[%d]%s%s (%d primitives)\n",
                            branch(lastM), m,
                            name.empty() ? "" : " ",
                            name.c_str(), nPrim);
                for (int p = 0; p < nPrim; ++p) {
                    bool lastP = (p == nPrim - 1);
                    const auto& prim = mesh["primitives"][p];
                    int idxAcc = prim.value("indices", -1);
                    int mode = prim.value("mode", 4);
                    const char* modeName =
                        mode == 0 ? "POINTS" :
                        mode == 1 ? "LINES" :
                        mode == 4 ? "TRIANGLES" : "?";
                    std::printf("│  %s%s[%d] %s indices=acc#%d\n",
                                cont(lastM), branch(lastP), p, modeName, idxAcc);
                }
            }
            // Nodes (flat list — could be tree but glTF nodes are a graph)
            int nNode = (gj.contains("nodes") && gj["nodes"].is_array())
                         ? static_cast<int>(gj["nodes"].size()) : 0;
            std::printf("├─ nodes (%d)\n", nNode);
            for (int n = 0; n < nNode; ++n) {
                bool last = (n == nNode - 1);
                const auto& node = gj["nodes"][n];
                std::string name = node.value("name", std::string{});
                int meshIdx = node.value("mesh", -1);
                std::printf("│  %s[%d]%s%s%s\n",
                            branch(last), n,
                            name.empty() ? "" : " ",
                            name.c_str(),
                            meshIdx >= 0 ? (" -> mesh#" + std::to_string(meshIdx)).c_str() : "");
            }
            // Scenes (last branch)
            int nScene = (gj.contains("scenes") && gj["scenes"].is_array())
                          ? static_cast<int>(gj["scenes"].size()) : 0;
            std::printf("└─ scenes (%d, default=%d)\n",
                        nScene, gj.value("scene", 0));
            for (int s = 0; s < nScene; ++s) {
                bool lastS = (s == nScene - 1);
                const auto& scene = gj["scenes"][s];
                int nodeRefs = (scene.contains("nodes") && scene["nodes"].is_array())
                                ? static_cast<int>(scene["nodes"].size()) : 0;
                std::printf("   %s[%d] nodes=[", branch(lastS), s);
                if (scene.contains("nodes") && scene["nodes"].is_array()) {
                    for (size_t k = 0; k < scene["nodes"].size(); ++k) {
                        std::printf("%s%d", k ? "," : "", scene["nodes"][k].get<int>());
                    }
                }
                std::printf("] (%d nodes)\n", nodeRefs);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-glb-bytes") == 0 && i + 1 < argc) {
            // Per-section + per-bufferView byte breakdown of a .glb. Useful
            // for understanding what's bloating a baked .glb (vertex attrs
            // vs indices, position vs uv vs normal data, mesh-level
            // payloads). Pairs with --info-glb (counts) and --info-glb-tree
            // (structure).
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr,
                    "info-glb-bytes: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            if (bytes.size() < 28) {
                std::fprintf(stderr, "info-glb-bytes: file too short\n");
                return 1;
            }
            uint32_t magic, version;
            std::memcpy(&magic, &bytes[0], 4);
            std::memcpy(&version, &bytes[4], 4);
            if (magic != 0x46546C67 || version != 2) {
                std::fprintf(stderr, "info-glb-bytes: not glTF 2.0\n");
                return 1;
            }
            uint32_t jsonLen, binLen = 0;
            std::memcpy(&jsonLen, &bytes[12], 4);
            std::string jsonStr(bytes.begin() + 20,
                                 bytes.begin() + 20 + jsonLen);
            size_t binOff = 20 + jsonLen;
            if (binOff + 8 <= bytes.size()) {
                std::memcpy(&binLen, &bytes[binOff], 4);
            }
            uint32_t headerBytes = 12;        // magic+version+totalLength
            uint32_t jsonHdrBytes = 8;        // jsonLen + jsonType
            uint32_t binHdrBytes = (binLen > 0) ? 8 : 0;
            nlohmann::json gj;
            try { gj = nlohmann::json::parse(jsonStr); }
            catch (const std::exception& e) {
                std::fprintf(stderr,
                    "info-glb-bytes: JSON parse failed: %s\n", e.what());
                return 1;
            }
            // Per-bufferView size table.
            struct BV { int idx; uint32_t off, len; std::string label; };
            std::vector<BV> bufferViews;
            if (gj.contains("bufferViews") && gj["bufferViews"].is_array()) {
                for (size_t k = 0; k < gj["bufferViews"].size(); ++k) {
                    const auto& bv = gj["bufferViews"][k];
                    BV b;
                    b.idx = static_cast<int>(k);
                    b.off = bv.value("byteOffset", 0u);
                    b.len = bv.value("byteLength", 0u);
                    int target = bv.value("target", 0);
                    b.label = (target == 34962) ? "vertex" :
                              (target == 34963) ? "index" : "other";
                    bufferViews.push_back(b);
                }
            }
            // Bucket bufferViews by purpose using accessor types.
            // Walk accessors: each references a bufferView, with type
            // (VEC3/VEC2/SCALAR) hinting at content (position/uv/etc.)
            std::map<std::string, uint64_t> bytesByPurpose;
            if (gj.contains("accessors") && gj["accessors"].is_array() &&
                gj.contains("meshes") && gj["meshes"].is_array()) {
                std::set<int> seenAccessors;
                for (const auto& m : gj["meshes"]) {
                    if (!m.contains("primitives") || !m["primitives"].is_array()) continue;
                    for (const auto& p : m["primitives"]) {
                        if (!p.contains("attributes")) continue;
                        for (auto it = p["attributes"].begin();
                             it != p["attributes"].end(); ++it) {
                            int ai = it.value().get<int>();
                            if (seenAccessors.count(ai)) continue;
                            seenAccessors.insert(ai);
                            if (ai < 0 || ai >= static_cast<int>(gj["accessors"].size())) continue;
                            const auto& acc = gj["accessors"][ai];
                            int bv = acc.value("bufferView", -1);
                            if (bv < 0 || bv >= static_cast<int>(bufferViews.size())) continue;
                            std::string typeStr = acc.value("type", std::string{});
                            int comp = acc.value("componentType", 0);
                            uint32_t cnt = acc.value("count", 0u);
                            uint32_t byteStride =
                                typeStr == "VEC3" ? 12 :
                                typeStr == "VEC2" ? 8 :
                                typeStr == "VEC4" ? 16 :
                                typeStr == "SCALAR" ?
                                    (comp == 5126 ? 4 : comp == 5125 ? 4 :
                                     comp == 5123 ? 2 : comp == 5121 ? 1 : 4) : 4;
                            uint64_t b = uint64_t(cnt) * byteStride;
                            bytesByPurpose[it.key()] += b;
                        }
                        // Indices accessor.
                        if (p.contains("indices")) {
                            int ai = p["indices"].get<int>();
                            if (seenAccessors.count(ai)) continue;
                            seenAccessors.insert(ai);
                            if (ai < 0 || ai >= static_cast<int>(gj["accessors"].size())) continue;
                            const auto& acc = gj["accessors"][ai];
                            uint32_t cnt = acc.value("count", 0u);
                            int comp = acc.value("componentType", 0);
                            uint32_t s = (comp == 5125 ? 4 : comp == 5123 ? 2 : 4);
                            bytesByPurpose["INDICES"] += uint64_t(cnt) * s;
                        }
                    }
                }
            }
            uint64_t totalBytes = bytes.size();
            if (jsonOut) {
                nlohmann::json j;
                j["glb"] = path;
                j["totalBytes"] = totalBytes;
                j["sections"] = {
                    {"header", headerBytes},
                    {"jsonHeader", jsonHdrBytes},
                    {"json", jsonLen},
                    {"binHeader", binHdrBytes},
                    {"bin", binLen}
                };
                nlohmann::json bvArr = nlohmann::json::array();
                for (const auto& bv : bufferViews) {
                    bvArr.push_back({{"index", bv.idx},
                                      {"target", bv.label},
                                      {"bytes", bv.len}});
                }
                j["bufferViews"] = bvArr;
                nlohmann::json byPurp = nlohmann::json::object();
                for (const auto& [p, b] : bytesByPurpose) byPurp[p] = b;
                j["byPurpose"] = byPurp;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("GLB bytes: %s\n", path.c_str());
            std::printf("  total: %llu bytes (%.2f MB)\n",
                        static_cast<unsigned long long>(totalBytes),
                        totalBytes / (1024.0 * 1024.0));
            std::printf("\n  Sections:\n");
            auto pct = [&](uint64_t v) {
                return totalBytes ? 100.0 * v / totalBytes : 0.0;
            };
            std::printf("    header     : %5u bytes  %5.2f%%\n", headerBytes, pct(headerBytes));
            std::printf("    JSON hdr   : %5u bytes  %5.2f%%\n", jsonHdrBytes, pct(jsonHdrBytes));
            std::printf("    JSON       : %5u bytes  %5.2f%%\n", jsonLen, pct(jsonLen));
            std::printf("    BIN hdr    : %5u bytes  %5.2f%%\n", binHdrBytes, pct(binHdrBytes));
            std::printf("    BIN        : %5u bytes  %5.2f%%\n", binLen, pct(binLen));
            if (!bufferViews.empty()) {
                std::printf("\n  BufferViews:\n");
                std::printf("    idx  target   bytes      MB    share-of-bin\n");
                for (const auto& bv : bufferViews) {
                    double bvPct = binLen ? 100.0 * bv.len / binLen : 0.0;
                    std::printf("    %3d  %-7s  %8u  %6.2f  %5.2f%%\n",
                                bv.idx, bv.label.c_str(), bv.len,
                                bv.len / (1024.0 * 1024.0), bvPct);
                }
            }
            if (!bytesByPurpose.empty()) {
                std::printf("\n  By attribute:\n");
                for (const auto& [p, b] : bytesByPurpose) {
                    double bPct = binLen ? 100.0 * b / binLen : 0.0;
                    std::printf("    %-12s %8llu bytes  (%.2f%% of BIN)\n",
                                p.c_str(),
                                static_cast<unsigned long long>(b), bPct);
                }
            }
            return 0;
        } else if (std::strcmp(argv[i], "--check-glb-bounds") == 0 && i + 1 < argc) {
            // Cross-checks every position accessor's claimed min/max
            // against the actual data in the BIN chunk. glTF viewers use
            // these for camera framing and frustum culling — stale
            // values (e.g. from a tool that edited geometry without
            // recomputing) cause models to vanish at certain angles or
            // get framed wrong on load.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr,
                    "check-glb-bounds: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            // Parse glb structure (re-implements --validate-glb's parser
            // since we need access to the BIN chunk bytes here).
            if (bytes.size() < 28) {
                std::fprintf(stderr,
                    "check-glb-bounds: file too short to be a .glb\n");
                return 1;
            }
            uint32_t magic, version;
            std::memcpy(&magic, &bytes[0], 4);
            std::memcpy(&version, &bytes[4], 4);
            if (magic != 0x46546C67 || version != 2) {
                std::fprintf(stderr,
                    "check-glb-bounds: not a valid glTF 2.0 binary\n");
                return 1;
            }
            uint32_t jsonLen, jsonType;
            std::memcpy(&jsonLen, &bytes[12], 4);
            std::memcpy(&jsonType, &bytes[16], 4);
            std::string jsonStr(bytes.begin() + 20, bytes.begin() + 20 + jsonLen);
            size_t binOff = 20 + jsonLen;
            std::memcpy(&magic, &bytes[binOff + 4], 4);  // chunkType
            const uint8_t* binData = &bytes[binOff + 8];
            uint32_t binLen;
            std::memcpy(&binLen, &bytes[binOff], 4);
            (void)binLen;  // not range-checked here; --validate-glb does that
            nlohmann::json gj;
            try { gj = nlohmann::json::parse(jsonStr); }
            catch (const std::exception& e) {
                std::fprintf(stderr,
                    "check-glb-bounds: JSON parse failed: %s\n", e.what());
                return 1;
            }
            std::vector<std::string> errors;
            int posAccessors = 0, mismatched = 0;
            // Walk all primitives, collect their POSITION accessor index,
            // dedupe (multiple primitives can share an accessor — only
            // recompute once per unique).
            std::set<int> posAccIndices;
            if (gj.contains("meshes") && gj["meshes"].is_array()) {
                for (const auto& m : gj["meshes"]) {
                    if (!m.contains("primitives") || !m["primitives"].is_array()) continue;
                    for (const auto& p : m["primitives"]) {
                        if (p.contains("attributes") &&
                            p["attributes"].contains("POSITION")) {
                            posAccIndices.insert(p["attributes"]["POSITION"].get<int>());
                        }
                    }
                }
            }
            const auto& accessors = gj["accessors"];
            const auto& bufferViews = gj["bufferViews"];
            for (int ai : posAccIndices) {
                if (ai < 0 || ai >= static_cast<int>(accessors.size())) {
                    errors.push_back("position accessor " + std::to_string(ai) +
                                     " out of range");
                    continue;
                }
                const auto& acc = accessors[ai];
                if (acc.value("type", std::string{}) != "VEC3" ||
                    acc.value("componentType", 0) != 5126) {
                    errors.push_back("accessor " + std::to_string(ai) +
                                     " is not VEC3 FLOAT");
                    continue;
                }
                posAccessors++;
                int bvIdx = acc.value("bufferView", -1);
                if (bvIdx < 0 || bvIdx >= static_cast<int>(bufferViews.size())) {
                    errors.push_back("accessor " + std::to_string(ai) +
                                     " bufferView " + std::to_string(bvIdx) +
                                     " out of range");
                    continue;
                }
                const auto& bv = bufferViews[bvIdx];
                uint32_t bvOff = bv.value("byteOffset", 0u);
                uint32_t accOff = acc.value("byteOffset", 0u);
                uint32_t count = acc.value("count", 0u);
                const uint8_t* p = binData + bvOff + accOff;
                glm::vec3 actualMin{1e30f}, actualMax{-1e30f};
                for (uint32_t v = 0; v < count; ++v) {
                    glm::vec3 pos;
                    std::memcpy(&pos.x, p + v * 12 + 0, 4);
                    std::memcpy(&pos.y, p + v * 12 + 4, 4);
                    std::memcpy(&pos.z, p + v * 12 + 8, 4);
                    actualMin = glm::min(actualMin, pos);
                    actualMax = glm::max(actualMax, pos);
                }
                // Compare against claimed min/max (within float epsilon).
                glm::vec3 claimedMin{0}, claimedMax{0};
                bool hasClaimed = (acc.contains("min") && acc.contains("max"));
                if (hasClaimed) {
                    claimedMin.x = acc["min"][0]; claimedMin.y = acc["min"][1]; claimedMin.z = acc["min"][2];
                    claimedMax.x = acc["max"][0]; claimedMax.y = acc["max"][1]; claimedMax.z = acc["max"][2];
                    auto close = [](float a, float b) {
                        return std::abs(a - b) < 1e-3f;
                    };
                    bool ok = close(claimedMin.x, actualMin.x) &&
                              close(claimedMin.y, actualMin.y) &&
                              close(claimedMin.z, actualMin.z) &&
                              close(claimedMax.x, actualMax.x) &&
                              close(claimedMax.y, actualMax.y) &&
                              close(claimedMax.z, actualMax.z);
                    if (!ok) {
                        mismatched++;
                        char buf[256];
                        std::snprintf(buf, sizeof(buf),
                            "accessor %d bounds mismatch: claimed [%g,%g,%g]-[%g,%g,%g] vs actual [%g,%g,%g]-[%g,%g,%g]",
                            ai,
                            claimedMin.x, claimedMin.y, claimedMin.z,
                            claimedMax.x, claimedMax.y, claimedMax.z,
                            actualMin.x, actualMin.y, actualMin.z,
                            actualMax.x, actualMax.y, actualMax.z);
                        errors.push_back(buf);
                    }
                } else {
                    // glTF spec requires position accessors to declare min/max.
                    errors.push_back("accessor " + std::to_string(ai) +
                                     " missing required min/max for POSITION attribute");
                    mismatched++;
                }
            }
            if (jsonOut) {
                nlohmann::json j;
                j["glb"] = path;
                j["positionAccessors"] = posAccessors;
                j["mismatched"] = mismatched;
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return errors.empty() ? 0 : 1;
            }
            std::printf("GLB bounds: %s\n", path.c_str());
            std::printf("  position accessors checked : %d\n", posAccessors);
            std::printf("  mismatched                 : %d\n", mismatched);
            if (errors.empty()) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %zu error(s):\n", errors.size());
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--validate-stl") == 0 && i + 1 < argc) {
            // Structural validator for ASCII STL — pairs with --export-stl
            // and --import-stl (and --bake-zone-stl). Catches truncation,
            // missing solid framing, mismatched facet/vertex counts, and
            // non-finite vertex coords that would crash a slicer's mesh
            // analyzer.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path);
            if (!in) {
                std::fprintf(stderr,
                    "validate-stl: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<std::string> errors;
            std::string solidName;
            int facetCount = 0, vertCount = 0, nonFinite = 0;
            int facetsOpen = 0;  // facet-without-endfacet leak detector
            bool sawSolid = false, sawEndsolid = false;
            int currentFacetVerts = 0;
            std::string line;
            int lineNum = 0;
            while (std::getline(in, line)) {
                lineNum++;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.empty()) continue;
                std::istringstream ss(line);
                std::string tok;
                ss >> tok;
                if (tok == "solid") {
                    if (sawSolid) {
                        errors.push_back("line " + std::to_string(lineNum) +
                                         ": multiple 'solid' headers");
                    }
                    sawSolid = true;
                    ss >> solidName;
                } else if (tok == "facet") {
                    facetCount++;
                    facetsOpen++;
                    currentFacetVerts = 0;
                    std::string nrmTok;
                    ss >> nrmTok;
                    if (nrmTok != "normal") {
                        errors.push_back("line " + std::to_string(lineNum) +
                                         ": 'facet' missing 'normal' subtoken");
                    } else {
                        float nx, ny, nz;
                        if (!(ss >> nx >> ny >> nz)) {
                            errors.push_back("line " + std::to_string(lineNum) +
                                             ": 'facet normal' missing 3 floats");
                        } else if (!std::isfinite(nx) || !std::isfinite(ny) ||
                                    !std::isfinite(nz)) {
                            errors.push_back("line " + std::to_string(lineNum) +
                                             ": non-finite facet normal");
                            nonFinite++;
                        }
                    }
                } else if (tok == "vertex") {
                    vertCount++;
                    currentFacetVerts++;
                    float x, y, z;
                    if (!(ss >> x >> y >> z)) {
                        errors.push_back("line " + std::to_string(lineNum) +
                                         ": 'vertex' missing 3 floats");
                    } else if (!std::isfinite(x) || !std::isfinite(y) ||
                                !std::isfinite(z)) {
                        nonFinite++;
                        if (errors.size() < 30) {
                            errors.push_back("line " + std::to_string(lineNum) +
                                             ": non-finite vertex coord");
                        }
                    }
                } else if (tok == "endfacet") {
                    facetsOpen--;
                    if (currentFacetVerts != 3) {
                        errors.push_back("line " + std::to_string(lineNum) +
                                         ": facet has " +
                                         std::to_string(currentFacetVerts) +
                                         " vertices, expected exactly 3");
                    }
                } else if (tok == "endsolid") {
                    sawEndsolid = true;
                }
                // outer loop / endloop are required by spec but ignored
                // here; their absence doesn't break parsing as long as
                // the vertex count per facet is correct.
            }
            if (!sawSolid) errors.push_back("missing 'solid' header");
            if (!sawEndsolid) errors.push_back("missing 'endsolid' footer");
            if (facetsOpen != 0) {
                errors.push_back(std::to_string(facetsOpen) +
                                 " unclosed 'facet' (missing 'endfacet')");
            }
            if (vertCount != facetCount * 3) {
                errors.push_back("vertex count " + std::to_string(vertCount) +
                                 " != 3 * facet count " +
                                 std::to_string(facetCount));
            }
            if (jsonOut) {
                nlohmann::json j;
                j["stl"] = path;
                j["solidName"] = solidName;
                j["facetCount"] = facetCount;
                j["vertexCount"] = vertCount;
                j["nonFiniteCount"] = nonFinite;
                j["errorCount"] = errors.size();
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return errors.empty() ? 0 : 1;
            }
            std::printf("STL: %s\n", path.c_str());
            std::printf("  solid name : %s\n",
                        solidName.empty() ? "(unset)" : solidName.c_str());
            std::printf("  facets     : %d\n", facetCount);
            std::printf("  vertices   : %d\n", vertCount);
            if (nonFinite > 0) {
                std::printf("  non-finite : %d\n", nonFinite);
            }
            if (errors.empty()) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %zu error(s):\n", errors.size());
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--validate-png") == 0 && i + 1 < argc) {
            // Full PNG structural validator — beyond --info-png's
            // header-only sniff. Walks every chunk, verifies CRC,
            // ensures IHDR/IDAT/IEND are present and ordered correctly.
            // Catches the kind of corruption (truncation mid-IDAT,
            // bit-flip in CRC) that browsers/decoders silently skip.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr,
                    "validate-png: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            std::vector<std::string> errors;
            // PNG signature: 89 50 4E 47 0D 0A 1A 0A
            static const uint8_t kSig[8] = {0x89, 0x50, 0x4E, 0x47,
                                             0x0D, 0x0A, 0x1A, 0x0A};
            if (bytes.size() < 8 || std::memcmp(bytes.data(), kSig, 8) != 0) {
                errors.push_back("missing PNG signature");
            }
            // CRC32 table per PNG spec (matches the standard polynomial
            // 0xEDB88320; building once via constexpr-eligible logic).
            uint32_t crcTable[256];
            for (uint32_t n = 0; n < 256; ++n) {
                uint32_t c = n;
                for (int k = 0; k < 8; ++k) {
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                crcTable[n] = c;
            }
            auto crc32 = [&](const uint8_t* data, size_t len) {
                uint32_t c = 0xFFFFFFFFu;
                for (size_t k = 0; k < len; ++k) {
                    c = crcTable[(c ^ data[k]) & 0xFF] ^ (c >> 8);
                }
                return c ^ 0xFFFFFFFFu;
            };
            auto be32 = [](const uint8_t* p) {
                return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                       (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
            };
            int chunkCount = 0;
            int badCrcs = 0;
            bool sawIHDR = false, sawIDAT = false, sawIEND = false;
            bool ihdrFirst = false;
            std::string firstChunkType;
            uint32_t width = 0, height = 0;
            uint8_t bitDepth = 0, colorType = 0;
            // Walk chunks: each is length(4) + type(4) + data(length) + crc(4).
            size_t off = 8;
            while (errors.empty() && off + 12 <= bytes.size()) {
                uint32_t len = be32(&bytes[off]);
                if (off + 8 + len + 4 > bytes.size()) {
                    errors.push_back("chunk at offset " + std::to_string(off) +
                                     " extends past file end");
                    break;
                }
                std::string type(reinterpret_cast<const char*>(&bytes[off + 4]), 4);
                if (chunkCount == 0) {
                    firstChunkType = type;
                    ihdrFirst = (type == "IHDR");
                }
                chunkCount++;
                if (type == "IHDR") {
                    sawIHDR = true;
                    if (len >= 13) {
                        width = be32(&bytes[off + 8]);
                        height = be32(&bytes[off + 12]);
                        bitDepth = bytes[off + 16];
                        colorType = bytes[off + 17];
                    }
                } else if (type == "IDAT") {
                    sawIDAT = true;
                } else if (type == "IEND") {
                    sawIEND = true;
                }
                // Verify CRC (computed over type + data, not length).
                uint32_t storedCrc = be32(&bytes[off + 8 + len]);
                uint32_t actualCrc = crc32(&bytes[off + 4], 4 + len);
                if (storedCrc != actualCrc) {
                    badCrcs++;
                    if (errors.size() < 10) {
                        char buf[128];
                        std::snprintf(buf, sizeof(buf),
                            "chunk '%s' at offset %zu: CRC mismatch (stored=0x%08X actual=0x%08X)",
                            type.c_str(), off, storedCrc, actualCrc);
                        errors.push_back(buf);
                    }
                }
                off += 8 + len + 4;
            }
            if (!ihdrFirst) {
                errors.push_back("first chunk is '" + firstChunkType +
                                  "', expected 'IHDR'");
            }
            if (!sawIHDR) errors.push_back("missing required IHDR chunk");
            if (!sawIDAT) errors.push_back("missing required IDAT chunk");
            if (!sawIEND) errors.push_back("missing required IEND chunk");
            if (off < bytes.size()) {
                errors.push_back(std::to_string(bytes.size() - off) +
                                  " trailing bytes after IEND chunk");
            }
            if (jsonOut) {
                nlohmann::json j;
                j["png"] = path;
                j["width"] = width;
                j["height"] = height;
                j["bitDepth"] = bitDepth;
                j["colorType"] = colorType;
                j["chunkCount"] = chunkCount;
                j["badCrcs"] = badCrcs;
                j["fileSize"] = bytes.size();
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return errors.empty() ? 0 : 1;
            }
            std::printf("PNG: %s\n", path.c_str());
            std::printf("  size       : %u x %u\n", width, height);
            std::printf("  bit depth  : %u (color type %u)\n", bitDepth, colorType);
            std::printf("  chunks     : %d (%d CRC mismatches)\n",
                        chunkCount, badCrcs);
            std::printf("  file bytes : %zu\n", bytes.size());
            if (errors.empty()) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %zu error(s):\n", errors.size());
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--validate-blp") == 0 && i + 1 < argc) {
            // BLP structural validator. --info-blp shows header fields
            // (full decode); this checks structural invariants without
            // decoding pixels — useful for spot-checking thousands of
            // BLPs in an extract dir without paying the DXT decompress
            // cost on each.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr,
                    "validate-blp: cannot open %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            std::vector<std::string> errors;
            uint32_t width = 0, height = 0;
            std::string magic;
            int validMips = 0;
            // BLP1 and BLP2 share magic 'BLP1' / 'BLP2' at byte 0; both
            // have 16 mipOffset slots + 16 mipSize slots after the
            // initial header (offsets vary by version).
            if (bytes.size() < 8) {
                errors.push_back("file too short to be a BLP");
            } else {
                magic.assign(bytes.begin(), bytes.begin() + 4);
                if (magic != "BLP1" && magic != "BLP2") {
                    errors.push_back("magic is '" + magic + "', expected 'BLP1' or 'BLP2'");
                }
            }
            // BLP1 layout (post-magic):
            //   compression(4) + alphaBits(4) + width(4) + height(4) +
            //   extra(4) + hasMips(4) + mipOffsets[16](64) + mipSizes[16](64) +
            //   palette[256](1024)  [palette only present if compression==1]
            // BLP2 layout (post-magic):
            //   version(4) + compression(1) + alphaDepth(1) +
            //   alphaEncoding(1) + hasMips(1) + width(4) + height(4) +
            //   mipOffsets[16](64) + mipSizes[16](64) + palette[256](1024)
            uint32_t mipOffPos = 0, mipSzPos = 0;
            if (errors.empty()) {
                auto le32 = [&](size_t off) {
                    uint32_t v = 0;
                    if (off + 4 <= bytes.size()) std::memcpy(&v, &bytes[off], 4);
                    return v;
                };
                if (magic == "BLP1") {
                    width  = le32(4 + 8);   // skip magic + comp + alphaBits
                    height = le32(4 + 12);
                    mipOffPos = 4 + 24;     // after extra + hasMips
                    mipSzPos  = 4 + 24 + 64;
                } else {
                    width  = le32(4 + 8);   // BLP2: skip magic + version + 4 bytes
                    height = le32(4 + 12);
                    mipOffPos = 4 + 16;
                    mipSzPos  = 4 + 16 + 64;
                }
                if (width == 0 || height == 0) {
                    errors.push_back("zero width or height in header");
                }
                if (width > 8192 || height > 8192) {
                    errors.push_back("dimensions " + std::to_string(width) +
                                     "x" + std::to_string(height) +
                                     " exceed 8192 (rejected by texture exporter)");
                }
                // Walk the mipOffset/mipSize tables and verify each
                // mip's data range is within the file. Stops at the
                // first zero offset (BLP convention for unused slots).
                if (mipSzPos + 64 <= bytes.size()) {
                    for (int m = 0; m < 16; ++m) {
                        uint32_t off = le32(mipOffPos + m * 4);
                        uint32_t sz  = le32(mipSzPos  + m * 4);
                        if (off == 0 && sz == 0) break;  // unused slot
                        if (off == 0 || sz == 0) {
                            errors.push_back("mip " + std::to_string(m) +
                                             " has off=0 but size=" +
                                             std::to_string(sz) + " (or vice versa)");
                            continue;
                        }
                        if (uint64_t(off) + sz > bytes.size()) {
                            errors.push_back("mip " + std::to_string(m) +
                                             " range [" + std::to_string(off) +
                                             ", " + std::to_string(off + sz) +
                                             ") past file end " +
                                             std::to_string(bytes.size()));
                        } else {
                            validMips++;
                        }
                    }
                }
            }
            if (jsonOut) {
                nlohmann::json j;
                j["blp"] = path;
                j["magic"] = magic;
                j["width"] = width;
                j["height"] = height;
                j["validMips"] = validMips;
                j["fileSize"] = bytes.size();
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return errors.empty() ? 0 : 1;
            }
            std::printf("BLP: %s\n", path.c_str());
            std::printf("  magic      : %s\n", magic.empty() ? "(none)" : magic.c_str());
            std::printf("  size       : %u x %u\n", width, height);
            std::printf("  valid mips : %d\n", validMips);
            std::printf("  file bytes : %zu\n", bytes.size());
            if (errors.empty()) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %zu error(s):\n", errors.size());
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--validate-jsondbc") == 0 && i + 1 < argc) {
            // Strict schema validator for JSON DBC sidecars. --info-jsondbc
            // checks that header recordCount matches the actual records[]
            // length; this goes deeper:
            //   - format tag is the wowee 1.0 string
            //   - source field present (so re-import knows which DBC slot)
            //   - recordCount + fieldCount are non-negative integers
            //   - records is an array
            //   - each record is an array exactly fieldCount long
            //   - each cell is string|number|bool|null (no objects/arrays)
            // Catches the kind of corruption that load() might silently
            // tolerate (missing fields default to 0/empty), letting the
            // editor's runtime DBC loader downstream-fail in confusing
            // ways.
            std::string path = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            std::ifstream in(path);
            if (!in) {
                std::fprintf(stderr,
                    "validate-jsondbc: cannot open %s\n", path.c_str());
                return 1;
            }
            nlohmann::json doc;
            std::vector<std::string> errors;
            try {
                in >> doc;
            } catch (const std::exception& e) {
                errors.push_back(std::string("JSON parse error: ") + e.what());
            }
            std::string format, source;
            uint32_t recordCount = 0, fieldCount = 0;
            uint32_t actualRecs = 0;
            int badRowWidths = 0, badCellTypes = 0;
            if (errors.empty()) {
                if (!doc.is_object()) {
                    errors.push_back("top-level value is not a JSON object");
                } else {
                    if (!doc.contains("format")) {
                        errors.push_back("missing 'format' field");
                    } else if (!doc["format"].is_string()) {
                        errors.push_back("'format' field is not a string");
                    } else {
                        format = doc["format"].get<std::string>();
                        if (format != "wowee-dbc-json-1.0") {
                            errors.push_back("'format' is '" + format +
                                             "', expected 'wowee-dbc-json-1.0'");
                        }
                    }
                    if (!doc.contains("source")) {
                        errors.push_back("missing 'source' field (re-import needs it)");
                    } else {
                        source = doc.value("source", std::string{});
                    }
                    if (!doc.contains("recordCount") ||
                        !doc["recordCount"].is_number_integer()) {
                        errors.push_back("'recordCount' missing or not an integer");
                    } else {
                        recordCount = doc["recordCount"].get<uint32_t>();
                    }
                    if (!doc.contains("fieldCount") ||
                        !doc["fieldCount"].is_number_integer()) {
                        errors.push_back("'fieldCount' missing or not an integer");
                    } else {
                        fieldCount = doc["fieldCount"].get<uint32_t>();
                    }
                    if (!doc.contains("records") || !doc["records"].is_array()) {
                        errors.push_back("'records' missing or not an array");
                    } else {
                        const auto& records = doc["records"];
                        actualRecs = static_cast<uint32_t>(records.size());
                        if (actualRecs != recordCount) {
                            errors.push_back("recordCount " + std::to_string(recordCount) +
                                             " != actual records " +
                                             std::to_string(actualRecs));
                        }
                        for (size_t r = 0; r < records.size(); ++r) {
                            const auto& row = records[r];
                            if (!row.is_array()) {
                                errors.push_back("record[" + std::to_string(r) +
                                                 "] is not an array");
                                continue;
                            }
                            if (row.size() != fieldCount) {
                                badRowWidths++;
                                if (badRowWidths <= 3) {
                                    errors.push_back("record[" + std::to_string(r) +
                                                     "] has " + std::to_string(row.size()) +
                                                     " cells, expected " +
                                                     std::to_string(fieldCount));
                                }
                            }
                            for (size_t c = 0; c < row.size(); ++c) {
                                const auto& cell = row[c];
                                bool ok = cell.is_string() || cell.is_number() ||
                                          cell.is_boolean() || cell.is_null();
                                if (!ok) {
                                    badCellTypes++;
                                    if (badCellTypes <= 3) {
                                        errors.push_back("record[" + std::to_string(r) +
                                                         "][" + std::to_string(c) +
                                                         "] has invalid type (objects/arrays not allowed)");
                                    }
                                }
                            }
                        }
                        if (badRowWidths > 3) {
                            errors.push_back("... and " + std::to_string(badRowWidths - 3) +
                                             " more rows with wrong cell count");
                        }
                        if (badCellTypes > 3) {
                            errors.push_back("... and " + std::to_string(badCellTypes - 3) +
                                             " more cells with invalid types");
                        }
                    }
                }
            }
            int errorCount = static_cast<int>(errors.size());
            if (jsonOut) {
                nlohmann::json j;
                j["jsondbc"] = path;
                j["format"] = format;
                j["source"] = source;
                j["recordCount"] = recordCount;
                j["fieldCount"] = fieldCount;
                j["actualRecords"] = actualRecs;
                j["errorCount"] = errorCount;
                j["errors"] = errors;
                j["passed"] = errors.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return errors.empty() ? 0 : 1;
            }
            std::printf("JSON DBC: %s\n", path.c_str());
            std::printf("  format    : %s\n", format.empty() ? "?" : format.c_str());
            std::printf("  source    : %s\n", source.empty() ? "?" : source.c_str());
            std::printf("  records   : %u (header) / %u (actual)\n",
                        recordCount, actualRecs);
            std::printf("  fields    : %u\n", fieldCount);
            if (errors.empty()) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %d error(s):\n", errorCount);
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--export-obj") == 0 && i + 1 < argc) {
            // Convert WOM (our open M2 replacement) to Wavefront OBJ — a
            // universally supported text format that opens directly in
            // Blender, MeshLab, ZBrush, Maya, and basically every other 3D
            // tool ever made. Makes the open-format ecosystem actually
            // useful for content authors who don't want to write a custom
            // WOM importer for their DCC of choice.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr, "WOM not found: %s.wom\n", base.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = base + ".obj";
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            if (!wom.isValid()) {
                std::fprintf(stderr, "WOM has no geometry to export: %s.wom\n", base.c_str());
                return 1;
            }
            std::ofstream obj(outPath);
            if (!obj) {
                std::fprintf(stderr, "Failed to open output file: %s\n", outPath.c_str());
                return 1;
            }
            // Header — preserves provenance so a designer reopening the OBJ
            // weeks later knows where it came from. The MTL line is a
            // courtesy: we don't currently emit a .mtl, but downstream
            // tools won't error without one either.
            obj << "# Wavefront OBJ generated by wowee_editor --export-obj\n";
            obj << "# Source: " << base << ".wom (v" << wom.version << ")\n";
            obj << "# Verts: " << wom.vertices.size()
                << " Tris: " << wom.indices.size() / 3
                << " Textures: " << wom.texturePaths.size() << "\n\n";
            obj << "o " << (wom.name.empty() ? "WoweeModel" : wom.name) << "\n";
            // Positions (v), texcoords (vt), normals (vn) — OBJ flips V so
            // that the same UVs that look right in our Vulkan renderer
            // also look right in Blender's bottom-left UV convention.
            for (const auto& v : wom.vertices) {
                obj << "v " << v.position.x << " " << v.position.y
                    << " " << v.position.z << "\n";
            }
            for (const auto& v : wom.vertices) {
                obj << "vt " << v.texCoord.x << " " << (1.0f - v.texCoord.y) << "\n";
            }
            for (const auto& v : wom.vertices) {
                obj << "vn " << v.normal.x << " " << v.normal.y
                    << " " << v.normal.z << "\n";
            }
            // Faces — split per-batch so each material/texture range becomes
            // its own group. Falls back to a single group when the WOM
            // wasn't authored with batches (WOM1/WOM2). OBJ indices are
            // 1-based, hence the +1.
            auto emitFaces = [&](const char* groupName,
                                  uint32_t start, uint32_t count) {
                obj << "g " << groupName << "\n";
                for (uint32_t k = 0; k < count; k += 3) {
                    uint32_t i0 = wom.indices[start + k] + 1;
                    uint32_t i1 = wom.indices[start + k + 1] + 1;
                    uint32_t i2 = wom.indices[start + k + 2] + 1;
                    obj << "f "
                        << i0 << "/" << i0 << "/" << i0 << " "
                        << i1 << "/" << i1 << "/" << i1 << " "
                        << i2 << "/" << i2 << "/" << i2 << "\n";
                }
            };
            if (wom.batches.empty()) {
                emitFaces("mesh", 0,
                          static_cast<uint32_t>(wom.indices.size()));
            } else {
                for (size_t b = 0; b < wom.batches.size(); ++b) {
                    const auto& batch = wom.batches[b];
                    std::string groupName = "batch_" + std::to_string(b);
                    if (batch.textureIndex < wom.texturePaths.size()) {
                        // Strip directory + extension for a readable group
                        // name; full path is preserved in the file header
                        // comment so nothing is lost.
                        std::string tex = wom.texturePaths[batch.textureIndex];
                        auto slash = tex.find_last_of("/\\");
                        if (slash != std::string::npos) tex = tex.substr(slash + 1);
                        auto dot = tex.find_last_of('.');
                        if (dot != std::string::npos) tex = tex.substr(0, dot);
                        if (!tex.empty()) groupName += "_" + tex;
                    }
                    emitFaces(groupName.c_str(), batch.indexStart, batch.indexCount);
                }
            }
            obj.close();
            std::printf("Exported %s.wom -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  %zu verts, %zu tris, %zu groups\n",
                        wom.vertices.size(), wom.indices.size() / 3,
                        wom.batches.empty() ? size_t(1) : wom.batches.size());
            return 0;
        } else if (std::strcmp(argv[i], "--export-glb") == 0 && i + 1 < argc) {
            // glTF 2.0 binary (.glb) export — modern industry standard
            // that, unlike OBJ, supports skinning + animations + PBR
            // materials natively. v1 here writes positions/normals/UVs/
            // indices as a single mesh (or one primitive per WOM3 batch);
            // bones/anims are deliberately not yet emitted because glTF's
            // joint matrix layout differs from WOM's bone tree and needs
            // a careful re-mapping pass.
            //
            // Why this matters: glTF is what Sketchfab, Three.js, Babylon.js,
            // and Unity/Unreal-via-import all consume. Shipping WOM through
            // .glb makes our open binary format viewable in any modern
            // browser-based 3D viewer with zero conversion friction.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr, "WOM not found: %s.wom\n", base.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = base + ".glb";
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            if (!wom.isValid()) {
                std::fprintf(stderr, "WOM has no geometry: %s.wom\n", base.c_str());
                return 1;
            }
            // BIN chunk layout — sections ordered so each accessor's
            // byteOffset is naturally aligned for its component type:
            //   positions (vec3 float)  : 12 bytes/vert, offset 0
            //   normals   (vec3 float)  : 12 bytes/vert
            //   uvs       (vec2 float)  :  8 bytes/vert
            //   indices   (uint32)      :  4 bytes each
            // After 32 bytes per vertex, indices start at a 4-byte aligned
            // offset for free.
            const uint32_t vCount = static_cast<uint32_t>(wom.vertices.size());
            const uint32_t iCount = static_cast<uint32_t>(wom.indices.size());
            const uint32_t posOff = 0;
            const uint32_t nrmOff = posOff + vCount * 12;
            const uint32_t uvOff  = nrmOff + vCount * 12;
            const uint32_t idxOff = uvOff  + vCount * 8;
            const uint32_t binSize = idxOff + iCount * 4;
            std::vector<uint8_t> bin(binSize);
            // Pack positions
            for (uint32_t v = 0; v < vCount; ++v) {
                const auto& vert = wom.vertices[v];
                std::memcpy(&bin[posOff + v * 12 + 0], &vert.position.x, 4);
                std::memcpy(&bin[posOff + v * 12 + 4], &vert.position.y, 4);
                std::memcpy(&bin[posOff + v * 12 + 8], &vert.position.z, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 0], &vert.normal.x, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 4], &vert.normal.y, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 8], &vert.normal.z, 4);
                std::memcpy(&bin[uvOff  + v * 8  + 0], &vert.texCoord.x, 4);
                std::memcpy(&bin[uvOff  + v * 8  + 4], &vert.texCoord.y, 4);
            }
            std::memcpy(&bin[idxOff], wom.indices.data(), iCount * 4);
            // Compute bounds for the position accessor's min/max — glTF
            // viewers rely on these for camera framing and culling.
            glm::vec3 bMin{1e30f}, bMax{-1e30f};
            for (const auto& v : wom.vertices) {
                bMin = glm::min(bMin, v.position);
                bMax = glm::max(bMax, v.position);
            }
            // Build the JSON structure. nlohmann::json keeps insertion
            // order in dump(), but glTF readers are key-based so order
            // doesn't matter functionally.
            nlohmann::json gj;
            gj["asset"] = {{"version", "2.0"},
                            {"generator", "wowee_editor --export-glb"}};
            gj["scene"] = 0;
            gj["scenes"] = nlohmann::json::array({nlohmann::json{{"nodes", {0}}}});
            gj["nodes"] = nlohmann::json::array({nlohmann::json{
                {"name", wom.name.empty() ? "WoweeModel" : wom.name},
                {"mesh", 0}
            }});
            gj["buffers"] = nlohmann::json::array({nlohmann::json{
                {"byteLength", binSize}
            }});
            // BufferViews: one per attribute + one per index range.
            // Per WOM3 batch we slice the index bufferView with separate
            // accessors so each batch becomes its own primitive.
            nlohmann::json bufferViews = nlohmann::json::array();
            // 0: positions, 1: normals, 2: uvs, 3: indices (whole range)
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", posOff},
                                    {"byteLength", vCount * 12},
                                    {"target", 34962}}); // ARRAY_BUFFER
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", nrmOff},
                                    {"byteLength", vCount * 12},
                                    {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", uvOff},
                                    {"byteLength", vCount * 8},
                                    {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", idxOff},
                                    {"byteLength", iCount * 4},
                                    {"target", 34963}}); // ELEMENT_ARRAY_BUFFER
            gj["bufferViews"] = bufferViews;
            // Accessors: 0=position, 1=normal, 2=uv, 3..N=indices (one
            // per primitive, sliced from bufferView 3).
            nlohmann::json accessors = nlohmann::json::array();
            accessors.push_back({
                {"bufferView", 0}, {"componentType", 5126}, // FLOAT
                {"count", vCount}, {"type", "VEC3"},
                {"min", {bMin.x, bMin.y, bMin.z}},
                {"max", {bMax.x, bMax.y, bMax.z}}
            });
            accessors.push_back({
                {"bufferView", 1}, {"componentType", 5126},
                {"count", vCount}, {"type", "VEC3"}
            });
            accessors.push_back({
                {"bufferView", 2}, {"componentType", 5126},
                {"count", vCount}, {"type", "VEC2"}
            });
            // Build primitives — one per WOM3 batch, or one over the
            // whole index range if no batches.
            nlohmann::json primitives = nlohmann::json::array();
            auto addPrimitive = [&](uint32_t idxStart, uint32_t idxCount) {
                uint32_t accessorIdx = static_cast<uint32_t>(accessors.size());
                accessors.push_back({
                    {"bufferView", 3},
                    {"byteOffset", idxStart * 4},
                    {"componentType", 5125}, // UNSIGNED_INT
                    {"count", idxCount},
                    {"type", "SCALAR"}
                });
                primitives.push_back({
                    {"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                    {"indices", accessorIdx},
                    {"mode", 4} // TRIANGLES
                });
            };
            if (wom.batches.empty()) {
                addPrimitive(0, iCount);
            } else {
                for (const auto& b : wom.batches) {
                    addPrimitive(b.indexStart, b.indexCount);
                }
            }
            gj["accessors"] = accessors;
            gj["meshes"] = nlohmann::json::array({nlohmann::json{
                {"primitives", primitives}
            }});
            // Serialize JSON to bytes; pad to 4-byte boundary with spaces
            // (glTF spec requires JSON chunk padded with 0x20).
            std::string jsonStr = gj.dump();
            while (jsonStr.size() % 4 != 0) jsonStr += ' ';
            // BIN chunk pads to 4-byte boundary with zeros (already
            // satisfied since binSize = idxOff + iCount*4 and idxOff is
            // 4-byte aligned).
            uint32_t jsonLen = static_cast<uint32_t>(jsonStr.size());
            uint32_t binLen = binSize;
            uint32_t totalLen = 12 + 8 + jsonLen + 8 + binLen;
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "Failed to open output: %s\n", outPath.c_str());
                return 1;
            }
            // Header: magic, version, total length (all little-endian uint32)
            uint32_t magic = 0x46546C67;  // 'glTF'
            uint32_t version = 2;
            out.write(reinterpret_cast<const char*>(&magic), 4);
            out.write(reinterpret_cast<const char*>(&version), 4);
            out.write(reinterpret_cast<const char*>(&totalLen), 4);
            // JSON chunk header + payload
            uint32_t jsonChunkType = 0x4E4F534A;  // 'JSON'
            out.write(reinterpret_cast<const char*>(&jsonLen), 4);
            out.write(reinterpret_cast<const char*>(&jsonChunkType), 4);
            out.write(jsonStr.data(), jsonLen);
            // BIN chunk header + payload
            uint32_t binChunkType = 0x004E4942;  // 'BIN\0'
            out.write(reinterpret_cast<const char*>(&binLen), 4);
            out.write(reinterpret_cast<const char*>(&binChunkType), 4);
            out.write(reinterpret_cast<const char*>(bin.data()), binLen);
            out.close();
            std::printf("Exported %s.wom -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  %u verts, %u tris, %zu primitive(s), %u-byte binary chunk\n",
                        vCount, iCount / 3, primitives.size(), binLen);
            return 0;
        } else if (std::strcmp(argv[i], "--export-stl") == 0 && i + 1 < argc) {
            // ASCII STL export — single most universal 3D-printer format.
            // Cura, PrusaSlicer, Bambu Studio, Slic3r, OctoPrint, MakerBot
            // — every slicer made in the last 25 years opens STL natively.
            // Lets WOM models drive physical prints with no conversion
            // friction beyond this one command.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr, "WOM not found: %s.wom\n", base.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = base + ".stl";
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            if (!wom.isValid()) {
                std::fprintf(stderr, "WOM has no geometry: %s.wom\n", base.c_str());
                return 1;
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr, "Failed to open output: %s\n", outPath.c_str());
                return 1;
            }
            // STL solid name must be alphanumeric + underscores per loose
            // convention; sanitize whatever the WOM name contains. Empty
            // -> 'wowee_model'.
            std::string solidName = wom.name.empty() ? "wowee_model" : wom.name;
            for (auto& c : solidName) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_')) c = '_';
            }
            out << "solid " << solidName << "\n";
            // Per-triangle facet — STL has no shared vertex pool, every
            // triangle stands alone. Compute face normal from cross product
            // (STL spec requires unit-length face normal; viewers fall
            // back to per-vertex if zero, but most slicers want the real
            // value for orientation hints).
            uint32_t triCount = 0;
            for (size_t k = 0; k + 2 < wom.indices.size(); k += 3) {
                uint32_t i0 = wom.indices[k];
                uint32_t i1 = wom.indices[k + 1];
                uint32_t i2 = wom.indices[k + 2];
                if (i0 >= wom.vertices.size() || i1 >= wom.vertices.size() ||
                    i2 >= wom.vertices.size()) continue;
                const auto& v0 = wom.vertices[i0].position;
                const auto& v1 = wom.vertices[i1].position;
                const auto& v2 = wom.vertices[i2].position;
                glm::vec3 e1 = v1 - v0;
                glm::vec3 e2 = v2 - v0;
                glm::vec3 n = glm::cross(e1, e2);
                float len = glm::length(n);
                if (len > 1e-12f) n /= len;
                else n = {0, 0, 1};  // degenerate — STL spec allows any unit normal
                out << "  facet normal " << n.x << " " << n.y << " " << n.z << "\n"
                    << "    outer loop\n"
                    << "      vertex " << v0.x << " " << v0.y << " " << v0.z << "\n"
                    << "      vertex " << v1.x << " " << v1.y << " " << v1.z << "\n"
                    << "      vertex " << v2.x << " " << v2.y << " " << v2.z << "\n"
                    << "    endloop\n"
                    << "  endfacet\n";
                triCount++;
            }
            out << "endsolid " << solidName << "\n";
            out.close();
            std::printf("Exported %s.wom -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  solid '%s', %u facets\n",
                        solidName.c_str(), triCount);
            return 0;
        } else if (std::strcmp(argv[i], "--import-stl") == 0 && i + 1 < argc) {
            // ASCII STL -> WOM. Closes the STL round trip so designers can
            // edit prints in TinkerCAD/Meshmixer/SolidWorks and bring them
            // back to the engine. Dedupes vertices on (pos, normal) so the
            // resulting WOM vertex buffer stays compact.
            std::string stlPath = argv[++i];
            std::string womBase;
            if (i + 1 < argc && argv[i + 1][0] != '-') womBase = argv[++i];
            if (!std::filesystem::exists(stlPath)) {
                std::fprintf(stderr, "STL not found: %s\n", stlPath.c_str());
                return 1;
            }
            if (womBase.empty()) {
                womBase = stlPath;
                if (womBase.size() >= 4 &&
                    womBase.substr(womBase.size() - 4) == ".stl") {
                    womBase = womBase.substr(0, womBase.size() - 4);
                }
            }
            std::ifstream in(stlPath);
            if (!in) {
                std::fprintf(stderr, "Failed to open STL: %s\n", stlPath.c_str());
                return 1;
            }
            wowee::pipeline::WoweeModel wom;
            wom.version = 1;
            // Dedupe key: 6 floats (pos + normal) packed as a string. Loose
            // matching, but exact for round-trips since we write the same
            // floats back. Real-world STLs from CAD tools rarely benefit
            // from looser tolerance — they already share verts at the
            // exporter level.
            std::unordered_map<std::string, uint32_t> dedupe;
            auto interVert = [&](const glm::vec3& pos, const glm::vec3& nrm) {
                char key[128];
                std::snprintf(key, sizeof(key), "%.6f|%.6f|%.6f|%.6f|%.6f|%.6f",
                              pos.x, pos.y, pos.z, nrm.x, nrm.y, nrm.z);
                auto it = dedupe.find(key);
                if (it != dedupe.end()) return it->second;
                wowee::pipeline::WoweeModel::Vertex v;
                v.position = pos;
                v.normal = nrm;
                v.texCoord = {0, 0};
                uint32_t idx = static_cast<uint32_t>(wom.vertices.size());
                wom.vertices.push_back(v);
                dedupe[key] = idx;
                return idx;
            };
            std::string line;
            std::string solidName;
            // Per-facet state: parsed normal + accumulating vertex queue.
            glm::vec3 currentNormal{0, 0, 1};
            std::vector<glm::vec3> facetVerts;
            int facetCount = 0;
            while (std::getline(in, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                std::istringstream ss(line);
                std::string tok;
                ss >> tok;
                if (tok == "solid" && solidName.empty()) {
                    ss >> solidName;
                } else if (tok == "facet") {
                    std::string normalKw;
                    ss >> normalKw;
                    if (normalKw == "normal") {
                        ss >> currentNormal.x >> currentNormal.y >> currentNormal.z;
                    }
                    facetVerts.clear();
                } else if (tok == "vertex") {
                    glm::vec3 v;
                    ss >> v.x >> v.y >> v.z;
                    facetVerts.push_back(v);
                } else if (tok == "endfacet") {
                    if (facetVerts.size() == 3) {
                        // Use the facet normal for all 3 verts since STL
                        // doesn't carry per-vertex normals. Glue-points to
                        // adjacent facets will get distinct verts (which is
                        // correct for faceted-shading STL geometry).
                        for (const auto& v : facetVerts) {
                            wom.indices.push_back(interVert(v, currentNormal));
                        }
                        facetCount++;
                    }
                    facetVerts.clear();
                }
                // 'outer loop', 'endloop', 'endsolid' ignored — we infer
                // from the vertex count per facet.
            }
            if (wom.vertices.empty() || wom.indices.empty()) {
                std::fprintf(stderr,
                    "import-stl: no geometry parsed from %s\n", stlPath.c_str());
                return 1;
            }
            wom.name = solidName.empty()
                ? std::filesystem::path(stlPath).stem().string()
                : solidName;
            // Compute bounds — renderer culls by these so wrong values
            // make models disappear at distance.
            wom.boundMin = wom.vertices[0].position;
            wom.boundMax = wom.boundMin;
            for (const auto& v : wom.vertices) {
                wom.boundMin = glm::min(wom.boundMin, v.position);
                wom.boundMax = glm::max(wom.boundMax, v.position);
            }
            glm::vec3 center = (wom.boundMin + wom.boundMax) * 0.5f;
            float r2 = 0;
            for (const auto& v : wom.vertices) {
                glm::vec3 d = v.position - center;
                r2 = std::max(r2, glm::dot(d, d));
            }
            wom.boundRadius = std::sqrt(r2);
            if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
                std::fprintf(stderr, "import-stl: failed to write %s.wom\n",
                             womBase.c_str());
                return 1;
            }
            std::printf("Imported %s -> %s.wom\n", stlPath.c_str(), womBase.c_str());
            std::printf("  %d facets, %zu verts (deduped), bounds [%.2f, %.2f, %.2f] - [%.2f, %.2f, %.2f]\n",
                        facetCount, wom.vertices.size(),
                        wom.boundMin.x, wom.boundMin.y, wom.boundMin.z,
                        wom.boundMax.x, wom.boundMax.y, wom.boundMax.z);
            return 0;
        } else if (std::strcmp(argv[i], "--export-wob-glb") == 0 && i + 1 < argc) {
            // glTF 2.0 binary export for WOB. Same purpose as --export-glb
            // for WOM but adapted for buildings: each WOB group becomes
            // one primitive in a single mesh, sharing one big vertex
            // pool concatenated from per-group vertex arrays.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wob")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeBuildingLoader::exists(base)) {
                std::fprintf(stderr, "WOB not found: %s.wob\n", base.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = base + ".glb";
            auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
            if (!bld.isValid()) {
                std::fprintf(stderr, "WOB has no groups: %s.wob\n", base.c_str());
                return 1;
            }
            // Total counts + per-group offsets needed before allocating
            // the BIN buffer. Index buffer is uint32 so groups can each
            // index into the global pool by offset.
            uint32_t totalV = 0, totalI = 0;
            std::vector<uint32_t> groupVertOff(bld.groups.size(), 0);
            std::vector<uint32_t> groupIdxOff(bld.groups.size(), 0);
            for (size_t g = 0; g < bld.groups.size(); ++g) {
                groupVertOff[g] = totalV;
                groupIdxOff[g] = totalI;
                totalV += static_cast<uint32_t>(bld.groups[g].vertices.size());
                totalI += static_cast<uint32_t>(bld.groups[g].indices.size());
            }
            if (totalV == 0 || totalI == 0) {
                std::fprintf(stderr, "WOB has no vertex data\n");
                return 1;
            }
            const uint32_t posOff = 0;
            const uint32_t nrmOff = posOff + totalV * 12;
            const uint32_t uvOff  = nrmOff + totalV * 12;
            const uint32_t idxOff = uvOff  + totalV * 8;
            const uint32_t binSize = idxOff + totalI * 4;
            std::vector<uint8_t> bin(binSize);
            // Pack per-group geometry into the global pool. Indices get
            // offset by the group's starting vertex index so they
            // continue to reference the right vertices in the merged pool.
            uint32_t vCursor = 0, iCursor = 0;
            glm::vec3 bMin{1e30f}, bMax{-1e30f};
            for (size_t g = 0; g < bld.groups.size(); ++g) {
                const auto& grp = bld.groups[g];
                for (const auto& v : grp.vertices) {
                    std::memcpy(&bin[posOff + vCursor * 12 + 0], &v.position.x, 4);
                    std::memcpy(&bin[posOff + vCursor * 12 + 4], &v.position.y, 4);
                    std::memcpy(&bin[posOff + vCursor * 12 + 8], &v.position.z, 4);
                    std::memcpy(&bin[nrmOff + vCursor * 12 + 0], &v.normal.x, 4);
                    std::memcpy(&bin[nrmOff + vCursor * 12 + 4], &v.normal.y, 4);
                    std::memcpy(&bin[nrmOff + vCursor * 12 + 8], &v.normal.z, 4);
                    std::memcpy(&bin[uvOff  + vCursor * 8  + 0], &v.texCoord.x, 4);
                    std::memcpy(&bin[uvOff  + vCursor * 8  + 4], &v.texCoord.y, 4);
                    bMin = glm::min(bMin, v.position);
                    bMax = glm::max(bMax, v.position);
                    vCursor++;
                }
                // Offset indices by group's vertex base so merged pool
                // indexing still works. uint32 indices, written LE.
                for (uint32_t idx : grp.indices) {
                    uint32_t off = idx + groupVertOff[g];
                    std::memcpy(&bin[idxOff + iCursor * 4], &off, 4);
                    iCursor++;
                }
            }
            // Build glTF JSON.
            nlohmann::json gj;
            gj["asset"] = {{"version", "2.0"},
                            {"generator", "wowee_editor --export-wob-glb"}};
            gj["scene"] = 0;
            gj["scenes"] = nlohmann::json::array({nlohmann::json{{"nodes", {0}}}});
            gj["nodes"] = nlohmann::json::array({nlohmann::json{
                {"name", bld.name.empty() ? "WoweeBuilding" : bld.name},
                {"mesh", 0}
            }});
            gj["buffers"] = nlohmann::json::array({nlohmann::json{
                {"byteLength", binSize}
            }});
            nlohmann::json bufferViews = nlohmann::json::array();
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", posOff},
                                    {"byteLength", totalV * 12}, {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", nrmOff},
                                    {"byteLength", totalV * 12}, {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", uvOff},
                                    {"byteLength", totalV * 8},  {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", idxOff},
                                    {"byteLength", totalI * 4},  {"target", 34963}});
            gj["bufferViews"] = bufferViews;
            nlohmann::json accessors = nlohmann::json::array();
            accessors.push_back({
                {"bufferView", 0}, {"componentType", 5126},
                {"count", totalV}, {"type", "VEC3"},
                {"min", {bMin.x, bMin.y, bMin.z}},
                {"max", {bMax.x, bMax.y, bMax.z}}
            });
            accessors.push_back({{"bufferView", 1}, {"componentType", 5126},
                                  {"count", totalV}, {"type", "VEC3"}});
            accessors.push_back({{"bufferView", 2}, {"componentType", 5126},
                                  {"count", totalV}, {"type", "VEC2"}});
            // Per-group primitives — each gets its own indices accessor
            // sliced from the shared index bufferView via byteOffset.
            nlohmann::json primitives = nlohmann::json::array();
            for (size_t g = 0; g < bld.groups.size(); ++g) {
                uint32_t accIdx = static_cast<uint32_t>(accessors.size());
                accessors.push_back({
                    {"bufferView", 3},
                    {"byteOffset", groupIdxOff[g] * 4},
                    {"componentType", 5125},
                    {"count", bld.groups[g].indices.size()},
                    {"type", "SCALAR"}
                });
                primitives.push_back({
                    {"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                    {"indices", accIdx},
                    {"mode", 4}
                });
            }
            gj["accessors"] = accessors;
            gj["meshes"] = nlohmann::json::array({nlohmann::json{
                {"primitives", primitives}
            }});
            std::string jsonStr = gj.dump();
            while (jsonStr.size() % 4 != 0) jsonStr += ' ';
            uint32_t jsonLen = static_cast<uint32_t>(jsonStr.size());
            uint32_t binLen = binSize;
            uint32_t totalLen = 12 + 8 + jsonLen + 8 + binLen;
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "Failed to open output: %s\n", outPath.c_str());
                return 1;
            }
            uint32_t magic = 0x46546C67;
            uint32_t version = 2;
            out.write(reinterpret_cast<const char*>(&magic), 4);
            out.write(reinterpret_cast<const char*>(&version), 4);
            out.write(reinterpret_cast<const char*>(&totalLen), 4);
            uint32_t jsonChunkType = 0x4E4F534A;
            out.write(reinterpret_cast<const char*>(&jsonLen), 4);
            out.write(reinterpret_cast<const char*>(&jsonChunkType), 4);
            out.write(jsonStr.data(), jsonLen);
            uint32_t binChunkType = 0x004E4942;
            out.write(reinterpret_cast<const char*>(&binLen), 4);
            out.write(reinterpret_cast<const char*>(&binChunkType), 4);
            out.write(reinterpret_cast<const char*>(bin.data()), binLen);
            out.close();
            std::printf("Exported %s.wob -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  %zu groups -> %zu primitives, %u verts, %u tris, %u-byte BIN\n",
                        bld.groups.size(), primitives.size(),
                        totalV, totalI / 3, binLen);
            return 0;
        } else if (std::strcmp(argv[i], "--export-whm-glb") == 0 && i + 1 < argc) {
            // glTF 2.0 binary export for WHM/WOT terrain. Mirrors
            // --export-whm-obj's mesh layout (9x9 outer grid per chunk
            // → 8x8 quads → 2 tris each), but ships as a single .glb
            // viewable in any modern web 3D tool. Per-chunk primitives
            // so designers can hide individual chunks in three.js.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            for (const char* ext : {".wot", ".whm"}) {
                if (base.size() >= 4 && base.substr(base.size() - 4) == ext) {
                    base = base.substr(0, base.size() - 4);
                    break;
                }
            }
            if (!wowee::pipeline::WoweeTerrainLoader::exists(base)) {
                std::fprintf(stderr, "WHM/WOT not found: %s.{whm,wot}\n", base.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = base + ".glb";
            wowee::pipeline::ADTTerrain terrain;
            wowee::pipeline::WoweeTerrainLoader::load(base, terrain);
            // Same coord constants as --export-whm-obj so the .glb and
            // .obj of the same source align spatially.
            constexpr float kTileSize = 533.33333f;
            constexpr float kChunkSize = kTileSize / 16.0f;
            constexpr float kVertSpacing = kChunkSize / 8.0f;
            // Walk the 16x16 chunk grid, build per-chunk vertex + index
            // arrays. Hole bits respected (cave-entrance quads dropped).
            struct ChunkMesh { uint32_t vertOff, vertCount, idxOff, idxCount; };
            std::vector<ChunkMesh> chunkMeshes;
            std::vector<glm::vec3> positions;  // packed sequentially
            std::vector<uint32_t>  indices;
            int loadedChunks = 0;
            glm::vec3 bMin{1e30f}, bMax{-1e30f};
            for (int cx = 0; cx < 16; ++cx) {
                for (int cy = 0; cy < 16; ++cy) {
                    const auto& chunk = terrain.getChunk(cx, cy);
                    if (!chunk.heightMap.isLoaded()) continue;
                    loadedChunks++;
                    ChunkMesh cm{};
                    cm.vertOff = static_cast<uint32_t>(positions.size());
                    cm.idxOff  = static_cast<uint32_t>(indices.size());
                    float chunkBaseX = (32.0f - terrain.coord.y) * kTileSize - cy * kChunkSize;
                    float chunkBaseY = (32.0f - terrain.coord.x) * kTileSize - cx * kChunkSize;
                    // 9x9 outer verts (skip 8x8 inner fan-center verts).
                    for (int row = 0; row < 9; ++row) {
                        for (int col = 0; col < 9; ++col) {
                            glm::vec3 p{
                                chunkBaseX - row * kVertSpacing,
                                chunkBaseY - col * kVertSpacing,
                                chunk.position[2] + chunk.heightMap.heights[row * 17 + col]
                            };
                            positions.push_back(p);
                            bMin = glm::min(bMin, p);
                            bMax = glm::max(bMax, p);
                        }
                    }
                    cm.vertCount = 81;
                    bool isHoleChunk = (chunk.holes != 0);
                    auto idx = [&](int r, int c) { return cm.vertOff + r * 9 + c; };
                    for (int row = 0; row < 8; ++row) {
                        for (int col = 0; col < 8; ++col) {
                            if (isHoleChunk) {
                                int hx = col / 2, hy = row / 2;
                                if (chunk.holes & (1 << (hy * 4 + hx))) continue;
                            }
                            indices.push_back(idx(row, col));
                            indices.push_back(idx(row, col + 1));
                            indices.push_back(idx(row + 1, col + 1));
                            indices.push_back(idx(row, col));
                            indices.push_back(idx(row + 1, col + 1));
                            indices.push_back(idx(row + 1, col));
                        }
                    }
                    cm.idxCount = static_cast<uint32_t>(indices.size()) - cm.idxOff;
                    chunkMeshes.push_back(cm);
                }
            }
            if (loadedChunks == 0) {
                std::fprintf(stderr, "WHM has no loaded chunks\n");
                return 1;
            }
            // Synthesize normals as +Z (terrain is Z-up). Real per-vertex
            // normals would need a smoothing pass across chunk boundaries
            // — skip for v1, viewers can compute their own from positions.
            const uint32_t totalV = static_cast<uint32_t>(positions.size());
            const uint32_t totalI = static_cast<uint32_t>(indices.size());
            const uint32_t posOff = 0;
            const uint32_t nrmOff = posOff + totalV * 12;
            const uint32_t idxOff = nrmOff + totalV * 12;
            const uint32_t binSize = idxOff + totalI * 4;
            std::vector<uint8_t> bin(binSize);
            for (uint32_t v = 0; v < totalV; ++v) {
                std::memcpy(&bin[posOff + v * 12 + 0], &positions[v].x, 4);
                std::memcpy(&bin[posOff + v * 12 + 4], &positions[v].y, 4);
                std::memcpy(&bin[posOff + v * 12 + 8], &positions[v].z, 4);
                float nx = 0, ny = 0, nz = 1;
                std::memcpy(&bin[nrmOff + v * 12 + 0], &nx, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 4], &ny, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 8], &nz, 4);
            }
            std::memcpy(&bin[idxOff], indices.data(), totalI * 4);
            // Build glTF JSON.
            nlohmann::json gj;
            gj["asset"] = {{"version", "2.0"},
                            {"generator", "wowee_editor --export-whm-glb"}};
            gj["scene"] = 0;
            gj["scenes"] = nlohmann::json::array({nlohmann::json{{"nodes", {0}}}});
            std::string nodeName = "WoweeTerrain_" + std::to_string(terrain.coord.x) +
                                    "_" + std::to_string(terrain.coord.y);
            gj["nodes"] = nlohmann::json::array({nlohmann::json{
                {"name", nodeName}, {"mesh", 0}
            }});
            gj["buffers"] = nlohmann::json::array({nlohmann::json{
                {"byteLength", binSize}
            }});
            nlohmann::json bufferViews = nlohmann::json::array();
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", posOff},
                                    {"byteLength", totalV * 12}, {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", nrmOff},
                                    {"byteLength", totalV * 12}, {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", idxOff},
                                    {"byteLength", totalI * 4},  {"target", 34963}});
            gj["bufferViews"] = bufferViews;
            nlohmann::json accessors = nlohmann::json::array();
            accessors.push_back({
                {"bufferView", 0}, {"componentType", 5126},
                {"count", totalV}, {"type", "VEC3"},
                {"min", {bMin.x, bMin.y, bMin.z}},
                {"max", {bMax.x, bMax.y, bMax.z}}
            });
            accessors.push_back({{"bufferView", 1}, {"componentType", 5126},
                                  {"count", totalV}, {"type", "VEC3"}});
            // Per-chunk primitive — sliced from shared index bufferView.
            nlohmann::json primitives = nlohmann::json::array();
            for (const auto& cm : chunkMeshes) {
                if (cm.idxCount == 0) continue;  // all-hole chunk
                uint32_t accIdx = static_cast<uint32_t>(accessors.size());
                accessors.push_back({
                    {"bufferView", 2},
                    {"byteOffset", cm.idxOff * 4},
                    {"componentType", 5125},
                    {"count", cm.idxCount},
                    {"type", "SCALAR"}
                });
                primitives.push_back({
                    {"attributes", {{"POSITION", 0}, {"NORMAL", 1}}},
                    {"indices", accIdx},
                    {"mode", 4}
                });
            }
            gj["accessors"] = accessors;
            gj["meshes"] = nlohmann::json::array({nlohmann::json{
                {"primitives", primitives}
            }});
            std::string jsonStr = gj.dump();
            while (jsonStr.size() % 4 != 0) jsonStr += ' ';
            uint32_t jsonLen = static_cast<uint32_t>(jsonStr.size());
            uint32_t binLen = binSize;
            uint32_t totalLen = 12 + 8 + jsonLen + 8 + binLen;
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "Failed to open output: %s\n", outPath.c_str());
                return 1;
            }
            uint32_t magic = 0x46546C67, version = 2;
            out.write(reinterpret_cast<const char*>(&magic), 4);
            out.write(reinterpret_cast<const char*>(&version), 4);
            out.write(reinterpret_cast<const char*>(&totalLen), 4);
            uint32_t jsonChunkType = 0x4E4F534A;
            out.write(reinterpret_cast<const char*>(&jsonLen), 4);
            out.write(reinterpret_cast<const char*>(&jsonChunkType), 4);
            out.write(jsonStr.data(), jsonLen);
            uint32_t binChunkType = 0x004E4942;
            out.write(reinterpret_cast<const char*>(&binLen), 4);
            out.write(reinterpret_cast<const char*>(&binChunkType), 4);
            out.write(reinterpret_cast<const char*>(bin.data()), binLen);
            out.close();
            std::printf("Exported %s.whm -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  %d chunks loaded, %u verts, %u tris, %zu primitives, %u-byte BIN\n",
                        loadedChunks, totalV, totalI / 3, primitives.size(), binLen);
            return 0;
        } else if (std::strcmp(argv[i], "--bake-zone-glb") == 0 && i + 1 < argc) {
            // Bake every WHM tile in a zone into ONE .glb so the whole
            // multi-tile zone opens in three.js / model-viewer with one
            // file. Each tile becomes its own mesh+node so they can be
            // toggled independently. v1: terrain only — object/WOB
            // instances are a follow-up that needs careful per-mesh
            // bufferView slicing.
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "bake-zone-glb: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr,
                    "bake-zone-glb: failed to parse zone.json\n");
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/" + zm.mapName + ".glb";
            if (zm.tiles.empty()) {
                std::fprintf(stderr, "bake-zone-glb: zone has no tiles\n");
                return 1;
            }
            constexpr float kTileSize = 533.33333f;
            constexpr float kChunkSize = kTileSize / 16.0f;
            constexpr float kVertSpacing = kChunkSize / 8.0f;
            // Per-tile mesh metadata so we can create one node per tile
            // and slice its index range from the shared bufferView.
            struct TileMesh {
                int tx, ty;
                uint32_t vertOff, vertCount;
                uint32_t idxOff, idxCount;
            };
            std::vector<TileMesh> tileMeshes;
            std::vector<glm::vec3> positions;
            std::vector<uint32_t>  indices;
            int loadedTiles = 0;
            glm::vec3 bMin{1e30f}, bMax{-1e30f};
            for (const auto& [tx, ty] : zm.tiles) {
                std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                       std::to_string(tx) + "_" + std::to_string(ty);
                if (!wowee::pipeline::WoweeTerrainLoader::exists(tileBase)) {
                    std::fprintf(stderr,
                        "bake-zone-glb: tile (%d,%d) WHM/WOT missing — skipping\n",
                        tx, ty);
                    continue;
                }
                wowee::pipeline::ADTTerrain terrain;
                wowee::pipeline::WoweeTerrainLoader::load(tileBase, terrain);
                TileMesh tm{tx, ty, 0, 0, 0, 0};
                tm.vertOff = static_cast<uint32_t>(positions.size());
                tm.idxOff  = static_cast<uint32_t>(indices.size());
                // Same per-chunk outer-grid layout as --export-whm-glb,
                // but accumulated across all tiles so they share one
                // global vertex+index pool.
                for (int cx = 0; cx < 16; ++cx) {
                    for (int cy = 0; cy < 16; ++cy) {
                        const auto& chunk = terrain.getChunk(cx, cy);
                        if (!chunk.heightMap.isLoaded()) continue;
                        float chunkBaseX = (32.0f - terrain.coord.y) * kTileSize - cy * kChunkSize;
                        float chunkBaseY = (32.0f - terrain.coord.x) * kTileSize - cx * kChunkSize;
                        uint32_t chunkVertOff =
                            static_cast<uint32_t>(positions.size());
                        for (int row = 0; row < 9; ++row) {
                            for (int col = 0; col < 9; ++col) {
                                glm::vec3 p{
                                    chunkBaseX - row * kVertSpacing,
                                    chunkBaseY - col * kVertSpacing,
                                    chunk.position[2] +
                                        chunk.heightMap.heights[row * 17 + col]
                                };
                                positions.push_back(p);
                                bMin = glm::min(bMin, p);
                                bMax = glm::max(bMax, p);
                            }
                        }
                        bool isHoleChunk = (chunk.holes != 0);
                        for (int row = 0; row < 8; ++row) {
                            for (int col = 0; col < 8; ++col) {
                                if (isHoleChunk) {
                                    int hx = col / 2, hy = row / 2;
                                    if (chunk.holes & (1 << (hy * 4 + hx))) continue;
                                }
                                auto idx = [&](int r, int c) {
                                    return chunkVertOff + r * 9 + c;
                                };
                                indices.push_back(idx(row, col));
                                indices.push_back(idx(row, col + 1));
                                indices.push_back(idx(row + 1, col + 1));
                                indices.push_back(idx(row, col));
                                indices.push_back(idx(row + 1, col + 1));
                                indices.push_back(idx(row + 1, col));
                            }
                        }
                    }
                }
                tm.vertCount = static_cast<uint32_t>(positions.size()) - tm.vertOff;
                tm.idxCount  = static_cast<uint32_t>(indices.size()) - tm.idxOff;
                if (tm.vertCount > 0 && tm.idxCount > 0) {
                    tileMeshes.push_back(tm);
                    loadedTiles++;
                }
            }
            if (loadedTiles == 0) {
                std::fprintf(stderr, "bake-zone-glb: no tiles loaded\n");
                return 1;
            }
            // Pack BIN chunk same way as --export-whm-glb (positions +
            // synthetic +Z normals + indices). Per-tile accessors slice
            // their index region via byteOffset.
            const uint32_t totalV = static_cast<uint32_t>(positions.size());
            const uint32_t totalI = static_cast<uint32_t>(indices.size());
            const uint32_t posOff = 0;
            const uint32_t nrmOff = posOff + totalV * 12;
            const uint32_t idxOff = nrmOff + totalV * 12;
            const uint32_t binSize = idxOff + totalI * 4;
            std::vector<uint8_t> bin(binSize);
            for (uint32_t v = 0; v < totalV; ++v) {
                std::memcpy(&bin[posOff + v * 12 + 0], &positions[v].x, 4);
                std::memcpy(&bin[posOff + v * 12 + 4], &positions[v].y, 4);
                std::memcpy(&bin[posOff + v * 12 + 8], &positions[v].z, 4);
                float nx = 0, ny = 0, nz = 1;
                std::memcpy(&bin[nrmOff + v * 12 + 0], &nx, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 4], &ny, 4);
                std::memcpy(&bin[nrmOff + v * 12 + 8], &nz, 4);
            }
            std::memcpy(&bin[idxOff], indices.data(), totalI * 4);
            // Build glTF JSON. One mesh + one node per tile so they can
            // be toggled in viewers.
            nlohmann::json gj;
            gj["asset"] = {{"version", "2.0"},
                            {"generator", "wowee_editor --bake-zone-glb"}};
            gj["scene"] = 0;
            gj["buffers"] = nlohmann::json::array({nlohmann::json{
                {"byteLength", binSize}
            }});
            // Three shared bufferViews — pos, nrm, idx — sliced into
            // per-tile primitives via byteOffset on the index accessor.
            nlohmann::json bufferViews = nlohmann::json::array();
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", posOff},
                                    {"byteLength", totalV * 12}, {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", nrmOff},
                                    {"byteLength", totalV * 12}, {"target", 34962}});
            bufferViews.push_back({{"buffer", 0}, {"byteOffset", idxOff},
                                    {"byteLength", totalI * 4},  {"target", 34963}});
            gj["bufferViews"] = bufferViews;
            // Shared position+normal accessors (covering the full pool;
            // primitives reference them, the index accessor does the
            // per-tile slicing).
            nlohmann::json accessors = nlohmann::json::array();
            accessors.push_back({
                {"bufferView", 0}, {"componentType", 5126},
                {"count", totalV}, {"type", "VEC3"},
                {"min", {bMin.x, bMin.y, bMin.z}},
                {"max", {bMax.x, bMax.y, bMax.z}}
            });
            accessors.push_back({{"bufferView", 1}, {"componentType", 5126},
                                  {"count", totalV}, {"type", "VEC3"}});
            // Per-tile mesh + node + indices accessor.
            nlohmann::json meshes = nlohmann::json::array();
            nlohmann::json nodes = nlohmann::json::array();
            nlohmann::json sceneNodes = nlohmann::json::array();
            for (const auto& tm : tileMeshes) {
                uint32_t accIdx = static_cast<uint32_t>(accessors.size());
                accessors.push_back({
                    {"bufferView", 2},
                    {"byteOffset", tm.idxOff * 4},
                    {"componentType", 5125},
                    {"count", tm.idxCount},
                    {"type", "SCALAR"}
                });
                uint32_t meshIdx = static_cast<uint32_t>(meshes.size());
                meshes.push_back({
                    {"primitives", nlohmann::json::array({nlohmann::json{
                        {"attributes", {{"POSITION", 0}, {"NORMAL", 1}}},
                        {"indices", accIdx}, {"mode", 4}
                    }})}
                });
                std::string nodeName = "tile_" + std::to_string(tm.tx) +
                                       "_" + std::to_string(tm.ty);
                uint32_t nodeIdx = static_cast<uint32_t>(nodes.size());
                nodes.push_back({{"name", nodeName}, {"mesh", meshIdx}});
                sceneNodes.push_back(nodeIdx);
            }
            gj["accessors"] = accessors;
            gj["meshes"] = meshes;
            gj["nodes"] = nodes;
            gj["scenes"] = nlohmann::json::array({nlohmann::json{
                {"nodes", sceneNodes}
            }});
            std::string jsonStr = gj.dump();
            while (jsonStr.size() % 4 != 0) jsonStr += ' ';
            uint32_t jsonLen = static_cast<uint32_t>(jsonStr.size());
            uint32_t binLen = binSize;
            uint32_t totalLen = 12 + 8 + jsonLen + 8 + binLen;
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "Failed to open output: %s\n", outPath.c_str());
                return 1;
            }
            uint32_t magic = 0x46546C67, version = 2;
            out.write(reinterpret_cast<const char*>(&magic), 4);
            out.write(reinterpret_cast<const char*>(&version), 4);
            out.write(reinterpret_cast<const char*>(&totalLen), 4);
            uint32_t jsonChunkType = 0x4E4F534A;
            out.write(reinterpret_cast<const char*>(&jsonLen), 4);
            out.write(reinterpret_cast<const char*>(&jsonChunkType), 4);
            out.write(jsonStr.data(), jsonLen);
            uint32_t binChunkType = 0x004E4942;
            out.write(reinterpret_cast<const char*>(&binLen), 4);
            out.write(reinterpret_cast<const char*>(&binChunkType), 4);
            out.write(reinterpret_cast<const char*>(bin.data()), binLen);
            out.close();
            std::printf("Baked %s -> %s\n", zoneDir.c_str(), outPath.c_str());
            std::printf("  %d tile(s), %u verts, %u tris, %zu meshes, %u-byte BIN\n",
                        loadedTiles, totalV, totalI / 3,
                        meshes.size(), binLen);
            return 0;
        } else if (std::strcmp(argv[i], "--bake-zone-stl") == 0 && i + 1 < argc) {
            // STL counterpart to --bake-zone-glb. Designers can 3D-print a
            // miniature of an entire multi-tile zone in one slicer load —
            // useful for tabletop RPG props or a physical reference of a
            // playtest area.
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "bake-zone-stl: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr,
                    "bake-zone-stl: failed to parse zone.json\n");
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/" + zm.mapName + ".stl";
            if (zm.tiles.empty()) {
                std::fprintf(stderr, "bake-zone-stl: zone has no tiles\n");
                return 1;
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr, "bake-zone-stl: cannot write %s\n", outPath.c_str());
                return 1;
            }
            constexpr float kTileSize = 533.33333f;
            constexpr float kChunkSize = kTileSize / 16.0f;
            constexpr float kVertSpacing = kChunkSize / 8.0f;
            // Solid name sanitized to alphanum + underscore.
            std::string solidName = zm.mapName;
            for (auto& c : solidName) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_')) c = '_';
            }
            if (solidName.empty()) solidName = "wowee_zone";
            out << "solid " << solidName << "\n";
            int loadedTiles = 0, holesSkipped = 0;
            uint64_t triCount = 0;
            // For each tile, generate the same 9x9 outer-grid mesh and
            // emit per-triangle facets directly (STL has no shared
            // vertex pool — each triangle stands alone). Compute face
            // normal from cross product (slicers use it for orientation).
            for (const auto& [tx, ty] : zm.tiles) {
                std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                       std::to_string(tx) + "_" + std::to_string(ty);
                if (!wowee::pipeline::WoweeTerrainLoader::exists(tileBase)) {
                    std::fprintf(stderr,
                        "bake-zone-stl: tile (%d, %d) WHM/WOT missing — skipping\n",
                        tx, ty);
                    continue;
                }
                wowee::pipeline::ADTTerrain terrain;
                wowee::pipeline::WoweeTerrainLoader::load(tileBase, terrain);
                loadedTiles++;
                for (int cx = 0; cx < 16; ++cx) {
                    for (int cy = 0; cy < 16; ++cy) {
                        const auto& chunk = terrain.getChunk(cx, cy);
                        if (!chunk.heightMap.isLoaded()) continue;
                        float chunkBaseX = (32.0f - terrain.coord.y) * kTileSize - cy * kChunkSize;
                        float chunkBaseY = (32.0f - terrain.coord.x) * kTileSize - cx * kChunkSize;
                        // Pre-compute the 9x9 vertex grid for this chunk.
                        glm::vec3 V[9][9];
                        for (int row = 0; row < 9; ++row) {
                            for (int col = 0; col < 9; ++col) {
                                V[row][col] = {
                                    chunkBaseX - row * kVertSpacing,
                                    chunkBaseY - col * kVertSpacing,
                                    chunk.position[2] +
                                        chunk.heightMap.heights[row * 17 + col]
                                };
                            }
                        }
                        bool isHoleChunk = (chunk.holes != 0);
                        auto emitTri = [&](const glm::vec3& a,
                                           const glm::vec3& b,
                                           const glm::vec3& c) {
                            glm::vec3 e1 = b - a, e2 = c - a;
                            glm::vec3 n = glm::cross(e1, e2);
                            float len = glm::length(n);
                            if (len > 1e-12f) n /= len;
                            else n = {0, 0, 1};
                            out << "  facet normal " << n.x << " " << n.y << " " << n.z << "\n"
                                << "    outer loop\n"
                                << "      vertex " << a.x << " " << a.y << " " << a.z << "\n"
                                << "      vertex " << b.x << " " << b.y << " " << b.z << "\n"
                                << "      vertex " << c.x << " " << c.y << " " << c.z << "\n"
                                << "    endloop\n"
                                << "  endfacet\n";
                            triCount++;
                        };
                        for (int row = 0; row < 8; ++row) {
                            for (int col = 0; col < 8; ++col) {
                                if (isHoleChunk) {
                                    int hx = col / 2, hy = row / 2;
                                    if (chunk.holes & (1 << (hy * 4 + hx))) {
                                        holesSkipped++;
                                        continue;
                                    }
                                }
                                emitTri(V[row][col], V[row][col + 1], V[row + 1][col + 1]);
                                emitTri(V[row][col], V[row + 1][col + 1], V[row + 1][col]);
                            }
                        }
                    }
                }
            }
            out << "endsolid " << solidName << "\n";
            out.close();
            if (loadedTiles == 0) {
                std::fprintf(stderr, "bake-zone-stl: no tiles loaded\n");
                std::filesystem::remove(outPath);
                return 1;
            }
            std::printf("Baked %s -> %s\n", zoneDir.c_str(), outPath.c_str());
            std::printf("  %d tile(s), %llu facets, %d hole quads skipped\n",
                        loadedTiles, static_cast<unsigned long long>(triCount),
                        holesSkipped);
            return 0;
        } else if (std::strcmp(argv[i], "--bake-zone-obj") == 0 && i + 1 < argc) {
            // OBJ companion to --bake-zone-glb / --bake-zone-stl. Same
            // multi-tile WHM aggregation, but as Wavefront OBJ — opens
            // directly in Blender / MeshLab / 3DS Max for hand-editing.
            // Each tile becomes its own 'g' block so designers can hide
            // tiles independently.
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "bake-zone-obj: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "bake-zone-obj: parse failed\n");
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/" + zm.mapName + ".obj";
            if (zm.tiles.empty()) {
                std::fprintf(stderr, "bake-zone-obj: zone has no tiles\n");
                return 1;
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr, "bake-zone-obj: cannot write %s\n", outPath.c_str());
                return 1;
            }
            constexpr float kTileSize = 533.33333f;
            constexpr float kChunkSize = kTileSize / 16.0f;
            constexpr float kVertSpacing = kChunkSize / 8.0f;
            out << "# Wavefront OBJ generated by wowee_editor --bake-zone-obj\n";
            out << "# Zone: " << zm.mapName << " (" << zm.tiles.size()
                << " tiles)\n";
            out << "o " << zm.mapName << "\n";
            // OBJ uses a single global vertex pool with per-tile g-blocks
            // and per-tile face index offsetting. We accumulate per-tile
            // vertex blocks first (so face indices know their offsets),
            // then per-tile face blocks at the end.
            // Layout: emit ALL verts first (organized by tile, in order),
            // then emit ALL face blocks. OBJ requires verts before faces
            // that reference them.
            int loadedTiles = 0;
            int totalVerts = 0;
            // Per-tile bookkeeping: vertex base index (1-based for OBJ)
            // and which faces reference it.
            struct TileMeta {
                int tx, ty;
                uint32_t vertBase;  // 1-based OBJ index of first vert
                uint32_t vertCount;
                std::vector<uint32_t> faceI0, faceI1, faceI2;  // local indices
            };
            std::vector<TileMeta> tiles;
            for (const auto& [tx, ty] : zm.tiles) {
                std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                       std::to_string(tx) + "_" + std::to_string(ty);
                if (!wowee::pipeline::WoweeTerrainLoader::exists(tileBase)) {
                    std::fprintf(stderr,
                        "bake-zone-obj: tile (%d, %d) WHM/WOT missing — skipping\n",
                        tx, ty);
                    continue;
                }
                wowee::pipeline::ADTTerrain terrain;
                wowee::pipeline::WoweeTerrainLoader::load(tileBase, terrain);
                TileMeta tm{tx, ty, static_cast<uint32_t>(totalVerts + 1), 0, {}, {}, {}};
                // Walk chunks; emit verts to file as we go (so we don't
                // hold a giant vector in memory). Track local indices for
                // face emission afterwards.
                uint32_t tileLocalIdx = 0;
                for (int cx = 0; cx < 16; ++cx) {
                    for (int cy = 0; cy < 16; ++cy) {
                        const auto& chunk = terrain.getChunk(cx, cy);
                        if (!chunk.heightMap.isLoaded()) continue;
                        float chunkBaseX = (32.0f - terrain.coord.y) * kTileSize - cy * kChunkSize;
                        float chunkBaseY = (32.0f - terrain.coord.x) * kTileSize - cx * kChunkSize;
                        uint32_t chunkBaseLocal = tileLocalIdx;
                        for (int row = 0; row < 9; ++row) {
                            for (int col = 0; col < 9; ++col) {
                                float x = chunkBaseX - row * kVertSpacing;
                                float y = chunkBaseY - col * kVertSpacing;
                                float z = chunk.position[2] +
                                          chunk.heightMap.heights[row * 17 + col];
                                out << "v " << x << " " << y << " " << z << "\n";
                                tileLocalIdx++;
                            }
                        }
                        bool isHoleChunk = (chunk.holes != 0);
                        for (int row = 0; row < 8; ++row) {
                            for (int col = 0; col < 8; ++col) {
                                if (isHoleChunk) {
                                    int hx = col / 2, hy = row / 2;
                                    if (chunk.holes & (1 << (hy * 4 + hx))) continue;
                                }
                                auto idx = [&](int r, int c) {
                                    return chunkBaseLocal + r * 9 + c;
                                };
                                tm.faceI0.push_back(idx(row, col));
                                tm.faceI1.push_back(idx(row, col + 1));
                                tm.faceI2.push_back(idx(row + 1, col + 1));
                                tm.faceI0.push_back(idx(row, col));
                                tm.faceI1.push_back(idx(row + 1, col + 1));
                                tm.faceI2.push_back(idx(row + 1, col));
                            }
                        }
                    }
                }
                tm.vertCount = tileLocalIdx;
                totalVerts += tm.vertCount;
                if (tm.vertCount > 0) {
                    tiles.push_back(std::move(tm));
                    loadedTiles++;
                }
            }
            // Now emit per-tile face groups (after all verts are written).
            uint64_t totalFaces = 0;
            for (const auto& tm : tiles) {
                out << "g tile_" << tm.tx << "_" << tm.ty << "\n";
                for (size_t k = 0; k < tm.faceI0.size(); ++k) {
                    uint32_t a = tm.faceI0[k] + tm.vertBase;
                    uint32_t b = tm.faceI1[k] + tm.vertBase;
                    uint32_t c = tm.faceI2[k] + tm.vertBase;
                    out << "f " << a << " " << b << " " << c << "\n";
                    totalFaces++;
                }
            }
            out.close();
            if (loadedTiles == 0) {
                std::fprintf(stderr, "bake-zone-obj: no tiles loaded\n");
                std::filesystem::remove(outPath);
                return 1;
            }
            std::printf("Baked %s -> %s\n", zoneDir.c_str(), outPath.c_str());
            std::printf("  %d tile(s), %d verts, %llu tris\n",
                        loadedTiles, totalVerts,
                        static_cast<unsigned long long>(totalFaces));
            return 0;
        } else if (std::strcmp(argv[i], "--bake-project-obj") == 0 && i + 1 < argc) {
            // Project-level OBJ bake: every zone in <projectDir> gets
            // emitted into one giant OBJ with one 'g zone_NAME' block
            // per zone. Useful for previewing an entire project's terrain
            // in MeshLab/Blender at once, or for printing the whole map.
            std::string projectDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "bake-project-obj: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = projectDir + "/project.obj";
            std::vector<std::string> zoneDirs;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zoneDirs.push_back(entry.path().string());
            }
            std::sort(zoneDirs.begin(), zoneDirs.end());
            if (zoneDirs.empty()) {
                std::fprintf(stderr,
                    "bake-project-obj: no zones found in %s\n",
                    projectDir.c_str());
                return 1;
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "bake-project-obj: cannot write %s\n", outPath.c_str());
                return 1;
            }
            constexpr float kTileSize = 533.33333f;
            constexpr float kChunkSize = kTileSize / 16.0f;
            constexpr float kVertSpacing = kChunkSize / 8.0f;
            out << "# Wavefront OBJ generated by wowee_editor --bake-project-obj\n";
            out << "# Project: " << projectDir << " (" << zoneDirs.size() << " zones)\n";
            // Single global vertex pool. Per-zone we accumulate verts then
            // emit faces; same shape as --bake-zone-obj.
            int totalZones = 0, totalTiles = 0;
            int totalVerts = 0;
            uint64_t totalFaces = 0;
            struct Pending {
                std::string zoneName;
                uint32_t vertBase;  // 1-based OBJ index
                std::vector<uint32_t> faceI0, faceI1, faceI2;
            };
            std::vector<Pending> queues;
            for (const auto& zoneDir : zoneDirs) {
                wowee::editor::ZoneManifest zm;
                if (!zm.load(zoneDir + "/zone.json")) continue;
                Pending pq;
                pq.zoneName = zm.mapName;
                pq.vertBase = static_cast<uint32_t>(totalVerts + 1);
                int zoneTiles = 0;
                uint32_t zoneLocalIdx = 0;
                for (const auto& [tx, ty] : zm.tiles) {
                    std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                            std::to_string(tx) + "_" +
                                            std::to_string(ty);
                    if (!wowee::pipeline::WoweeTerrainLoader::exists(tileBase)) continue;
                    wowee::pipeline::ADTTerrain terrain;
                    wowee::pipeline::WoweeTerrainLoader::load(tileBase, terrain);
                    zoneTiles++;
                    for (int cx = 0; cx < 16; ++cx) {
                        for (int cy = 0; cy < 16; ++cy) {
                            const auto& chunk = terrain.getChunk(cx, cy);
                            if (!chunk.heightMap.isLoaded()) continue;
                            float chunkBaseX = (32.0f - terrain.coord.y) * kTileSize - cy * kChunkSize;
                            float chunkBaseY = (32.0f - terrain.coord.x) * kTileSize - cx * kChunkSize;
                            uint32_t chunkBaseLocal = zoneLocalIdx;
                            for (int row = 0; row < 9; ++row) {
                                for (int col = 0; col < 9; ++col) {
                                    float x = chunkBaseX - row * kVertSpacing;
                                    float y = chunkBaseY - col * kVertSpacing;
                                    float z = chunk.position[2] +
                                              chunk.heightMap.heights[row * 17 + col];
                                    out << "v " << x << " " << y << " " << z << "\n";
                                    zoneLocalIdx++;
                                }
                            }
                            bool isHoleChunk = (chunk.holes != 0);
                            for (int row = 0; row < 8; ++row) {
                                for (int col = 0; col < 8; ++col) {
                                    if (isHoleChunk) {
                                        int hx = col / 2, hy = row / 2;
                                        if (chunk.holes & (1 << (hy * 4 + hx))) continue;
                                    }
                                    auto idx = [&](int r, int c) {
                                        return chunkBaseLocal + r * 9 + c;
                                    };
                                    pq.faceI0.push_back(idx(row, col));
                                    pq.faceI1.push_back(idx(row, col + 1));
                                    pq.faceI2.push_back(idx(row + 1, col + 1));
                                    pq.faceI0.push_back(idx(row, col));
                                    pq.faceI1.push_back(idx(row + 1, col + 1));
                                    pq.faceI2.push_back(idx(row + 1, col));
                                }
                            }
                        }
                    }
                }
                if (zoneLocalIdx == 0) continue;
                totalVerts += zoneLocalIdx;
                totalTiles += zoneTiles;
                totalZones++;
                queues.push_back(std::move(pq));
            }
            // After all verts written, emit faces grouped by zone.
            for (const auto& pq : queues) {
                out << "g zone_" << pq.zoneName << "\n";
                for (size_t k = 0; k < pq.faceI0.size(); ++k) {
                    out << "f " << (pq.faceI0[k] + pq.vertBase) << " "
                        << (pq.faceI1[k] + pq.vertBase) << " "
                        << (pq.faceI2[k] + pq.vertBase) << "\n";
                    totalFaces++;
                }
            }
            out.close();
            std::printf("Baked %s -> %s\n", projectDir.c_str(), outPath.c_str());
            std::printf("  %d zone(s), %d tiles, %d verts, %llu tris\n",
                        totalZones, totalTiles, totalVerts,
                        static_cast<unsigned long long>(totalFaces));
            return 0;
        } else if ((std::strcmp(argv[i], "--bake-project-stl") == 0 ||
                    std::strcmp(argv[i], "--bake-project-glb") == 0) &&
                   i + 1 < argc) {
            // STL + glTF project bakes share the per-zone walking logic
            // with --bake-project-obj. Only the output emission differs:
            //   STL → per-triangle 'facet normal'+'outer loop'+vertex×3
            //   GLB → packed BIN chunk + JSON describing per-zone meshes
            // Coords match across all three exporters so an .obj/.stl/
            // .glb of the same source line up spatially when overlaid.
            bool isStl = (std::strcmp(argv[i], "--bake-project-stl") == 0);
            const char* cmdName = isStl ? "bake-project-stl" : "bake-project-glb";
            std::string projectDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "%s: %s is not a directory\n", cmdName, projectDir.c_str());
                return 1;
            }
            if (outPath.empty()) {
                outPath = projectDir + "/project." + (isStl ? "stl" : "glb");
            }
            std::vector<std::string> zoneDirs;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zoneDirs.push_back(entry.path().string());
            }
            std::sort(zoneDirs.begin(), zoneDirs.end());
            if (zoneDirs.empty()) {
                std::fprintf(stderr, "%s: no zones found\n", cmdName);
                return 1;
            }
            constexpr float kTileSize = 533.33333f;
            constexpr float kChunkSize = kTileSize / 16.0f;
            constexpr float kVertSpacing = kChunkSize / 8.0f;
            // Common pass: collect per-zone vertex+index pools. STL emits
            // per-triangle facets directly; GLB packs everything into BIN.
            struct ZonePool {
                std::string name;
                std::vector<glm::vec3> verts;
                std::vector<uint32_t> indices;
            };
            std::vector<ZonePool> zones;
            int totalZones = 0, totalTiles = 0;
            glm::vec3 bMin{1e30f}, bMax{-1e30f};
            for (const auto& zoneDir : zoneDirs) {
                wowee::editor::ZoneManifest zm;
                if (!zm.load(zoneDir + "/zone.json")) continue;
                ZonePool zp;
                zp.name = zm.mapName;
                int zoneTiles = 0;
                for (const auto& [tx, ty] : zm.tiles) {
                    std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                            std::to_string(tx) + "_" + std::to_string(ty);
                    if (!wowee::pipeline::WoweeTerrainLoader::exists(tileBase)) continue;
                    wowee::pipeline::ADTTerrain terrain;
                    wowee::pipeline::WoweeTerrainLoader::load(tileBase, terrain);
                    zoneTiles++;
                    for (int cx = 0; cx < 16; ++cx) {
                        for (int cy = 0; cy < 16; ++cy) {
                            const auto& chunk = terrain.getChunk(cx, cy);
                            if (!chunk.heightMap.isLoaded()) continue;
                            float chunkBaseX = (32.0f - terrain.coord.y) * kTileSize - cy * kChunkSize;
                            float chunkBaseY = (32.0f - terrain.coord.x) * kTileSize - cx * kChunkSize;
                            uint32_t chunkBase = static_cast<uint32_t>(zp.verts.size());
                            for (int row = 0; row < 9; ++row) {
                                for (int col = 0; col < 9; ++col) {
                                    glm::vec3 p{
                                        chunkBaseX - row * kVertSpacing,
                                        chunkBaseY - col * kVertSpacing,
                                        chunk.position[2] +
                                            chunk.heightMap.heights[row * 17 + col]
                                    };
                                    zp.verts.push_back(p);
                                    bMin = glm::min(bMin, p);
                                    bMax = glm::max(bMax, p);
                                }
                            }
                            bool isHoleChunk = (chunk.holes != 0);
                            for (int row = 0; row < 8; ++row) {
                                for (int col = 0; col < 8; ++col) {
                                    if (isHoleChunk) {
                                        int hx = col / 2, hy = row / 2;
                                        if (chunk.holes & (1 << (hy * 4 + hx))) continue;
                                    }
                                    auto idx = [&](int r, int c) {
                                        return chunkBase + r * 9 + c;
                                    };
                                    zp.indices.push_back(idx(row, col));
                                    zp.indices.push_back(idx(row, col + 1));
                                    zp.indices.push_back(idx(row + 1, col + 1));
                                    zp.indices.push_back(idx(row, col));
                                    zp.indices.push_back(idx(row + 1, col + 1));
                                    zp.indices.push_back(idx(row + 1, col));
                                }
                            }
                        }
                    }
                }
                if (zp.verts.empty()) continue;
                totalTiles += zoneTiles;
                totalZones++;
                zones.push_back(std::move(zp));
            }
            if (zones.empty()) {
                std::fprintf(stderr, "%s: no loadable terrain found\n", cmdName);
                return 1;
            }
            if (isStl) {
                std::ofstream out(outPath);
                if (!out) {
                    std::fprintf(stderr, "%s: cannot write %s\n", cmdName, outPath.c_str());
                    return 1;
                }
                out << "solid wowee_project\n";
                uint64_t triCount = 0;
                for (const auto& zp : zones) {
                    for (size_t k = 0; k + 2 < zp.indices.size(); k += 3) {
                        const auto& v0 = zp.verts[zp.indices[k]];
                        const auto& v1 = zp.verts[zp.indices[k + 1]];
                        const auto& v2 = zp.verts[zp.indices[k + 2]];
                        glm::vec3 n = glm::cross(v1 - v0, v2 - v0);
                        float len = glm::length(n);
                        if (len > 1e-12f) n /= len; else n = {0, 0, 1};
                        out << "  facet normal " << n.x << " " << n.y << " " << n.z << "\n"
                            << "    outer loop\n"
                            << "      vertex " << v0.x << " " << v0.y << " " << v0.z << "\n"
                            << "      vertex " << v1.x << " " << v1.y << " " << v1.z << "\n"
                            << "      vertex " << v2.x << " " << v2.y << " " << v2.z << "\n"
                            << "    endloop\n"
                            << "  endfacet\n";
                        triCount++;
                    }
                }
                out << "endsolid wowee_project\n";
                out.close();
                std::printf("Baked %s -> %s\n", projectDir.c_str(), outPath.c_str());
                std::printf("  %d zone(s), %d tiles, %llu facets\n",
                            totalZones, totalTiles,
                            static_cast<unsigned long long>(triCount));
                return 0;
            }
            // GLB path: pack positions+normals+indices into one BIN chunk,
            // one mesh+node per zone with sliced index accessor.
            uint32_t totalV = 0, totalI = 0;
            for (const auto& zp : zones) {
                totalV += static_cast<uint32_t>(zp.verts.size());
                totalI += static_cast<uint32_t>(zp.indices.size());
            }
            const uint32_t posOff = 0;
            const uint32_t nrmOff = posOff + totalV * 12;
            const uint32_t idxOff = nrmOff + totalV * 12;
            const uint32_t binSize = idxOff + totalI * 4;
            std::vector<uint8_t> bin(binSize);
            uint32_t vCursor = 0, iCursor = 0;
            // Per-zone bookkeeping for accessor slicing.
            struct ZoneSlice { std::string name; uint32_t vOff, vCnt, iOff, iCnt; };
            std::vector<ZoneSlice> slices;
            for (const auto& zp : zones) {
                ZoneSlice s{zp.name, vCursor, static_cast<uint32_t>(zp.verts.size()),
                             iCursor, static_cast<uint32_t>(zp.indices.size())};
                for (const auto& v : zp.verts) {
                    std::memcpy(&bin[posOff + vCursor * 12 + 0], &v.x, 4);
                    std::memcpy(&bin[posOff + vCursor * 12 + 4], &v.y, 4);
                    std::memcpy(&bin[posOff + vCursor * 12 + 8], &v.z, 4);
                    float nx = 0, ny = 0, nz = 1;
                    std::memcpy(&bin[nrmOff + vCursor * 12 + 0], &nx, 4);
                    std::memcpy(&bin[nrmOff + vCursor * 12 + 4], &ny, 4);
                    std::memcpy(&bin[nrmOff + vCursor * 12 + 8], &nz, 4);
                    vCursor++;
                }
                // Offset zone indices by the global vertBase so they
                // resolve into the merged pool.
                for (uint32_t idx : zp.indices) {
                    uint32_t global = idx + s.vOff;
                    std::memcpy(&bin[idxOff + iCursor * 4], &global, 4);
                    iCursor++;
                }
                slices.push_back(s);
            }
            nlohmann::json gj;
            gj["asset"] = {{"version", "2.0"},
                            {"generator", "wowee_editor --bake-project-glb"}};
            gj["scene"] = 0;
            gj["buffers"] = nlohmann::json::array({{{"byteLength", binSize}}});
            nlohmann::json bvs = nlohmann::json::array();
            bvs.push_back({{"buffer", 0}, {"byteOffset", posOff},
                            {"byteLength", totalV * 12}, {"target", 34962}});
            bvs.push_back({{"buffer", 0}, {"byteOffset", nrmOff},
                            {"byteLength", totalV * 12}, {"target", 34962}});
            bvs.push_back({{"buffer", 0}, {"byteOffset", idxOff},
                            {"byteLength", totalI * 4},  {"target", 34963}});
            gj["bufferViews"] = bvs;
            nlohmann::json accessors = nlohmann::json::array();
            accessors.push_back({{"bufferView", 0}, {"componentType", 5126},
                                  {"count", totalV}, {"type", "VEC3"},
                                  {"min", {bMin.x, bMin.y, bMin.z}},
                                  {"max", {bMax.x, bMax.y, bMax.z}}});
            accessors.push_back({{"bufferView", 1}, {"componentType", 5126},
                                  {"count", totalV}, {"type", "VEC3"}});
            nlohmann::json meshes = nlohmann::json::array();
            nlohmann::json nodes = nlohmann::json::array();
            nlohmann::json sceneNodes = nlohmann::json::array();
            for (const auto& s : slices) {
                uint32_t accIdx = static_cast<uint32_t>(accessors.size());
                accessors.push_back({{"bufferView", 2},
                                      {"byteOffset", s.iOff * 4},
                                      {"componentType", 5125},
                                      {"count", s.iCnt}, {"type", "SCALAR"}});
                uint32_t meshIdx = static_cast<uint32_t>(meshes.size());
                meshes.push_back({{"primitives", nlohmann::json::array({nlohmann::json{
                    {"attributes", {{"POSITION", 0}, {"NORMAL", 1}}},
                    {"indices", accIdx}, {"mode", 4}}})}});
                uint32_t nodeIdx = static_cast<uint32_t>(nodes.size());
                nodes.push_back({{"name", "zone_" + s.name}, {"mesh", meshIdx}});
                sceneNodes.push_back(nodeIdx);
            }
            gj["accessors"] = accessors;
            gj["meshes"] = meshes;
            gj["nodes"] = nodes;
            gj["scenes"] = nlohmann::json::array({{{"nodes", sceneNodes}}});
            std::string jsonStr = gj.dump();
            while (jsonStr.size() % 4 != 0) jsonStr += ' ';
            uint32_t jsonLen = static_cast<uint32_t>(jsonStr.size());
            uint32_t binLen = binSize;
            uint32_t totalLen = 12 + 8 + jsonLen + 8 + binLen;
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "%s: cannot write %s\n", cmdName, outPath.c_str());
                return 1;
            }
            uint32_t magic = 0x46546C67, version = 2;
            out.write(reinterpret_cast<const char*>(&magic), 4);
            out.write(reinterpret_cast<const char*>(&version), 4);
            out.write(reinterpret_cast<const char*>(&totalLen), 4);
            uint32_t jt = 0x4E4F534A;
            out.write(reinterpret_cast<const char*>(&jsonLen), 4);
            out.write(reinterpret_cast<const char*>(&jt), 4);
            out.write(jsonStr.data(), jsonLen);
            uint32_t bt = 0x004E4942;
            out.write(reinterpret_cast<const char*>(&binLen), 4);
            out.write(reinterpret_cast<const char*>(&bt), 4);
            out.write(reinterpret_cast<const char*>(bin.data()), binLen);
            out.close();
            std::printf("Baked %s -> %s\n", projectDir.c_str(), outPath.c_str());
            std::printf("  %d zone(s), %d tiles, %u verts, %u tris, %u-byte BIN\n",
                        totalZones, totalTiles, totalV, totalI / 3, binLen);
            return 0;
        } else if (std::strcmp(argv[i], "--export-wob-obj") == 0 && i + 1 < argc) {
            // WOB is the WMO replacement; like --export-obj for WOM, this
            // bridges WOB into the universal-3D-tool ecosystem. Each WOB
            // group becomes one OBJ 'g' block, preserving the room/floor
            // structure for downstream selection in Blender/MeshLab.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wob")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeBuildingLoader::exists(base)) {
                std::fprintf(stderr, "WOB not found: %s.wob\n", base.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = base + ".obj";
            auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
            if (!bld.isValid()) {
                std::fprintf(stderr, "WOB has no groups to export: %s.wob\n", base.c_str());
                return 1;
            }
            std::ofstream obj(outPath);
            if (!obj) {
                std::fprintf(stderr, "Failed to open output file: %s\n", outPath.c_str());
                return 1;
            }
            // Total verts/tris across all groups for the header.
            size_t totalV = 0, totalI = 0;
            for (const auto& g : bld.groups) {
                totalV += g.vertices.size();
                totalI += g.indices.size();
            }
            obj << "# Wavefront OBJ generated by wowee_editor --export-wob-obj\n";
            obj << "# Source: " << base << ".wob\n";
            obj << "# Groups: " << bld.groups.size()
                << " Verts: " << totalV
                << " Tris: " << totalI / 3
                << " Portals: " << bld.portals.size()
                << " Doodads: " << bld.doodads.size() << "\n\n";
            obj << "o " << (bld.name.empty() ? "WoweeBuilding" : bld.name) << "\n";
            // OBJ uses a single global vertex pool, so we offset each group's
            // local indices by the running total of verts written so far.
            uint32_t vertOffset = 0;
            for (size_t g = 0; g < bld.groups.size(); ++g) {
                const auto& grp = bld.groups[g];
                if (grp.vertices.empty()) continue;
                for (const auto& v : grp.vertices) {
                    obj << "v "  << v.position.x << " "
                                  << v.position.y << " "
                                  << v.position.z << "\n";
                }
                for (const auto& v : grp.vertices) {
                    obj << "vt " << v.texCoord.x << " "
                                  << (1.0f - v.texCoord.y) << "\n";
                }
                for (const auto& v : grp.vertices) {
                    obj << "vn " << v.normal.x << " "
                                  << v.normal.y << " "
                                  << v.normal.z << "\n";
                }
                std::string groupName = grp.name.empty()
                    ? "group_" + std::to_string(g)
                    : grp.name;
                if (grp.isOutdoor) groupName += "_outdoor";
                obj << "g " << groupName << "\n";
                for (size_t k = 0; k + 2 < grp.indices.size(); k += 3) {
                    uint32_t i0 = grp.indices[k]     + 1 + vertOffset;
                    uint32_t i1 = grp.indices[k + 1] + 1 + vertOffset;
                    uint32_t i2 = grp.indices[k + 2] + 1 + vertOffset;
                    obj << "f "
                        << i0 << "/" << i0 << "/" << i0 << " "
                        << i1 << "/" << i1 << "/" << i1 << " "
                        << i2 << "/" << i2 << "/" << i2 << "\n";
                }
                vertOffset += static_cast<uint32_t>(grp.vertices.size());
            }
            // Doodad placements as a separate informational block — emit
            // each as a comment line so OBJ stays valid but the data is
            // recoverable for tools that want to re-create the placements.
            if (!bld.doodads.empty()) {
                obj << "\n# Doodad placements (model, position, rotation, scale):\n";
                for (const auto& d : bld.doodads) {
                    obj << "# doodad " << d.modelPath
                        << " pos " << d.position.x << "," << d.position.y << "," << d.position.z
                        << " rot " << d.rotation.x << "," << d.rotation.y << "," << d.rotation.z
                        << " scale " << d.scale << "\n";
                }
            }
            obj.close();
            std::printf("Exported %s.wob -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  %zu groups, %zu verts, %zu tris, %zu doodad placements\n",
                        bld.groups.size(), totalV, totalI / 3,
                        bld.doodads.size());
            return 0;
        } else if (std::strcmp(argv[i], "--import-wob-obj") == 0 && i + 1 < argc) {
            // Round-trip companion to --export-wob-obj. Each OBJ 'g' block
            // becomes one WoweeBuilding::Group; geometry under that group
            // is deduped into the group's local vertex array. Faces
            // before any 'g' directive land in a default 'imported' group.
            // Doodad placements written as # comment lines by --export-wob-obj
            // ARE recognized and re-instanced into bld.doodads.
            std::string objPath = argv[++i];
            std::string wobBase;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                wobBase = argv[++i];
            }
            if (!std::filesystem::exists(objPath)) {
                std::fprintf(stderr, "OBJ not found: %s\n", objPath.c_str());
                return 1;
            }
            if (wobBase.empty()) {
                wobBase = objPath;
                if (wobBase.size() >= 4 &&
                    wobBase.substr(wobBase.size() - 4) == ".obj") {
                    wobBase = wobBase.substr(0, wobBase.size() - 4);
                }
            }
            std::ifstream in(objPath);
            if (!in) {
                std::fprintf(stderr, "Failed to open OBJ: %s\n", objPath.c_str());
                return 1;
            }
            // Global pools (OBJ vertex/uv/normal indices reference these
            // across all groups).
            std::vector<glm::vec3> positions;
            std::vector<glm::vec2> texcoords;
            std::vector<glm::vec3> normals;
            wowee::pipeline::WoweeBuilding bld;
            // Active group bookkeeping: dedupe table is per-group since
            // each WOB group has its own local vertex buffer.
            std::string activeGroup = "imported";
            std::unordered_map<std::string, uint32_t> groupDedupe;
            int activeGroupIdx = -1;
            int badFaces = 0;
            int triangulatedNgons = 0;
            std::string objectName;
            auto ensureActiveGroup = [&]() {
                if (activeGroupIdx >= 0) return;
                wowee::pipeline::WoweeBuilding::Group g;
                g.name = activeGroup;
                if (g.name.size() >= 8 &&
                    g.name.substr(g.name.size() - 8) == "_outdoor") {
                    g.name = g.name.substr(0, g.name.size() - 8);
                    g.isOutdoor = true;
                }
                bld.groups.push_back(g);
                activeGroupIdx = static_cast<int>(bld.groups.size()) - 1;
                groupDedupe.clear();
            };
            auto resolveCorner = [&](const std::string& token) -> int {
                int v = 0, t = 0, n = 0;
                {
                    const char* p = token.c_str();
                    char* endp = nullptr;
                    v = std::strtol(p, &endp, 10);
                    if (*endp == '/') {
                        ++endp;
                        if (*endp != '/') t = std::strtol(endp, &endp, 10);
                        if (*endp == '/') {
                            ++endp;
                            n = std::strtol(endp, &endp, 10);
                        }
                    }
                }
                auto absIdx = [](int idx, size_t pool) {
                    if (idx < 0) return static_cast<int>(pool) + idx;
                    return idx - 1;
                };
                int vi = absIdx(v, positions.size());
                int ti = (t == 0) ? -1 : absIdx(t, texcoords.size());
                int ni = (n == 0) ? -1 : absIdx(n, normals.size());
                if (vi < 0 || vi >= static_cast<int>(positions.size())) return -1;
                ensureActiveGroup();
                std::string key = std::to_string(vi) + "/" +
                                  std::to_string(ti) + "/" +
                                  std::to_string(ni);
                auto it = groupDedupe.find(key);
                if (it != groupDedupe.end()) return static_cast<int>(it->second);
                wowee::pipeline::WoweeBuilding::Vertex vert;
                vert.position = positions[vi];
                if (ti >= 0 && ti < static_cast<int>(texcoords.size())) {
                    vert.texCoord = texcoords[ti];
                    // Reverse the V-flip from --export-wob-obj.
                    vert.texCoord.y = 1.0f - vert.texCoord.y;
                } else {
                    vert.texCoord = {0, 0};
                }
                if (ni >= 0 && ni < static_cast<int>(normals.size())) {
                    vert.normal = normals[ni];
                } else {
                    vert.normal = {0, 0, 1};
                }
                vert.color = {1, 1, 1, 1};
                auto& grp = bld.groups[activeGroupIdx];
                uint32_t newIdx = static_cast<uint32_t>(grp.vertices.size());
                grp.vertices.push_back(vert);
                groupDedupe[key] = newIdx;
                return static_cast<int>(newIdx);
            };
            std::string line;
            while (std::getline(in, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.empty()) continue;
                // Recognize doodad placement comment lines emitted by
                // --export-wob-obj so the round-trip preserves them.
                if (line[0] == '#') {
                    if (line.find("# doodad ") == 0) {
                        std::istringstream ss(line);
                        std::string hash, doodadKw, modelPath, posKw, posStr,
                                    rotKw, rotStr, scaleKw;
                        float scale = 1.0f;
                        ss >> hash >> doodadKw >> modelPath
                           >> posKw >> posStr
                           >> rotKw >> rotStr
                           >> scaleKw >> scale;
                        auto parse3 = [](const std::string& s, glm::vec3& out) {
                            int got = std::sscanf(s.c_str(), "%f,%f,%f",
                                                  &out.x, &out.y, &out.z);
                            return got == 3;
                        };
                        wowee::pipeline::WoweeBuilding::DoodadPlacement d;
                        d.modelPath = modelPath;
                        if (parse3(posStr, d.position) &&
                            parse3(rotStr, d.rotation) &&
                            std::isfinite(scale) && scale > 0.0f) {
                            d.scale = scale;
                            bld.doodads.push_back(d);
                        }
                    }
                    continue;
                }
                std::istringstream ss(line);
                std::string tag;
                ss >> tag;
                if (tag == "v") {
                    glm::vec3 p; ss >> p.x >> p.y >> p.z;
                    positions.push_back(p);
                } else if (tag == "vt") {
                    glm::vec2 t; ss >> t.x >> t.y;
                    texcoords.push_back(t);
                } else if (tag == "vn") {
                    glm::vec3 n; ss >> n.x >> n.y >> n.z;
                    normals.push_back(n);
                } else if (tag == "o") {
                    if (objectName.empty()) ss >> objectName;
                } else if (tag == "g") {
                    // New group — flush dedupe table so the next batch of
                    // verts is local to this group.
                    std::string name;
                    ss >> name;
                    activeGroup = name.empty() ? "group" : name;
                    activeGroupIdx = -1;
                    groupDedupe.clear();
                } else if (tag == "f") {
                    std::vector<std::string> corners;
                    std::string c;
                    while (ss >> c) corners.push_back(c);
                    if (corners.size() < 3) { badFaces++; continue; }
                    std::vector<int> resolved;
                    resolved.reserve(corners.size());
                    bool ok = true;
                    for (const auto& cc : corners) {
                        int idx = resolveCorner(cc);
                        if (idx < 0) { ok = false; break; }
                        resolved.push_back(idx);
                    }
                    if (!ok) { badFaces++; continue; }
                    if (resolved.size() > 3) triangulatedNgons++;
                    auto& grp = bld.groups[activeGroupIdx];
                    for (size_t k = 1; k + 1 < resolved.size(); ++k) {
                        grp.indices.push_back(static_cast<uint32_t>(resolved[0]));
                        grp.indices.push_back(static_cast<uint32_t>(resolved[k]));
                        grp.indices.push_back(static_cast<uint32_t>(resolved[k + 1]));
                    }
                }
                // mtllib/usemtl/s lines silently skipped.
            }
            // Compute per-group bounds + global building bound.
            if (bld.groups.empty()) {
                std::fprintf(stderr, "import-wob-obj: no geometry found in %s\n",
                             objPath.c_str());
                return 1;
            }
            glm::vec3 bMin{1e30f}, bMax{-1e30f};
            for (auto& grp : bld.groups) {
                if (grp.vertices.empty()) continue;
                grp.boundMin = grp.vertices[0].position;
                grp.boundMax = grp.boundMin;
                for (const auto& v : grp.vertices) {
                    grp.boundMin = glm::min(grp.boundMin, v.position);
                    grp.boundMax = glm::max(grp.boundMax, v.position);
                }
                bMin = glm::min(bMin, grp.boundMin);
                bMax = glm::max(bMax, grp.boundMax);
            }
            glm::vec3 center = (bMin + bMax) * 0.5f;
            float r2 = 0;
            for (const auto& grp : bld.groups) {
                for (const auto& v : grp.vertices) {
                    glm::vec3 d = v.position - center;
                    r2 = std::max(r2, glm::dot(d, d));
                }
            }
            bld.boundRadius = std::sqrt(r2);
            bld.name = objectName.empty()
                ? std::filesystem::path(objPath).stem().string()
                : objectName;
            if (!wowee::pipeline::WoweeBuildingLoader::save(bld, wobBase)) {
                std::fprintf(stderr, "import-wob-obj: failed to write %s.wob\n",
                             wobBase.c_str());
                return 1;
            }
            size_t totalV = 0, totalI = 0;
            for (const auto& g : bld.groups) {
                totalV += g.vertices.size();
                totalI += g.indices.size();
            }
            std::printf("Imported %s -> %s.wob\n", objPath.c_str(), wobBase.c_str());
            std::printf("  %zu groups, %zu verts, %zu tris, %zu doodad placements\n",
                        bld.groups.size(), totalV, totalI / 3, bld.doodads.size());
            if (triangulatedNgons > 0) {
                std::printf("  fan-triangulated %d n-gon(s)\n", triangulatedNgons);
            }
            if (badFaces > 0) {
                std::printf("  warning: skipped %d malformed face(s)\n", badFaces);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--export-woc-obj") == 0 && i + 1 < argc) {
            // Visualize a WOC collision mesh in any 3D tool. Each
            // walkability class becomes its own OBJ group (walkable /
            // steep / water / indoor) so designers can hide categories
            // independently in Blender to debug 'why can the player
            // walk here?' or 'why can't they walk there?'.
            std::string path = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "WOC not found: %s\n", path.c_str());
                return 1;
            }
            if (outPath.empty()) {
                outPath = path;
                if (outPath.size() >= 4 &&
                    outPath.substr(outPath.size() - 4) == ".woc") {
                    outPath = outPath.substr(0, outPath.size() - 4);
                }
                outPath += ".obj";
            }
            auto woc = wowee::pipeline::WoweeCollisionBuilder::load(path);
            if (!woc.isValid()) {
                std::fprintf(stderr, "WOC has no triangles: %s\n", path.c_str());
                return 1;
            }
            std::ofstream obj(outPath);
            if (!obj) {
                std::fprintf(stderr, "Failed to open output file: %s\n", outPath.c_str());
                return 1;
            }
            // Bucket triangles by flag combination so the OBJ can split
            // them into named groups. Flag bits: walkable=0x01, water=0x02,
            // steep=0x04, indoor=0x08 (per WoweeCollision::Triangle).
            // Triangles can have multiple flags set so a per-flag group
            // would over-count; instead we bucket by exact flag value.
            std::unordered_map<uint8_t, std::vector<size_t>> byFlag;
            for (size_t t = 0; t < woc.triangles.size(); ++t) {
                byFlag[woc.triangles[t].flags].push_back(t);
            }
            obj << "# Wavefront OBJ generated by wowee_editor --export-woc-obj\n";
            obj << "# Source: " << path << "\n";
            obj << "# Triangles: " << woc.triangles.size()
                << " (walkable=" << woc.walkableCount()
                << " steep=" << woc.steepCount() << ")\n";
            obj << "# Tile: (" << woc.tileX << ", " << woc.tileY << ")\n\n";
            obj << "o WoweeCollision\n";
            // Emit ALL vertices first (3 per triangle, no dedupe — the
            // collision mesh has triangle-soup topology where shared
            // verts often have different flags, so deduping would
            // actually merge categories).
            for (const auto& tri : woc.triangles) {
                obj << "v " << tri.v0.x << " " << tri.v0.y << " " << tri.v0.z << "\n";
                obj << "v " << tri.v1.x << " " << tri.v1.y << " " << tri.v1.z << "\n";
                obj << "v " << tri.v2.x << " " << tri.v2.y << " " << tri.v2.z << "\n";
            }
            // Emit faces grouped by flag class. OBJ index of triangle t
            // vertex k is (t * 3 + k + 1) — 1-based, three verts per tri.
            auto flagName = [](uint8_t f) {
                if (f == 0) return std::string("nonwalkable");
                std::string s;
                if (f & 0x01) s += "walkable";
                if (f & 0x02) { if (!s.empty()) s += "_"; s += "water"; }
                if (f & 0x04) { if (!s.empty()) s += "_"; s += "steep"; }
                if (f & 0x08) { if (!s.empty()) s += "_"; s += "indoor"; }
                if (s.empty()) s = "flag" + std::to_string(int(f));
                return s;
            };
            for (const auto& [flag, tris] : byFlag) {
                obj << "g " << flagName(flag) << "\n";
                for (size_t t : tris) {
                    uint32_t base = static_cast<uint32_t>(t * 3 + 1);
                    obj << "f " << base << " " << (base + 1) << " " << (base + 2) << "\n";
                }
            }
            obj.close();
            std::printf("Exported %s -> %s\n", path.c_str(), outPath.c_str());
            std::printf("  %zu triangles in %zu flag class(es), tile (%u, %u)\n",
                        woc.triangles.size(), byFlag.size(), woc.tileX, woc.tileY);
            return 0;
        } else if (std::strcmp(argv[i], "--export-whm-obj") == 0 && i + 1 < argc) {
            // Convert a WHM/WOT terrain pair to OBJ for visualization in
            // Blender / MeshLab. Emits the 9x9 outer vertex grid per
            // chunk (skipping the 8x8 inner verts the engine uses for
            // 4-tri fans) — that's the canonical 'heightmap as mesh'
            // view, 256 chunks × 81 verts = 20736 verts, 32768 tris.
            // Geometry mirrors WoweeCollisionBuilder's outer-grid layout
            // exactly so the OBJ aligns with the corresponding WOC.
            std::string base = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            for (const char* ext : {".wot", ".whm"}) {
                if (base.size() >= 4 && base.substr(base.size() - 4) == ext) {
                    base = base.substr(0, base.size() - 4);
                    break;
                }
            }
            if (!wowee::pipeline::WoweeTerrainLoader::exists(base)) {
                std::fprintf(stderr, "WHM/WOT not found: %s.{whm,wot}\n", base.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = base + ".obj";
            wowee::pipeline::ADTTerrain terrain;
            wowee::pipeline::WoweeTerrainLoader::load(base, terrain);
            std::ofstream obj(outPath);
            if (!obj) {
                std::fprintf(stderr, "Failed to open output file: %s\n", outPath.c_str());
                return 1;
            }
            // Tile + chunk constants — must match WoweeCollisionBuilder so
            // exports of the same source align in space when overlaid.
            constexpr float kTileSize = 533.33333f;
            constexpr float kChunkSize = kTileSize / 16.0f;
            constexpr float kVertSpacing = kChunkSize / 8.0f;
            obj << "# Wavefront OBJ generated by wowee_editor --export-whm-obj\n";
            obj << "# Source: " << base << ".whm\n";
            obj << "# Tile coord: (" << terrain.coord.x << ", " << terrain.coord.y << ")\n";
            obj << "# Layout: 9x9 outer vertex grid per chunk, 8x8 quads -> 2 tris each\n\n";
            obj << "o WoweeTerrain_" << terrain.coord.x << "_" << terrain.coord.y << "\n";
            int loadedChunks = 0;
            uint32_t vertOffset = 0;
            for (int cx = 0; cx < 16; ++cx) {
                for (int cy = 0; cy < 16; ++cy) {
                    const auto& chunk = terrain.getChunk(cx, cy);
                    if (!chunk.heightMap.isLoaded()) continue;
                    loadedChunks++;
                    // Same XY origin formula as collision builder so
                    // overlaid OBJ exports line up exactly.
                    float chunkBaseX = (32.0f - terrain.coord.y) * kTileSize - cy * kChunkSize;
                    float chunkBaseY = (32.0f - terrain.coord.x) * kTileSize - cx * kChunkSize;
                    // Emit 9x9 outer verts. Layout: heights[row*17 + col]
                    // for col in [0,8] (the inner 8 verts at col 9..16
                    // are skipped — they're the quad-center verts).
                    for (int row = 0; row < 9; ++row) {
                        for (int col = 0; col < 9; ++col) {
                            float x = chunkBaseX - row * kVertSpacing;
                            float y = chunkBaseY - col * kVertSpacing;
                            float z = chunk.position[2] +
                                      chunk.heightMap.heights[row * 17 + col];
                            obj << "v " << x << " " << y << " " << z << "\n";
                        }
                    }
                    // Per-vertex UV: just the row/col in 0..1 — Blender
                    // can use this to slap a checker texture for scale.
                    for (int row = 0; row < 9; ++row) {
                        for (int col = 0; col < 9; ++col) {
                            obj << "vt " << (col / 8.0f) << " "
                                          << (row / 8.0f) << "\n";
                        }
                    }
                    // 8x8 quads — two tris each, respecting hole bits so
                    // cave-entrance quads correctly disappear from the mesh.
                    bool isHoleChunk = (chunk.holes != 0);
                    obj << "g chunk_" << cx << "_" << cy << "\n";
                    auto idx = [&](int r, int c) {
                        return vertOffset + r * 9 + c + 1;  // 1-based
                    };
                    for (int row = 0; row < 8; ++row) {
                        for (int col = 0; col < 8; ++col) {
                            if (isHoleChunk) {
                                int hx = col / 2, hy = row / 2;
                                if (chunk.holes & (1 << (hy * 4 + hx))) continue;
                            }
                            uint32_t i00 = idx(row, col);
                            uint32_t i10 = idx(row, col + 1);
                            uint32_t i01 = idx(row + 1, col);
                            uint32_t i11 = idx(row + 1, col + 1);
                            obj << "f " << i00 << "/" << i00 << " "
                                << i10 << "/" << i10 << " "
                                << i11 << "/" << i11 << "\n";
                            obj << "f " << i00 << "/" << i00 << " "
                                << i11 << "/" << i11 << " "
                                << i01 << "/" << i01 << "\n";
                        }
                    }
                    vertOffset += 81;  // 9x9 verts per chunk
                }
            }
            obj.close();
            // Estimated tri count: chunks × 128 (8x8 quads × 2 tris).
            // Holes reduce this but counting exactly would mean walking
            // the bitmask again — the rough estimate is the user-visible
            // useful number anyway.
            std::printf("Exported %s.whm -> %s\n", base.c_str(), outPath.c_str());
            std::printf("  %d chunks loaded, ~%d verts, ~%d tris\n",
                        loadedChunks, loadedChunks * 81, loadedChunks * 128);
            return 0;
        } else if (std::strcmp(argv[i], "--import-obj") == 0 && i + 1 < argc) {
            // Convert a Wavefront OBJ back into WOM. Round-trips with
            // --export-obj for the geometry/UV/normal data; bones,
            // animations, and material flags are not in OBJ and stay
            // empty (the resulting WOM is WOM1, static-only). The intent
            // is "edit a static prop in Blender, ship it".
            std::string objPath = argv[++i];
            std::string womBase;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                womBase = argv[++i];
            }
            if (!std::filesystem::exists(objPath)) {
                std::fprintf(stderr, "OBJ not found: %s\n", objPath.c_str());
                return 1;
            }
            if (womBase.empty()) {
                womBase = objPath;
                if (womBase.size() >= 4 &&
                    womBase.substr(womBase.size() - 4) == ".obj") {
                    womBase = womBase.substr(0, womBase.size() - 4);
                }
            }
            std::ifstream in(objPath);
            if (!in) {
                std::fprintf(stderr, "Failed to open OBJ: %s\n", objPath.c_str());
                return 1;
            }
            // Pools — OBJ stores positions/UVs/normals in independent
            // arrays and references them by index in face lines, so we
            // collect each pool first then expand into WOM vertices on
            // the fly (one WOM vertex per (vIdx, vtIdx, vnIdx) triple
            // since WOM has interleaved vertex data, not pooled).
            std::vector<glm::vec3> positions;
            std::vector<glm::vec2> texcoords;
            std::vector<glm::vec3> normals;
            wowee::pipeline::WoweeModel wom;
            wom.version = 1;
            std::unordered_map<std::string, uint32_t> dedupe;
            int badFaces = 0;
            int triangulatedNgons = 0;
            std::string objectName;
            std::string line;
            // Convert a single OBJ vertex token like "3/4/5" or "3//5" or
            // "3/4" or "3" into a WOM vertex index, deduping identical
            // (pos, uv, normal) triples to keep the buffer compact.
            auto resolveCorner = [&](const std::string& token) -> int {
                int v = 0, t = 0, n = 0;
                {
                    const char* p = token.c_str();
                    char* endp = nullptr;
                    v = std::strtol(p, &endp, 10);
                    if (*endp == '/') {
                        ++endp;
                        if (*endp != '/') {
                            t = std::strtol(endp, &endp, 10);
                        }
                        if (*endp == '/') {
                            ++endp;
                            n = std::strtol(endp, &endp, 10);
                        }
                    }
                }
                // Translate negative (relative) indices to absolute.
                auto absIdx = [](int idx, size_t poolSize) -> int {
                    if (idx < 0) return static_cast<int>(poolSize) + idx;
                    return idx - 1;  // OBJ is 1-based
                };
                int vi = absIdx(v, positions.size());
                int ti = (t == 0) ? -1 : absIdx(t, texcoords.size());
                int ni = (n == 0) ? -1 : absIdx(n, normals.size());
                if (vi < 0 || vi >= static_cast<int>(positions.size())) return -1;
                std::string key = std::to_string(vi) + "/" +
                                  std::to_string(ti) + "/" +
                                  std::to_string(ni);
                auto it = dedupe.find(key);
                if (it != dedupe.end()) return static_cast<int>(it->second);
                wowee::pipeline::WoweeModel::Vertex vert;
                vert.position = positions[vi];
                if (ti >= 0 && ti < static_cast<int>(texcoords.size())) {
                    vert.texCoord = texcoords[ti];
                    // Reverse the V-flip from --export-obj so a round-trip
                    // returns the original UVs unchanged.
                    vert.texCoord.y = 1.0f - vert.texCoord.y;
                } else {
                    vert.texCoord = {0, 0};
                }
                if (ni >= 0 && ni < static_cast<int>(normals.size())) {
                    vert.normal = normals[ni];
                } else {
                    vert.normal = {0, 0, 1};
                }
                uint32_t newIdx = static_cast<uint32_t>(wom.vertices.size());
                wom.vertices.push_back(vert);
                dedupe[key] = newIdx;
                return static_cast<int>(newIdx);
            };
            while (std::getline(in, line)) {
                // Strip CR for CRLF files.
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                std::istringstream ss(line);
                std::string tag;
                ss >> tag;
                if (tag == "v") {
                    glm::vec3 p; ss >> p.x >> p.y >> p.z;
                    positions.push_back(p);
                } else if (tag == "vt") {
                    glm::vec2 t; ss >> t.x >> t.y;
                    texcoords.push_back(t);
                } else if (tag == "vn") {
                    glm::vec3 n; ss >> n.x >> n.y >> n.z;
                    normals.push_back(n);
                } else if (tag == "o") {
                    if (objectName.empty()) ss >> objectName;
                } else if (tag == "f") {
                    std::vector<std::string> corners;
                    std::string c;
                    while (ss >> c) corners.push_back(c);
                    if (corners.size() < 3) { badFaces++; continue; }
                    std::vector<int> resolved;
                    resolved.reserve(corners.size());
                    bool ok = true;
                    for (const auto& cc : corners) {
                        int idx = resolveCorner(cc);
                        if (idx < 0) { ok = false; break; }
                        resolved.push_back(idx);
                    }
                    if (!ok) { badFaces++; continue; }
                    // Fan-triangulate (works for triangles, quads, and
                    // n-gons; assumes the polygon is convex which is the
                    // common case from DCC exporters).
                    if (resolved.size() > 3) triangulatedNgons++;
                    for (size_t k = 1; k + 1 < resolved.size(); ++k) {
                        wom.indices.push_back(static_cast<uint32_t>(resolved[0]));
                        wom.indices.push_back(static_cast<uint32_t>(resolved[k]));
                        wom.indices.push_back(static_cast<uint32_t>(resolved[k + 1]));
                    }
                }
                // mtllib/usemtl/g/s lines are silently skipped — material
                // info doesn't survive the round-trip but groups would
                // (left as future work; current import keeps it simple).
            }
            if (wom.vertices.empty() || wom.indices.empty()) {
                std::fprintf(stderr, "import-obj: no geometry found in %s\n",
                             objPath.c_str());
                return 1;
            }
            wom.name = objectName.empty()
                ? std::filesystem::path(objPath).stem().string()
                : objectName;
            // Compute bounds from positions — the renderer culls by these
            // so wrong values cause the model to disappear at distance.
            wom.boundMin = wom.vertices[0].position;
            wom.boundMax = wom.boundMin;
            for (const auto& v : wom.vertices) {
                wom.boundMin = glm::min(wom.boundMin, v.position);
                wom.boundMax = glm::max(wom.boundMax, v.position);
            }
            glm::vec3 center = (wom.boundMin + wom.boundMax) * 0.5f;
            float r2 = 0;
            for (const auto& v : wom.vertices) {
                glm::vec3 d = v.position - center;
                r2 = std::max(r2, glm::dot(d, d));
            }
            wom.boundRadius = std::sqrt(r2);
            if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
                std::fprintf(stderr, "import-obj: failed to write %s.wom\n",
                             womBase.c_str());
                return 1;
            }
            std::printf("Imported %s -> %s.wom\n", objPath.c_str(), womBase.c_str());
            std::printf("  %zu verts, %zu tris, bounds [%.2f, %.2f, %.2f] - [%.2f, %.2f, %.2f]\n",
                        wom.vertices.size(), wom.indices.size() / 3,
                        wom.boundMin.x, wom.boundMin.y, wom.boundMin.z,
                        wom.boundMax.x, wom.boundMax.y, wom.boundMax.z);
            if (triangulatedNgons > 0) {
                std::printf("  fan-triangulated %d n-gon(s)\n", triangulatedNgons);
            }
            if (badFaces > 0) {
                std::printf("  warning: skipped %d malformed face(s)\n", badFaces);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--export-png") == 0 && i + 1 < argc) {
            // Render heightmap, normal-map, and zone-map PNG previews for a
            // terrain. Useful for portfolio screenshots, ground-truth map
            // comparison, and quick visual validation without launching GUI.
            std::string base = argv[++i];
            for (const char* ext : {".wot", ".whm"}) {
                if (base.size() >= 4 && base.substr(base.size() - 4) == ext) {
                    base = base.substr(0, base.size() - 4);
                    break;
                }
            }
            if (!wowee::pipeline::WoweeTerrainLoader::exists(base)) {
                std::fprintf(stderr, "WOT/WHM not found at base: %s\n", base.c_str());
                return 1;
            }
            wowee::pipeline::ADTTerrain terrain;
            if (!wowee::pipeline::WoweeTerrainLoader::load(base, terrain)) {
                std::fprintf(stderr, "Failed to load terrain: %s\n", base.c_str());
                return 1;
            }
            wowee::editor::WoweeTerrain::exportHeightmapPreview(terrain, base + "_heightmap.png");
            wowee::editor::WoweeTerrain::exportNormalMap(terrain, base + "_normals.png");
            wowee::editor::WoweeTerrain::exportZoneMap(terrain, base + "_zone.png", 512);
            std::printf("Exported PNGs: %s_{heightmap,normals,zone}.png\n", base.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--fix-zone") == 0 && i + 1 < argc) {
            // Re-parse + re-save every JSON/binary file in a zone to apply
            // the editor's load-time scrubs and save-time caps. Useful when
            // an old zone was created before recent hardening — running
            // this once cleans up NaN/oversize fields without touching
            // the editor GUI.
            std::string zoneDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr, "fix-zone: %s does not exist\n", zoneDir.c_str());
                return 1;
            }
            int touched = 0;
            // zone.json
            {
                wowee::editor::ZoneManifest m;
                std::string p = zoneDir + "/zone.json";
                if (fs::exists(p) && m.load(p) && m.save(p)) touched++;
            }
            // creatures.json
            {
                wowee::editor::NpcSpawner sp;
                std::string p = zoneDir + "/creatures.json";
                if (fs::exists(p) && sp.loadFromFile(p) && sp.saveToFile(p)) touched++;
            }
            // objects.json
            {
                wowee::editor::ObjectPlacer op;
                std::string p = zoneDir + "/objects.json";
                if (fs::exists(p) && op.loadFromFile(p) && op.saveToFile(p)) touched++;
            }
            // quests.json
            {
                wowee::editor::QuestEditor qe;
                std::string p = zoneDir + "/quests.json";
                if (fs::exists(p) && qe.loadFromFile(p) && qe.saveToFile(p)) touched++;
            }
            // WHM/WOT pairs and WoB files would need full pipeline access;
            // skip them — the editor opens them on next zone load anyway,
            // and the load-time scrubs run then.
            std::printf("fix-zone: cleaned %d files in %s\n", touched, zoneDir.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--regen-collision") == 0 && i + 1 < argc) {
            // Find all WHM/WOT pairs under a zone dir and rebuild WOC for each.
            // Useful after sculpting changes when you want to re-derive
            // collision in batch instead of one tile at a time.
            std::string zoneDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr, "regen-collision: %s does not exist\n",
                             zoneDir.c_str());
                return 1;
            }
            int rebuilt = 0, failed = 0;
            for (auto& entry : fs::recursive_directory_iterator(zoneDir)) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() != ".whm") continue;
                std::string base = entry.path().string();
                base = base.substr(0, base.size() - 4); // strip .whm
                wowee::pipeline::ADTTerrain terrain;
                if (!wowee::pipeline::WoweeTerrainLoader::load(base, terrain)) {
                    std::fprintf(stderr, "  FAILED to load: %s\n", base.c_str());
                    failed++;
                    continue;
                }
                auto col = wowee::pipeline::WoweeCollisionBuilder::fromTerrain(terrain);
                std::string outPath = base + ".woc";
                if (wowee::pipeline::WoweeCollisionBuilder::save(col, outPath)) {
                    std::printf("  WOC rebuilt: %s (%zu triangles)\n",
                                outPath.c_str(), col.triangles.size());
                    rebuilt++;
                } else {
                    std::fprintf(stderr, "  FAILED to save: %s\n", outPath.c_str());
                    failed++;
                }
            }
            std::printf("regen-collision: %d rebuilt, %d failed\n", rebuilt, failed);
            return failed > 0 ? 1 : 0;
        } else if (std::strcmp(argv[i], "--build-woc") == 0 && i + 1 < argc) {
            // Generate a WOC collision mesh from a WHM/WOT terrain pair.
            // Uses terrain triangles only (no WMO overlays); useful as a
            // first-pass collision build before the editor adds buildings.
            std::string base = argv[++i];
            for (const char* ext : {".wot", ".whm", ".woc"}) {
                if (base.size() >= 4 && base.substr(base.size() - 4) == ext) {
                    base = base.substr(0, base.size() - 4);
                    break;
                }
            }
            if (!wowee::pipeline::WoweeTerrainLoader::exists(base)) {
                std::fprintf(stderr, "WOT/WHM not found at base: %s\n", base.c_str());
                return 1;
            }
            wowee::pipeline::ADTTerrain terrain;
            if (!wowee::pipeline::WoweeTerrainLoader::load(base, terrain)) {
                std::fprintf(stderr, "Failed to load terrain: %s\n", base.c_str());
                return 1;
            }
            auto col = wowee::pipeline::WoweeCollisionBuilder::fromTerrain(terrain);
            std::string outPath = base + ".woc";
            if (!wowee::pipeline::WoweeCollisionBuilder::save(col, outPath)) {
                std::fprintf(stderr, "WOC save failed: %s\n", outPath.c_str());
                return 1;
            }
            std::printf("WOC built: %s (%zu triangles, %zu walkable, %zu steep)\n",
                        outPath.c_str(),
                        col.triangles.size(), col.walkableCount(), col.steepCount());
            return 0;
        } else if (std::strcmp(argv[i], "--add-quest") == 0 && i + 2 < argc) {
            // Append a single quest to a zone's quests.json.
            // Args: <zoneDir> <title> [giverId] [turnInId] [xp] [level]
            std::string zoneDir = argv[++i];
            std::string title = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr, "add-quest: zone '%s' does not exist\n",
                             zoneDir.c_str());
                return 1;
            }
            wowee::editor::Quest q;
            q.title = title;
            // Optional positional args after title. Each is read in order;
            // an empty string or '-' stops consumption so users can omit
            // later fields.
            auto tryReadUint = [&](uint32_t& target) {
                if (i + 1 >= argc || argv[i + 1][0] == '-') return false;
                try {
                    target = static_cast<uint32_t>(std::stoul(argv[i + 1]));
                    ++i;
                    return true;
                } catch (...) { return false; }
            };
            tryReadUint(q.questGiverNpcId);
            tryReadUint(q.turnInNpcId);
            tryReadUint(q.reward.xp);
            tryReadUint(q.requiredLevel);
            wowee::editor::QuestEditor qe;
            std::string path = zoneDir + "/quests.json";
            if (fs::exists(path)) qe.loadFromFile(path);
            qe.addQuest(q);
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "add-quest: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Added quest '%s' to %s (now %zu total)\n",
                        title.c_str(), path.c_str(), qe.questCount());
            return 0;
        } else if (std::strcmp(argv[i], "--add-quest-objective") == 0 && i + 4 < argc) {
            // Append a single objective to an existing quest. The quest
            // must already exist (use --add-quest first); index is 0-based
            // and matches --list-quests output.
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string typeStr = argv[++i];
            std::string targetName = argv[++i];
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "add-quest-objective: %s not found — run --add-quest first\n",
                             path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "add-quest-objective: bad questIdx '%s'\n", idxStr.c_str());
                return 1;
            }
            using OT = wowee::editor::QuestObjectiveType;
            OT type;
            if      (typeStr == "kill")    type = OT::KillCreature;
            else if (typeStr == "collect") type = OT::CollectItem;
            else if (typeStr == "talk")    type = OT::TalkToNPC;
            else if (typeStr == "explore") type = OT::ExploreArea;
            else if (typeStr == "escort")  type = OT::EscortNPC;
            else if (typeStr == "use")     type = OT::UseObject;
            else {
                std::fprintf(stderr,
                    "add-quest-objective: type must be kill/collect/talk/explore/escort/use, got '%s'\n",
                    typeStr.c_str());
                return 1;
            }
            uint32_t count = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try {
                    count = static_cast<uint32_t>(std::stoul(argv[++i]));
                    if (count == 0) count = 1;
                } catch (...) {}
            }
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "add-quest-objective: failed to load %s\n", path.c_str());
                return 1;
            }
            if (idx < 0 || idx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr,
                    "add-quest-objective: questIdx %d out of range [0, %zu)\n",
                    idx, qe.questCount());
                return 1;
            }
            wowee::editor::QuestObjective obj;
            obj.type = type;
            obj.targetName = targetName;
            obj.targetCount = count;
            // Auto-generate a description from type+name+count so addons
            // and tooltips have something useful by default. The user can
            // edit quests.json directly if they want bespoke prose.
            const char* verb = "complete";
            switch (type) {
                case OT::KillCreature: verb = "Slay"; break;
                case OT::CollectItem:  verb = "Collect"; break;
                case OT::TalkToNPC:    verb = "Talk to"; break;
                case OT::ExploreArea:  verb = "Explore"; break;
                case OT::EscortNPC:    verb = "Escort"; break;
                case OT::UseObject:    verb = "Use"; break;
            }
            obj.description = std::string(verb) + " " +
                              (count > 1 ? std::to_string(count) + " " : "") +
                              targetName;
            // Quest is stored by value in the editor's vector; mutate via
            // the non-const getter, which gives us a pointer we can write
            // through.
            wowee::editor::Quest* q = qe.getQuest(idx);
            if (!q) {
                std::fprintf(stderr, "add-quest-objective: getQuest(%d) returned null\n", idx);
                return 1;
            }
            q->objectives.push_back(obj);
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "add-quest-objective: failed to write %s\n",
                             path.c_str());
                return 1;
            }
            std::printf("Added objective '%s' to quest %d ('%s'), now %zu objective(s)\n",
                        obj.description.c_str(), idx, q->title.c_str(),
                        q->objectives.size());
            return 0;
        } else if (std::strcmp(argv[i], "--remove-quest-objective") == 0 && i + 3 < argc) {
            // Symmetric counterpart to --add-quest-objective. Removes the
            // objective at <objIdx> within quest <questIdx>. Pair with
            // --info-quests / --list-quests to find the right indices.
            std::string zoneDir = argv[++i];
            std::string qIdxStr = argv[++i];
            std::string oIdxStr = argv[++i];
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "remove-quest-objective: %s not found\n", path.c_str());
                return 1;
            }
            int qIdx, oIdx;
            try {
                qIdx = std::stoi(qIdxStr);
                oIdx = std::stoi(oIdxStr);
            } catch (...) {
                std::fprintf(stderr, "remove-quest-objective: bad index\n");
                return 1;
            }
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "remove-quest-objective: failed to load %s\n",
                             path.c_str());
                return 1;
            }
            if (qIdx < 0 || qIdx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr,
                    "remove-quest-objective: questIdx %d out of range [0, %zu)\n",
                    qIdx, qe.questCount());
                return 1;
            }
            wowee::editor::Quest* q = qe.getQuest(qIdx);
            if (!q) return 1;
            if (oIdx < 0 || oIdx >= static_cast<int>(q->objectives.size())) {
                std::fprintf(stderr,
                    "remove-quest-objective: objIdx %d out of range [0, %zu)\n",
                    oIdx, q->objectives.size());
                return 1;
            }
            std::string removedDesc = q->objectives[oIdx].description;
            q->objectives.erase(q->objectives.begin() + oIdx);
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "remove-quest-objective: failed to write %s\n",
                             path.c_str());
                return 1;
            }
            std::printf("Removed objective '%s' (was index %d) from quest %d ('%s'), now %zu remaining\n",
                        removedDesc.c_str(), oIdx, qIdx, q->title.c_str(),
                        q->objectives.size());
            return 0;
        } else if (std::strcmp(argv[i], "--clone-quest") == 0 && i + 2 < argc) {
            // Duplicate a quest. Useful for templating: create a base
            // quest with objectives + rewards once, then clone N times
            // for variants ('Slay Wolves', 'Slay Bears' with the same
            // shape). Optional newTitle replaces the cloned copy's title;
            // omit to get '<original> (copy)'.
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string newTitle;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                newTitle = argv[++i];
            }
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "clone-quest: %s not found\n", path.c_str());
                return 1;
            }
            int qIdx;
            try { qIdx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "clone-quest: bad questIdx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "clone-quest: failed to load %s\n", path.c_str());
                return 1;
            }
            if (qIdx < 0 || qIdx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr,
                    "clone-quest: questIdx %d out of range [0, %zu)\n",
                    qIdx, qe.questCount());
                return 1;
            }
            // Deep-copy by value via vector iteration; .objectives and
            // .reward are STL containers so the copy is automatic.
            wowee::editor::Quest clone = qe.getQuests()[qIdx];
            // Reset id so the editor's auto-id sequence assigns a fresh
            // one — addQuest does this internally if id==0.
            clone.id = 0;
            // Reset chain link too — copying a chained quest with the
            // same nextQuestId would corrupt the chain semantics.
            clone.nextQuestId = 0;
            clone.title = newTitle.empty()
                ? (clone.title + " (copy)")
                : newTitle;
            qe.addQuest(clone);
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "clone-quest: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Cloned quest %d -> '%s' (now %zu total)\n",
                        qIdx, clone.title.c_str(), qe.questCount());
            std::printf("  carried %zu objective(s), %zu item reward(s), xp=%u\n",
                        clone.objectives.size(),
                        clone.reward.itemRewards.size(),
                        clone.reward.xp);
            return 0;
        } else if (std::strcmp(argv[i], "--clone-creature") == 0 && i + 2 < argc) {
            // Duplicate a creature spawn. Common workflow: design one
            // 'patrol guard' archetype, then clone it across spawn points
            // around a town. Preserves stats, faction, behavior, equipment;
            // resets id and offsets position by 5 yards by default so the
            // copy doesn't z-fight with the original.
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string newName;
            float dx = 5.0f, dy = 0.0f, dz = 0.0f;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                newName = argv[++i];
            }
            // Optional 3-axis offset after newName.
            if (i + 3 < argc && argv[i + 1][0] != '-') {
                try {
                    dx = std::stof(argv[++i]);
                    dy = std::stof(argv[++i]);
                    dz = std::stof(argv[++i]);
                } catch (...) {
                    std::fprintf(stderr, "clone-creature: bad offset coordinate\n");
                    return 1;
                }
            }
            std::string path = zoneDir + "/creatures.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "clone-creature: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "clone-creature: bad idx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::NpcSpawner sp;
            if (!sp.loadFromFile(path)) {
                std::fprintf(stderr, "clone-creature: failed to load %s\n", path.c_str());
                return 1;
            }
            if (idx < 0 || idx >= static_cast<int>(sp.spawnCount())) {
                std::fprintf(stderr,
                    "clone-creature: idx %d out of range [0, %zu)\n",
                    idx, sp.spawnCount());
                return 1;
            }
            // Deep-copy by value; CreatureSpawn is POD-ish (vectors for
            // patrol points copy automatically).
            wowee::editor::CreatureSpawn clone = sp.getSpawns()[idx];
            clone.id = 0;  // addCreature auto-assigns a fresh id
            clone.name = newName.empty()
                ? (clone.name + " (copy)")
                : newName;
            clone.position.x += dx;
            clone.position.y += dy;
            clone.position.z += dz;
            // Patrol path is intentionally NOT offset — patrol points are
            // typically authored as world-space waypoints, not relative to
            // the spawn. Designers re-author the path if needed.
            sp.getSpawns().push_back(clone);
            if (!sp.saveToFile(path)) {
                std::fprintf(stderr, "clone-creature: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Cloned creature %d -> '%s' at (%.1f, %.1f, %.1f) (now %zu total)\n",
                        idx, clone.name.c_str(),
                        clone.position.x, clone.position.y, clone.position.z,
                        sp.spawnCount());
            return 0;
        } else if (std::strcmp(argv[i], "--clone-object") == 0 && i + 2 < argc) {
            // Symmetric to --clone-creature/--clone-quest. Common
            // workflow: place one tree/lamp/barrel just right, then
            // clone N copies along a path or around a square. Default
            // 5-yard X offset prevents z-fighting; rotation/scale are
            // preserved so a tilted object stays tilted.
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            float dx = 5.0f, dy = 0.0f, dz = 0.0f;
            if (i + 3 < argc && argv[i + 1][0] != '-') {
                try {
                    dx = std::stof(argv[++i]);
                    dy = std::stof(argv[++i]);
                    dz = std::stof(argv[++i]);
                } catch (...) {
                    std::fprintf(stderr, "clone-object: bad offset\n");
                    return 1;
                }
            }
            std::string path = zoneDir + "/objects.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "clone-object: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "clone-object: bad idx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::ObjectPlacer placer;
            if (!placer.loadFromFile(path)) {
                std::fprintf(stderr, "clone-object: failed to load %s\n", path.c_str());
                return 1;
            }
            auto& objs = placer.getObjects();
            if (idx < 0 || idx >= static_cast<int>(objs.size())) {
                std::fprintf(stderr,
                    "clone-object: idx %d out of range [0, %zu)\n",
                    idx, objs.size());
                return 1;
            }
            // Deep-copy by value. uniqueId is reset so the new object
            // doesn't collide with the source's identifier in any
            // downstream system that dedups by it.
            wowee::editor::PlacedObject clone = objs[idx];
            clone.uniqueId = 0;
            clone.selected = false;
            clone.position.x += dx;
            clone.position.y += dy;
            clone.position.z += dz;
            objs.push_back(clone);
            if (!placer.saveToFile(path)) {
                std::fprintf(stderr, "clone-object: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Cloned object %d -> '%s' at (%.1f, %.1f, %.1f) (now %zu total)\n",
                        idx, clone.path.c_str(),
                        clone.position.x, clone.position.y, clone.position.z,
                        objs.size());
            return 0;
        } else if (std::strcmp(argv[i], "--add-quest-reward-item") == 0 && i + 3 < argc) {
            // Append one or more item rewards to a quest. Multiple paths
            // can be passed in a single invocation:
            //   --add-quest-reward-item zone 0 'Item:Sword' 'Item:Shield'
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "add-quest-reward-item: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "add-quest-reward-item: bad questIdx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "add-quest-reward-item: failed to load %s\n", path.c_str());
                return 1;
            }
            if (idx < 0 || idx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr,
                    "add-quest-reward-item: questIdx %d out of range [0, %zu)\n",
                    idx, qe.questCount());
                return 1;
            }
            wowee::editor::Quest* q = qe.getQuest(idx);
            if (!q) return 1;
            int added = 0;
            // Greedy-consume any remaining args that don't start with '-'
            // so the caller can batch-add a whole loot table in one shot.
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                q->reward.itemRewards.push_back(argv[++i]);
                added++;
            }
            if (added == 0) {
                std::fprintf(stderr, "add-quest-reward-item: need at least one itemPath\n");
                return 1;
            }
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "add-quest-reward-item: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Added %d item reward(s) to quest %d ('%s'), now %zu total\n",
                        added, idx, q->title.c_str(), q->reward.itemRewards.size());
            return 0;
        } else if (std::strcmp(argv[i], "--set-quest-reward") == 0 && i + 2 < argc) {
            // Update XP / coin reward fields on an existing quest. Each
            // field is optional — only the ones explicitly passed are
            // changed. This avoids the round-trip-and-clobber footgun of
            // a "replace whole reward" command.
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "set-quest-reward: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "set-quest-reward: bad questIdx '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::QuestEditor qe;
            if (!qe.loadFromFile(path)) {
                std::fprintf(stderr, "set-quest-reward: failed to load %s\n", path.c_str());
                return 1;
            }
            if (idx < 0 || idx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr,
                    "set-quest-reward: questIdx %d out of range [0, %zu)\n",
                    idx, qe.questCount());
                return 1;
            }
            wowee::editor::Quest* q = qe.getQuest(idx);
            if (!q) return 1;
            int changed = 0;
            auto consumeUint = [&](const char* flag, uint32_t& target) {
                if (i + 2 < argc && std::strcmp(argv[i + 1], flag) == 0) {
                    try {
                        target = static_cast<uint32_t>(std::stoul(argv[i + 2]));
                        i += 2;
                        changed++;
                        return true;
                    } catch (...) {
                        std::fprintf(stderr, "set-quest-reward: bad %s value '%s'\n",
                                     flag, argv[i + 2]);
                    }
                }
                return false;
            };
            // Loop until no more recognised flags consume their value —
            // order-independent, so callers can pass --gold then --xp.
            bool any = true;
            while (any) {
                any = false;
                if (consumeUint("--xp",     q->reward.xp))     any = true;
                if (consumeUint("--gold",   q->reward.gold))   any = true;
                if (consumeUint("--silver", q->reward.silver)) any = true;
                if (consumeUint("--copper", q->reward.copper)) any = true;
            }
            if (changed == 0) {
                std::fprintf(stderr,
                    "set-quest-reward: no fields changed — pass --xp / --gold / --silver / --copper\n");
                return 1;
            }
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "set-quest-reward: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Updated %d field(s) on quest %d ('%s'): xp=%u gold=%u silver=%u copper=%u\n",
                        changed, idx, q->title.c_str(),
                        q->reward.xp, q->reward.gold,
                        q->reward.silver, q->reward.copper);
            return 0;
        } else if (std::strcmp(argv[i], "--remove-creature") == 0 && i + 2 < argc) {
            // Remove a creature spawn by 0-based index. Pair with
            // --info-creatures (or your editor) to find the right index
            // first; nothing identifies entries reliably across reloads.
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string path = zoneDir + "/creatures.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "remove-creature: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "remove-creature: bad index '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::NpcSpawner sp;
            sp.loadFromFile(path);
            if (idx < 0 || idx >= static_cast<int>(sp.spawnCount())) {
                std::fprintf(stderr, "remove-creature: index %d out of range [0, %zu)\n",
                             idx, sp.spawnCount());
                return 1;
            }
            std::string removedName = sp.getSpawns()[idx].name;
            sp.removeCreature(idx);
            if (!sp.saveToFile(path)) {
                std::fprintf(stderr, "remove-creature: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Removed creature '%s' (was index %d) from %s (now %zu total)\n",
                        removedName.c_str(), idx, path.c_str(), sp.spawnCount());
            return 0;
        } else if (std::strcmp(argv[i], "--remove-object") == 0 && i + 2 < argc) {
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string path = zoneDir + "/objects.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "remove-object: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "remove-object: bad index '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::ObjectPlacer placer;
            placer.loadFromFile(path);
            auto& objs = placer.getObjects();
            if (idx < 0 || idx >= static_cast<int>(objs.size())) {
                std::fprintf(stderr, "remove-object: index %d out of range [0, %zu)\n",
                             idx, objs.size());
                return 1;
            }
            std::string removedPath = objs[idx].path;
            objs.erase(objs.begin() + idx);
            if (!placer.saveToFile(path)) {
                std::fprintf(stderr, "remove-object: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Removed object '%s' (was index %d) from %s (now %zu total)\n",
                        removedPath.c_str(), idx, path.c_str(), objs.size());
            return 0;
        } else if (std::strcmp(argv[i], "--remove-quest") == 0 && i + 2 < argc) {
            std::string zoneDir = argv[++i];
            std::string idxStr = argv[++i];
            std::string path = zoneDir + "/quests.json";
            if (!std::filesystem::exists(path)) {
                std::fprintf(stderr, "remove-quest: %s not found\n", path.c_str());
                return 1;
            }
            int idx;
            try { idx = std::stoi(idxStr); }
            catch (...) {
                std::fprintf(stderr, "remove-quest: bad index '%s'\n", idxStr.c_str());
                return 1;
            }
            wowee::editor::QuestEditor qe;
            qe.loadFromFile(path);
            if (idx < 0 || idx >= static_cast<int>(qe.questCount())) {
                std::fprintf(stderr, "remove-quest: index %d out of range [0, %zu)\n",
                             idx, qe.questCount());
                return 1;
            }
            std::string removedTitle = qe.getQuests()[idx].title;
            qe.removeQuest(idx);
            if (!qe.saveToFile(path)) {
                std::fprintf(stderr, "remove-quest: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Removed quest '%s' (was index %d) from %s (now %zu total)\n",
                        removedTitle.c_str(), idx, path.c_str(), qe.questCount());
            return 0;
        } else if (std::strcmp(argv[i], "--add-object") == 0 && i + 5 < argc) {
            // Append a single object placement to a zone's objects.json.
            // Args: <zoneDir> <m2|wmo> <gamePath> <x> <y> <z> [scale]
            std::string zoneDir = argv[++i];
            std::string typeStr = argv[++i];
            std::string gamePath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr, "add-object: zone '%s' does not exist\n",
                             zoneDir.c_str());
                return 1;
            }
            wowee::editor::PlaceableType ptype;
            if (typeStr == "m2") ptype = wowee::editor::PlaceableType::M2;
            else if (typeStr == "wmo") ptype = wowee::editor::PlaceableType::WMO;
            else {
                std::fprintf(stderr, "add-object: type must be 'm2' or 'wmo'\n");
                return 1;
            }
            glm::vec3 pos;
            try {
                pos.x = std::stof(argv[++i]);
                pos.y = std::stof(argv[++i]);
                pos.z = std::stof(argv[++i]);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "add-object: bad coordinate (%s)\n", e.what());
                return 1;
            }
            wowee::editor::ObjectPlacer placer;
            std::string path = zoneDir + "/objects.json";
            if (fs::exists(path)) placer.loadFromFile(path);
            placer.setActivePath(gamePath, ptype);
            placer.placeObject(pos);
            // Optional scale after coordinates.
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try {
                    float scale = std::stof(argv[++i]);
                    if (std::isfinite(scale) && scale > 0.0f) {
                        // Set scale on the just-placed object (last in list).
                        placer.getObjects().back().scale = scale;
                    }
                } catch (...) {}
            }
            if (!placer.saveToFile(path)) {
                std::fprintf(stderr, "add-object: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Added %s '%s' to %s (now %zu total)\n",
                        typeStr.c_str(), gamePath.c_str(), path.c_str(),
                        placer.getObjects().size());
            return 0;
        } else if (std::strcmp(argv[i], "--add-creature") == 0 && i + 4 < argc) {
            // Append a single creature spawn to a zone's creatures.json.
            // Args: <zoneDir> <name> <x> <y> <z> [displayId] [level]
            // Useful for batch-populating zones via shell script without
            // launching the GUI placement tool.
            std::string zoneDir = argv[++i];
            std::string name = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr, "add-creature: zone '%s' does not exist\n",
                             zoneDir.c_str());
                return 1;
            }
            wowee::editor::CreatureSpawn s;
            s.name = name;
            try {
                s.position.x = std::stof(argv[++i]);
                s.position.y = std::stof(argv[++i]);
                s.position.z = std::stof(argv[++i]);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "add-creature: bad coordinate (%s)\n", e.what());
                return 1;
            }
            // Optional displayId (positional, after coordinates).
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try {
                    s.displayId = static_cast<uint32_t>(std::stoul(argv[++i]));
                } catch (...) { /* leave 0 → SQL exporter substitutes 11707 */ }
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try {
                    s.level = static_cast<uint32_t>(std::stoul(argv[++i]));
                } catch (...) { /* leave default 1 */ }
            }
            // Load existing spawns (if any), append, save.
            wowee::editor::NpcSpawner spawner;
            std::string path = zoneDir + "/creatures.json";
            if (fs::exists(path)) spawner.loadFromFile(path);
            spawner.placeCreature(s);
            if (!spawner.saveToFile(path)) {
                std::fprintf(stderr, "add-creature: failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("Added creature '%s' to %s (now %zu total)\n",
                        name.c_str(), path.c_str(), spawner.spawnCount());
            return 0;
        } else if (std::strcmp(argv[i], "--add-item") == 0 && i + 2 < argc) {
            // Append one item entry to <zoneDir>/items.json. Inline
            // JSON without a dedicated editor class — items.json is
            // a simple {"items": [...]} array of records, and the
            // schema is small enough that we don't need NpcSpawner-
            // style infrastructure yet.
            //
            // Schema per item:
            //   id (uint32) — Item.dbc primary key (auto-increments
            //                 from 1 if omitted)
            //   name (string)
            //   quality (uint8) — 0..6 (poor..artifact, default 1)
            //   displayId (uint32) — ItemDisplayInfo index (default 0)
            //   itemLevel (uint32) — default 1
            //   stackable (uint32) — max stack size (default 1)
            std::string zoneDir = argv[++i];
            std::string name = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr,
                    "add-item: zone '%s' does not exist\n", zoneDir.c_str());
                return 1;
            }
            uint32_t id = 0, displayId = 0, itemLevel = 1;
            uint32_t quality = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try { id = static_cast<uint32_t>(std::stoul(argv[++i])); }
                catch (...) {}
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try { quality = static_cast<uint32_t>(std::stoul(argv[++i])); }
                catch (...) {}
                if (quality > 6) quality = 1;
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try { displayId = static_cast<uint32_t>(std::stoul(argv[++i])); }
                catch (...) {}
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try { itemLevel = static_cast<uint32_t>(std::stoul(argv[++i])); }
                catch (...) {}
            }
            std::string path = zoneDir + "/items.json";
            nlohmann::json doc = nlohmann::json::object({{"items",
                                                          nlohmann::json::array()}});
            if (fs::exists(path)) {
                std::ifstream in(path);
                try { in >> doc; } catch (...) {
                    std::fprintf(stderr,
                        "add-item: %s exists but is not valid JSON\n",
                        path.c_str());
                    return 1;
                }
                if (!doc.contains("items") || !doc["items"].is_array()) {
                    doc["items"] = nlohmann::json::array();
                }
            }
            // Auto-assign id if user passed 0 / nothing — pick the
            // smallest unused positive integer so the items.json
            // numbering stays contiguous.
            if (id == 0) {
                std::set<uint32_t> used;
                for (const auto& it : doc["items"]) {
                    if (it.contains("id") && it["id"].is_number_unsigned()) {
                        used.insert(it["id"].get<uint32_t>());
                    }
                }
                id = 1;
                while (used.count(id)) ++id;
            }
            // Reject duplicate id so the user notices a collision.
            for (const auto& it : doc["items"]) {
                if (it.contains("id") && it["id"].is_number_unsigned() &&
                    it["id"].get<uint32_t>() == id) {
                    std::fprintf(stderr,
                        "add-item: id %u already in use in %s\n",
                        id, path.c_str());
                    return 1;
                }
            }
            nlohmann::json item = {
                {"id", id},
                {"name", name},
                {"quality", quality},
                {"displayId", displayId},
                {"itemLevel", itemLevel},
                {"stackable", 1},
            };
            doc["items"].push_back(item);
            std::ofstream out(path);
            if (!out) {
                std::fprintf(stderr,
                    "add-item: failed to write %s\n", path.c_str());
                return 1;
            }
            out << doc.dump(2);
            out.close();
            static const char* qualityNames[] = {
                "poor", "common", "uncommon", "rare", "epic",
                "legendary", "artifact"
            };
            std::printf("Added item '%s' (id=%u, quality=%s, ilvl=%u) to %s (now %zu total)\n",
                        name.c_str(), id,
                        qualityNames[quality], itemLevel,
                        path.c_str(), doc["items"].size());
            return 0;
        } else if (std::strcmp(argv[i], "--random-populate-zone") == 0 && i + 1 < argc) {
            // Randomly add creatures and/or objects to a zone for
            // playtest scenarios. Reads the zone manifest's tile
            // bounds so spawn positions stay inside the actual
            // playable area. Seeded LCG for reproducibility — same
            // seed always produces the same population.
            //
            // Flags:
            //   --seed N      (default 42)
            //   --creatures N (default 20)
            //   --objects N   (default 10)
            std::string zoneDir = argv[++i];
            uint32_t seed = 42;
            int creatureCount = 20;
            int objectCount = 10;
            while (i + 2 < argc && argv[i + 1][0] == '-') {
                std::string flag = argv[++i];
                if (flag == "--seed") {
                    try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); }
                    catch (...) {}
                } else if (flag == "--creatures") {
                    try { creatureCount = std::stoi(argv[++i]); }
                    catch (...) {}
                } else if (flag == "--objects") {
                    try { objectCount = std::stoi(argv[++i]); }
                    catch (...) {}
                } else {
                    std::fprintf(stderr,
                        "random-populate-zone: unknown flag '%s'\n", flag.c_str());
                    return 1;
                }
            }
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "random-populate-zone: %s has no zone.json\n",
                    zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr,
                    "random-populate-zone: failed to parse %s\n",
                    manifestPath.c_str());
                return 1;
            }
            if (zm.tiles.empty()) {
                std::fprintf(stderr,
                    "random-populate-zone: zone has no tiles to populate\n");
                return 1;
            }
            // Compute the world AABB the zone occupies so spawns land
            // inside it. Each tile is 533.33y; WoW grid centers tile
            // (32, 32) at world origin.
            constexpr float kTileSize = 533.33333f;
            int tMinX = 64, tMaxX = -1, tMinY = 64, tMaxY = -1;
            for (const auto& [tx, ty] : zm.tiles) {
                tMinX = std::min(tMinX, tx); tMaxX = std::max(tMaxX, tx);
                tMinY = std::min(tMinY, ty); tMaxY = std::max(tMaxY, ty);
            }
            float wMinX = (32.0f - tMaxY - 1) * kTileSize;
            float wMaxX = (32.0f - tMinY)     * kTileSize;
            float wMinY = (32.0f - tMaxX - 1) * kTileSize;
            float wMaxY = (32.0f - tMinX)     * kTileSize;
            float baseZ = zm.baseHeight;

            uint32_t rng = seed ? seed : 1u;
            auto next01 = [&]() {
                rng = rng * 1664525u + 1013904223u;
                return (rng >> 8) / float(1 << 24);
            };
            auto rangeF = [&](float a, float b) { return a + next01() * (b - a); };
            auto rangeI = [&](int a, int b) {
                return a + static_cast<int>(next01() * (b - a + 1));
            };

            // Tiny bestiary so the random output reads as plausible
            // rather than "Creature1 / Creature2".
            static const std::vector<std::pair<const char*, uint32_t>> kRandomCreatures = {
                {"Wolf", 5},      {"Boar", 4},     {"Bear", 7},
                {"Spider", 3},    {"Bandit", 6},   {"Kobold", 4},
                {"Murloc", 5},    {"Skeleton", 5}, {"Wisp", 3},
                {"Goblin", 5},    {"Stag", 4},     {"Crab", 3},
            };
            static const std::vector<const char*> kRandomObjects = {
                "World/Generic/Tree01.wmo",
                "World/Generic/Boulder.wmo",
                "World/Generic/Bush.wmo",
                "World/Generic/Stump.wmo",
                "World/Generic/Mushroom.wmo",
            };

            // Creatures.
            wowee::editor::NpcSpawner spawner;
            std::string cpath = zoneDir + "/creatures.json";
            if (fs::exists(cpath)) spawner.loadFromFile(cpath);
            int placedCreatures = 0;
            for (int n = 0; n < creatureCount; ++n) {
                const auto& [name, baseLvl] = kRandomCreatures[
                    rangeI(0, static_cast<int>(kRandomCreatures.size()) - 1)];
                wowee::editor::CreatureSpawn s;
                s.name = name;
                s.position.x = rangeF(wMinX, wMaxX);
                s.position.y = rangeF(wMinY, wMaxY);
                s.position.z = baseZ;
                int lvl = std::max(1, static_cast<int>(baseLvl) + rangeI(-1, 2));
                s.level = static_cast<uint32_t>(lvl);
                s.health = 50 + s.level * 10;
                s.orientation = rangeF(0.0f, 360.0f);
                spawner.placeCreature(s);
                placedCreatures++;
            }
            if (placedCreatures > 0) spawner.saveToFile(cpath);
            // Objects.
            wowee::editor::ObjectPlacer placer;
            std::string opath = zoneDir + "/objects.json";
            if (fs::exists(opath)) placer.loadFromFile(opath);
            int placedObjects = 0;
            // Push PlacedObject directly into the placer's vector so
            // we don't fight placeObject()'s early-return on empty
            // activePath_. uniqueId starts after any existing objects
            // to keep IDs collision-free.
            auto& objs = placer.getObjects();
            uint32_t maxUid = 0;
            for (const auto& o : objs) maxUid = std::max(maxUid, o.uniqueId);
            for (int n = 0; n < objectCount; ++n) {
                wowee::editor::PlacedObject o;
                o.path = kRandomObjects[
                    rangeI(0, static_cast<int>(kRandomObjects.size()) - 1)];
                o.type = wowee::editor::PlaceableType::WMO;
                o.position.x = rangeF(wMinX, wMaxX);
                o.position.y = rangeF(wMinY, wMaxY);
                o.position.z = baseZ;
                o.rotation = glm::vec3(0.0f, rangeF(0.0f, 6.28f), 0.0f);
                o.scale = rangeF(0.8f, 1.4f);
                o.uniqueId = ++maxUid;
                o.nameId = 0;
                o.selected = false;
                objs.push_back(o);
                placedObjects++;
            }
            if (placedObjects > 0) placer.saveToFile(opath);
            std::printf("random-populate-zone: %s\n", zoneDir.c_str());
            std::printf("  seed       : %u\n", seed);
            std::printf("  zone bbox  : (%.0f, %.0f) - (%.0f, %.0f)\n",
                        wMinX, wMinY, wMaxX, wMaxY);
            std::printf("  creatures  : %d added (%zu total)\n",
                        placedCreatures, spawner.spawnCount());
            std::printf("  objects    : %d added (%zu total)\n",
                        placedObjects, placer.getObjects().size());
            return 0;
        } else if (std::strcmp(argv[i], "--random-populate-items") == 0 && i + 1 < argc) {
            // Seeded random items.json populator. Pulls a base name
            // and a noun from inline word lists, picks a quality up
            // to maxQuality, randomizes itemLevel and stack size
            // around plausible defaults. Useful for playtest loot
            // tables that need bulk content without hand-typing each
            // entry.
            //
            // Flags: --seed N (default 7), --count N (default 30),
            //        --max-quality Q (default 4 = epic; 0..6 valid).
            std::string zoneDir = argv[++i];
            uint32_t seed = 7;
            int count = 30;
            int maxQuality = 4;
            while (i + 2 < argc && argv[i + 1][0] == '-') {
                std::string flag = argv[++i];
                if (flag == "--seed") {
                    try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); }
                    catch (...) {}
                } else if (flag == "--count") {
                    try { count = std::stoi(argv[++i]); } catch (...) {}
                } else if (flag == "--max-quality") {
                    try { maxQuality = std::stoi(argv[++i]); } catch (...) {}
                } else {
                    std::fprintf(stderr,
                        "random-populate-items: unknown flag '%s'\n", flag.c_str());
                    return 1;
                }
            }
            if (maxQuality < 0 || maxQuality > 6) maxQuality = 4;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "random-populate-items: %s has no zone.json\n",
                    zoneDir.c_str());
                return 1;
            }
            uint32_t rng = seed ? seed : 1u;
            auto next01 = [&]() {
                rng = rng * 1664525u + 1013904223u;
                return (rng >> 8) / float(1 << 24);
            };
            auto rangeI = [&](int a, int b) {
                return a + static_cast<int>(next01() * (b - a + 1));
            };
            // Inline name lexicon. {prefix, noun} → "Glowing Sword".
            // Quality ramps prefix selection; rare+ items get fancier
            // adjectives.
            static const std::vector<const char*> kPrefixes[5] = {
                {"Worn", "Tattered", "Cracked", "Dented", "Faded"},      // poor
                {"Common", "Plain", "Basic", "Simple", "Standard"},      // common
                {"Sharp", "Sturdy", "Polished", "Reinforced", "Fine"},   // uncommon
                {"Glowing", "Runed", "Enchanted", "Storm", "Mystic"},    // rare
                {"Ancient", "Eternal", "Heroic", "Vengeful", "Soul"},    // epic
            };
            static const std::vector<const char*> kNouns = {
                "Sword", "Mace", "Axe", "Dagger", "Staff",
                "Bow", "Helm", "Cuirass", "Greaves", "Gauntlets",
                "Ring", "Amulet", "Cloak", "Belt", "Boots",
                "Potion", "Scroll", "Tome", "Wand", "Shield",
            };
            // Open the items doc.
            std::string ipath = zoneDir + "/items.json";
            nlohmann::json doc = nlohmann::json::object({{"items",
                                  nlohmann::json::array()}});
            if (fs::exists(ipath)) {
                std::ifstream in(ipath);
                try { in >> doc; } catch (...) {}
                if (!doc.contains("items") || !doc["items"].is_array()) {
                    doc["items"] = nlohmann::json::array();
                }
            }
            std::set<uint32_t> used;
            for (const auto& it : doc["items"]) {
                if (it.contains("id") && it["id"].is_number_unsigned())
                    used.insert(it["id"].get<uint32_t>());
            }
            int added = 0;
            for (int n = 0; n < count; ++n) {
                int q = std::min(maxQuality, rangeI(0, maxQuality));
                int qBucket = std::min(q, 4);
                const auto& prefixes = kPrefixes[qBucket];
                std::string name = prefixes[rangeI(0,
                    static_cast<int>(prefixes.size()) - 1)];
                name += " ";
                name += kNouns[rangeI(0, static_cast<int>(kNouns.size()) - 1)];
                uint32_t id = 1;
                while (used.count(id)) ++id;
                used.insert(id);
                int ilvl = std::max(1,
                    rangeI(1, 5) + q * 12 + rangeI(-3, 3));
                doc["items"].push_back({
                    {"id", id},
                    {"name", name},
                    {"quality", q},
                    {"displayId", rangeI(1000, 9999)},
                    {"itemLevel", ilvl},
                    {"stackable", q == 0 || q == 1 ? rangeI(1, 20) : 1},
                });
                added++;
            }
            std::ofstream out(ipath);
            if (!out) {
                std::fprintf(stderr,
                    "random-populate-items: failed to write %s\n",
                    ipath.c_str());
                return 1;
            }
            out << doc.dump(2);
            out.close();
            std::printf("random-populate-items: %s\n", ipath.c_str());
            std::printf("  seed         : %u\n", seed);
            std::printf("  added        : %d\n", added);
            std::printf("  total items  : %zu\n", doc["items"].size());
            std::printf("  max quality  : %d\n", maxQuality);
            return 0;
        } else if (std::strcmp(argv[i], "--gen-random-zone") == 0 && i + 1 < argc) {
            // End-to-end random zone generator. Composes scaffold-zone
            // + random-populate-zone + random-populate-items in one
            // invocation. Useful for "I just want a complete test
            // zone, don't make me chain three commands."
            //
            // Args:
            //   <name>                   required (becomes the slug)
            //   [tx ty]                  optional (default 32 32)
            //   --seed N                 default 42
            //   --creatures N            default 20
            //   --objects N              default 10
            //   --items N                default 25
            //
            // Honors --random-populate-zone's hard caps + the existing
            // scaffold-zone validation. Sub-commands' output streams
            // through.
            std::string name = argv[++i];
            int tx = 32, ty = 32;
            uint32_t seed = 42;
            int creatures = 20, objects = 10, items = 25;
            // Optional positional tx/ty (must be before any --flags).
            if (i + 2 < argc && argv[i + 1][0] != '-' && argv[i + 2][0] != '-') {
                try { tx = std::stoi(argv[++i]); ty = std::stoi(argv[++i]); }
                catch (...) {}
            }
            while (i + 2 < argc && argv[i + 1][0] == '-') {
                std::string flag = argv[++i];
                if (flag == "--seed")
                    try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
                else if (flag == "--creatures")
                    try { creatures = std::stoi(argv[++i]); } catch (...) {}
                else if (flag == "--objects")
                    try { objects = std::stoi(argv[++i]); } catch (...) {}
                else if (flag == "--items")
                    try { items = std::stoi(argv[++i]); } catch (...) {}
                else {
                    std::fprintf(stderr,
                        "gen-random-zone: unknown flag '%s'\n", flag.c_str());
                    return 1;
                }
            }
            // Slug-clean the name to match scaffold-zone's expectations.
            std::string slug;
            for (char c : name) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-') {
                    slug += c;
                } else if (c == ' ') {
                    slug += '_';
                }
            }
            if (slug.empty()) {
                std::fprintf(stderr,
                    "gen-random-zone: name '%s' has no valid characters\n",
                    name.c_str());
                return 1;
            }
            std::string self = argv[0];
            namespace fs = std::filesystem;
            std::string zoneDir = "custom_zones/" + slug;
            std::printf("gen-random-zone: %s (tile %d, %d)\n",
                        slug.c_str(), tx, ty);
            std::fflush(stdout);
            // 1. Scaffold.
            std::string scaffoldCmd = "\"" + self + "\" --scaffold-zone \"" +
                                       slug + "\" " + std::to_string(tx) + " " +
                                       std::to_string(ty);
            int rc = std::system(scaffoldCmd.c_str());
            if (rc != 0) {
                std::fprintf(stderr,
                    "gen-random-zone: scaffold step failed (rc=%d)\n", rc);
                return 1;
            }
            // 2. Random populate.
            std::fflush(stdout);
            std::string popCmd = "\"" + self + "\" --random-populate-zone \"" +
                                  zoneDir + "\" --seed " + std::to_string(seed) +
                                  " --creatures " + std::to_string(creatures) +
                                  " --objects " + std::to_string(objects);
            rc = std::system(popCmd.c_str());
            if (rc != 0) {
                std::fprintf(stderr,
                    "gen-random-zone: populate step failed (rc=%d)\n", rc);
                return 1;
            }
            // 3. Random items.
            std::fflush(stdout);
            std::string itemsCmd = "\"" + self + "\" --random-populate-items \"" +
                                    zoneDir + "\" --seed " + std::to_string(seed + 1) +
                                    " --count " + std::to_string(items);
            rc = std::system(itemsCmd.c_str());
            if (rc != 0) {
                std::fprintf(stderr,
                    "gen-random-zone: items step failed (rc=%d)\n", rc);
                return 1;
            }
            std::printf("\ngen-random-zone: complete\n");
            std::printf("  zone dir  : %s\n", zoneDir.c_str());
            std::printf("  creatures : %d\n", creatures);
            std::printf("  objects   : %d\n", objects);
            std::printf("  items     : %d\n", items);
            return 0;
        } else if (std::strcmp(argv[i], "--gen-random-project") == 0 && i + 1 < argc) {
            // Project-wide companion: spawn N random zones in one
            // pass. Names default to "Zone1, Zone2..."; tile
            // coordinates step from (32, 32) outward in a simple
            // raster so they don't overlap. Each zone gets a unique
            // sub-seed so its random content differs.
            int count = 0;
            try { count = std::stoi(argv[++i]); }
            catch (...) {
                std::fprintf(stderr,
                    "gen-random-project: <count> must be an integer\n");
                return 1;
            }
            if (count < 1 || count > 100) {
                std::fprintf(stderr,
                    "gen-random-project: count %d out of range (1..100)\n",
                    count);
                return 1;
            }
            std::string prefix = "Zone";
            uint32_t seed = 100;
            int creatures = 20, objects = 10, items = 25;
            while (i + 2 < argc && argv[i + 1][0] == '-') {
                std::string flag = argv[++i];
                if (flag == "--prefix") prefix = argv[++i];
                else if (flag == "--seed")
                    try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
                else if (flag == "--creatures")
                    try { creatures = std::stoi(argv[++i]); } catch (...) {}
                else if (flag == "--objects")
                    try { objects = std::stoi(argv[++i]); } catch (...) {}
                else if (flag == "--items")
                    try { items = std::stoi(argv[++i]); } catch (...) {}
                else {
                    std::fprintf(stderr,
                        "gen-random-project: unknown flag '%s'\n", flag.c_str());
                    return 1;
                }
            }
            std::string self = argv[0];
            int produced = 0, failed = 0;
            std::printf("gen-random-project: %d zone(s) with prefix '%s'\n",
                        count, prefix.c_str());
            for (int n = 0; n < count; ++n) {
                // Step outward from (32, 32) in a small raster so the
                // tiles don't coincide. (-1,0,1,...) X (-1,0,1,...).
                int side = 1;
                while ((2 * side + 1) * (2 * side + 1) <= n) side++;
                int idx = n;
                int dx = idx % (2 * side + 1) - side;
                int dy = (idx / (2 * side + 1)) - side;
                int tx = std::max(0, std::min(63, 32 + dx));
                int ty = std::max(0, std::min(63, 32 + dy));
                std::string zoneName = prefix + std::to_string(n + 1);
                std::printf("\n=== %s (tile %d, %d) ===\n",
                            zoneName.c_str(), tx, ty);
                std::fflush(stdout);
                std::string cmd = "\"" + self + "\" --gen-random-zone \"" +
                                   zoneName + "\" " +
                                   std::to_string(tx) + " " + std::to_string(ty) +
                                   " --seed " + std::to_string(seed + n) +
                                   " --creatures " + std::to_string(creatures) +
                                   " --objects " + std::to_string(objects) +
                                   " --items " + std::to_string(items);
                int rc = std::system(cmd.c_str());
                if (rc == 0) produced++;
                else failed++;
            }
            std::printf("\n--- summary ---\n");
            std::printf("  produced : %d\n", produced);
            std::printf("  failed   : %d\n", failed);
            return failed == 0 ? 0 : 1;
        } else if (std::strcmp(argv[i], "--info-zone-audio") == 0 && i + 1 < argc) {
            // Print the audio configuration stored in zone.json:
            // music track, day/night ambience, volume sliders.
            // Useful for spot-checking that the zone has been wired
            // up to the right audio assets before bake/export.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "info-zone-audio: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr,
                    "info-zone-audio: failed to parse %s\n",
                    manifestPath.c_str());
                return 1;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["music"] = zm.musicTrack;
                j["ambienceDay"] = zm.ambienceDay;
                j["ambienceNight"] = zm.ambienceNight;
                j["musicVolume"] = zm.musicVolume;
                j["ambienceVolume"] = zm.ambienceVolume;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone audio: %s\n", zoneDir.c_str());
            std::printf("  music         : %s\n",
                        zm.musicTrack.empty() ? "(none)" : zm.musicTrack.c_str());
            std::printf("  ambience day  : %s\n",
                        zm.ambienceDay.empty() ? "(none)" : zm.ambienceDay.c_str());
            std::printf("  ambience night: %s\n",
                        zm.ambienceNight.empty() ? "(none)" : zm.ambienceNight.c_str());
            std::printf("  music vol     : %.2f\n", zm.musicVolume);
            std::printf("  ambience vol  : %.2f\n", zm.ambienceVolume);
            return 0;
        } else if (std::strcmp(argv[i], "--info-project-audio") == 0 && i + 1 < argc) {
            // Project-wide audio rollup. Walks every zone in
            // <projectDir>, reads the audio fields out of zone.json,
            // emits a table showing which zones have music/ambience
            // configured. Useful for spotting zones still missing
            // audio assignment before a release pass.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "info-project-audio: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            struct Row {
                std::string name;
                std::string music;
                std::string ambDay;
                std::string ambNight;
                float musicVol, ambVol;
            };
            std::vector<Row> rows;
            int withMusic = 0, withAmbience = 0;
            for (const auto& zoneDir : zones) {
                wowee::editor::ZoneManifest zm;
                if (!zm.load(zoneDir + "/zone.json")) continue;
                Row r;
                r.name = fs::path(zoneDir).filename().string();
                r.music = zm.musicTrack;
                r.ambDay = zm.ambienceDay;
                r.ambNight = zm.ambienceNight;
                r.musicVol = zm.musicVolume;
                r.ambVol = zm.ambienceVolume;
                if (!r.music.empty()) withMusic++;
                if (!r.ambDay.empty() || !r.ambNight.empty()) withAmbience++;
                rows.push_back(r);
            }
            if (jsonOut) {
                nlohmann::json j;
                j["project"] = projectDir;
                j["zoneCount"] = zones.size();
                j["withMusic"] = withMusic;
                j["withAmbience"] = withAmbience;
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& r : rows) {
                    arr.push_back({{"name", r.name},
                                    {"music", r.music},
                                    {"ambienceDay", r.ambDay},
                                    {"ambienceNight", r.ambNight},
                                    {"musicVolume", r.musicVol},
                                    {"ambienceVolume", r.ambVol}});
                }
                j["zones"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Project audio: %s\n", projectDir.c_str());
            std::printf("  zones         : %zu\n", zones.size());
            std::printf("  with music    : %d\n", withMusic);
            std::printf("  with ambience : %d\n", withAmbience);
            std::printf("\n  zone                    music?  ambience?  m-vol  a-vol\n");
            auto label = [](const std::string& s) {
                if (s.empty()) return "(none)";
                auto sl = s.rfind('\\');
                if (sl == std::string::npos) sl = s.rfind('/');
                return sl == std::string::npos ? s.c_str()
                                                : s.c_str() + sl + 1;
            };
            for (const auto& r : rows) {
                std::string ambLabel = !r.ambDay.empty() ? r.ambDay :
                                        !r.ambNight.empty() ? r.ambNight : "";
                std::printf("  %-22s  %-6s  %-9s  %5.2f  %5.2f\n",
                            r.name.substr(0, 22).c_str(),
                            r.music.empty() ? "no" : "yes",
                            ambLabel.empty() ? "no" : "yes",
                            r.musicVol, r.ambVol);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--snap-zone-to-ground") == 0 && i + 1 < argc) {
            // Walk every creature + object in a zone and snap their Z
            // to the actual terrain height. Useful after terrain edits
            // or after --random-populate-zone if the spawn baseZ
            // doesn't match the carved terrain.
            //
            // Height lookup walks the loaded WHM tiles and finds the
            // chunk containing each spawn's (x, y), then uses the
            // chunk's average heightmap height + base.
            std::string zoneDir = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "snap-zone-to-ground: %s has no zone.json\n",
                    zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr,
                    "snap-zone-to-ground: failed to parse %s\n",
                    manifestPath.c_str());
                return 1;
            }
            // Load all tiles into a flat map keyed by (tx, ty).
            struct LoadedTile {
                wowee::pipeline::ADTTerrain terrain;
                int tx, ty;
            };
            std::vector<LoadedTile> tiles;
            for (const auto& [tx, ty] : zm.tiles) {
                std::string base = zoneDir + "/" + zm.mapName + "_" +
                                    std::to_string(tx) + "_" + std::to_string(ty);
                if (!wowee::pipeline::WoweeTerrainLoader::exists(base)) continue;
                LoadedTile lt;
                lt.tx = tx; lt.ty = ty;
                if (wowee::pipeline::WoweeTerrainLoader::load(base, lt.terrain)) {
                    tiles.push_back(std::move(lt));
                }
            }
            if (tiles.empty()) {
                std::fprintf(stderr,
                    "snap-zone-to-ground: no .whm tiles loaded\n");
                return 1;
            }
            // Compute terrain height at world (x, y) by finding the
            // chunk that contains it and averaging its heightmap. Each
            // chunk is 33.33y across; chunk position[1]=wowX origin,
            // [0]=wowY origin.
            constexpr float kChunkSize = 33.33333f;
            auto sampleHeight = [&](float wx, float wy) -> float {
                for (const auto& lt : tiles) {
                    for (const auto& chunk : lt.terrain.chunks) {
                        if (!chunk.heightMap.isLoaded()) continue;
                        float cx0 = chunk.position[1];
                        float cy0 = chunk.position[0];
                        if (wx < cx0 || wx >= cx0 + kChunkSize) continue;
                        if (wy < cy0 || wy >= cy0 + kChunkSize) continue;
                        // Use average heightmap height to dodge the
                        // need for full bilinear sampling. Good enough
                        // for spawn placement; finer interpolation is
                        // a future optimization.
                        float sum = 0; int n = 0;
                        for (float h : chunk.heightMap.heights) {
                            if (std::isfinite(h)) { sum += h; n++; }
                        }
                        if (n == 0) return chunk.position[2];
                        return chunk.position[2] + sum / n;
                    }
                }
                return zm.baseHeight;  // outside any loaded chunk
            };
            int snappedC = 0, snappedO = 0;
            // Creatures.
            wowee::editor::NpcSpawner spawner;
            std::string cpath = zoneDir + "/creatures.json";
            if (fs::exists(cpath) && spawner.loadFromFile(cpath)) {
                auto& spawns = spawner.getSpawns();
                for (auto& s : spawns) {
                    s.position.z = sampleHeight(s.position.x, s.position.y);
                    snappedC++;
                }
                if (snappedC > 0) spawner.saveToFile(cpath);
            }
            // Objects.
            wowee::editor::ObjectPlacer placer;
            std::string opath = zoneDir + "/objects.json";
            if (fs::exists(opath) && placer.loadFromFile(opath)) {
                auto& objs = placer.getObjects();
                for (auto& o : objs) {
                    o.position.z = sampleHeight(o.position.x, o.position.y);
                    snappedO++;
                }
                if (snappedO > 0) placer.saveToFile(opath);
            }
            std::printf("snap-zone-to-ground: %s\n", zoneDir.c_str());
            std::printf("  tiles loaded : %zu\n", tiles.size());
            std::printf("  creatures    : %d snapped\n", snappedC);
            std::printf("  objects      : %d snapped\n", snappedO);
            return 0;
        } else if (std::strcmp(argv[i], "--audit-zone-spawns") == 0 && i + 1 < argc) {
            // Non-destructive companion to --snap-zone-to-ground.
            // Loads the zone's terrain, walks every creature + object,
            // and flags any whose Z is more than <threshold> yards
            // off from the sampled terrain height. Useful for
            // surveying placement issues before deciding whether to
            // run --snap-zone-to-ground (which would silently rewrite
            // every spawn).
            std::string zoneDir = argv[++i];
            float threshold = 5.0f;
            if (i + 2 < argc && std::strcmp(argv[i + 1], "--threshold") == 0) {
                try { threshold = std::stof(argv[i + 2]); i += 2; }
                catch (...) {}
            }
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "audit-zone-spawns: %s has no zone.json\n",
                    zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr,
                    "audit-zone-spawns: failed to parse %s\n",
                    manifestPath.c_str());
                return 1;
            }
            // Same chunk-average sampler as --snap-zone-to-ground.
            // Returning baseHeight when no chunk hits = "no terrain
            // data here", so flag those too via the threshold check.
            struct LoadedTile {
                wowee::pipeline::ADTTerrain terrain;
            };
            std::vector<LoadedTile> tiles;
            for (const auto& [tx, ty] : zm.tiles) {
                std::string base = zoneDir + "/" + zm.mapName + "_" +
                                    std::to_string(tx) + "_" + std::to_string(ty);
                if (!wowee::pipeline::WoweeTerrainLoader::exists(base)) continue;
                LoadedTile lt;
                if (wowee::pipeline::WoweeTerrainLoader::load(base, lt.terrain)) {
                    tiles.push_back(std::move(lt));
                }
            }
            constexpr float kChunkSize = 33.33333f;
            auto sampleHeight = [&](float wx, float wy) -> float {
                for (const auto& lt : tiles) {
                    for (const auto& chunk : lt.terrain.chunks) {
                        if (!chunk.heightMap.isLoaded()) continue;
                        float cx0 = chunk.position[1];
                        float cy0 = chunk.position[0];
                        if (wx < cx0 || wx >= cx0 + kChunkSize) continue;
                        if (wy < cy0 || wy >= cy0 + kChunkSize) continue;
                        float sum = 0; int n = 0;
                        for (float h : chunk.heightMap.heights) {
                            if (std::isfinite(h)) { sum += h; n++; }
                        }
                        if (n == 0) return chunk.position[2];
                        return chunk.position[2] + sum / n;
                    }
                }
                return zm.baseHeight;
            };
            struct Issue { std::string kind; std::string name;
                           float spawnZ, terrainZ; };
            std::vector<Issue> issues;
            wowee::editor::NpcSpawner spawner;
            if (fs::exists(zoneDir + "/creatures.json") &&
                spawner.loadFromFile(zoneDir + "/creatures.json")) {
                for (const auto& s : spawner.getSpawns()) {
                    float th = sampleHeight(s.position.x, s.position.y);
                    if (std::fabs(s.position.z - th) > threshold) {
                        issues.push_back({"creature", s.name,
                                          s.position.z, th});
                    }
                }
            }
            wowee::editor::ObjectPlacer placer;
            if (fs::exists(zoneDir + "/objects.json") &&
                placer.loadFromFile(zoneDir + "/objects.json")) {
                for (const auto& o : placer.getObjects()) {
                    float th = sampleHeight(o.position.x, o.position.y);
                    if (std::fabs(o.position.z - th) > threshold) {
                        issues.push_back({"object", o.path,
                                          o.position.z, th});
                    }
                }
            }
            std::printf("audit-zone-spawns: %s\n", zoneDir.c_str());
            std::printf("  threshold    : %.1f yards\n", threshold);
            std::printf("  creatures    : %zu\n", spawner.spawnCount());
            std::printf("  objects      : %zu\n", placer.getObjects().size());
            std::printf("  issues       : %zu\n", issues.size());
            if (issues.empty()) {
                std::printf("\n  PASSED — every spawn is within %.1f y of the terrain\n",
                            threshold);
                return 0;
            }
            std::printf("\n  Flagged spawns (delta = spawnZ - terrainZ):\n");
            std::printf("  kind      delta    spawnZ   terrainZ  name\n");
            for (const auto& iss : issues) {
                float delta = iss.spawnZ - iss.terrainZ;
                std::printf("  %-8s  %+6.1f   %7.1f   %7.1f  %s\n",
                            iss.kind.c_str(), delta, iss.spawnZ,
                            iss.terrainZ,
                            iss.name.substr(0, 40).c_str());
            }
            std::printf("\n  Run --snap-zone-to-ground to fix in bulk.\n");
            return 1;
        } else if (std::strcmp(argv[i], "--list-zone-spawns") == 0 && i + 1 < argc) {
            // Combined creature + object listing. Useful for a quick
            // "what's in this zone" survey without running both
            // --info-creatures and --info-objects separately.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "list-zone-spawns: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::NpcSpawner spawner;
            wowee::editor::ObjectPlacer placer;
            spawner.loadFromFile(zoneDir + "/creatures.json");
            placer.loadFromFile(zoneDir + "/objects.json");
            const auto& spawns = spawner.getSpawns();
            const auto& objs = placer.getObjects();
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["creatureCount"] = spawns.size();
                j["objectCount"] = objs.size();
                nlohmann::json carr = nlohmann::json::array();
                for (const auto& s : spawns) {
                    carr.push_back({{"name", s.name},
                                     {"level", s.level},
                                     {"x", s.position.x},
                                     {"y", s.position.y},
                                     {"z", s.position.z},
                                     {"hostile", s.hostile}});
                }
                j["creatures"] = carr;
                nlohmann::json oarr = nlohmann::json::array();
                for (const auto& o : objs) {
                    oarr.push_back({{"path", o.path},
                                     {"type", o.type == wowee::editor::PlaceableType::M2 ? "m2" : "wmo"},
                                     {"x", o.position.x},
                                     {"y", o.position.y},
                                     {"z", o.position.z},
                                     {"scale", o.scale}});
                }
                j["objects"] = oarr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone spawns: %s\n", zoneDir.c_str());
            std::printf("  creatures : %zu\n", spawns.size());
            std::printf("  objects   : %zu\n", objs.size());
            if (!spawns.empty()) {
                std::printf("\n  Creatures:\n");
                std::printf("    idx  lvl  hostile  x         y         z         name\n");
                for (size_t k = 0; k < spawns.size(); ++k) {
                    const auto& s = spawns[k];
                    std::printf("    %3zu  %3u  %-7s  %8.1f  %8.1f  %8.1f  %s\n",
                                k, s.level, s.hostile ? "yes" : "no",
                                s.position.x, s.position.y, s.position.z,
                                s.name.c_str());
                }
            }
            if (!objs.empty()) {
                std::printf("\n  Objects:\n");
                std::printf("    idx  type  scale  x         y         z         path\n");
                for (size_t k = 0; k < objs.size(); ++k) {
                    const auto& o = objs[k];
                    std::printf("    %3zu  %-4s  %5.2f  %8.1f  %8.1f  %8.1f  %s\n",
                                k,
                                o.type == wowee::editor::PlaceableType::M2 ? "m2" : "wmo",
                                o.scale,
                                o.position.x, o.position.y, o.position.z,
                                o.path.c_str());
                }
            }
            return 0;
        } else if (std::strcmp(argv[i], "--diff-zone-spawns") == 0 && i + 2 < argc) {
            // Compare two zones' creatures + objects. Matches by
            // (kind, name) — paired entries with mismatched positions
            // are reported as "moved" with the delta. Entries that
            // exist in only one zone are added/removed.
            //
            // Useful for "what did the new branch change vs main"
            // before merging, or for confirming a copy-zone-items
            // produced what was expected.
            std::string aDir = argv[++i];
            std::string bDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(aDir + "/zone.json")) {
                std::fprintf(stderr,
                    "diff-zone-spawns: %s has no zone.json\n", aDir.c_str());
                return 1;
            }
            if (!fs::exists(bDir + "/zone.json")) {
                std::fprintf(stderr,
                    "diff-zone-spawns: %s has no zone.json\n", bDir.c_str());
                return 1;
            }
            // Multiset key: kind/name. Position comes along so we can
            // report "moved" deltas when a name appears in both with
            // different XYZ.
            struct Entry { std::string kind, name; glm::vec3 pos; };
            auto load = [&](const std::string& dir) {
                std::vector<Entry> out;
                wowee::editor::NpcSpawner spawner;
                if (spawner.loadFromFile(dir + "/creatures.json")) {
                    for (const auto& s : spawner.getSpawns()) {
                        out.push_back({"creature", s.name, s.position});
                    }
                }
                wowee::editor::ObjectPlacer placer;
                if (placer.loadFromFile(dir + "/objects.json")) {
                    for (const auto& o : placer.getObjects()) {
                        out.push_back({"object", o.path, o.position});
                    }
                }
                return out;
            };
            auto av = load(aDir);
            auto bv = load(bDir);
            // Sort each side for stable key matching.
            auto cmp = [](const Entry& x, const Entry& y) {
                if (x.kind != y.kind) return x.kind < y.kind;
                return x.name < y.name;
            };
            std::sort(av.begin(), av.end(), cmp);
            std::sort(bv.begin(), bv.end(), cmp);
            int added = 0, removed = 0, moved = 0, same = 0;
            std::vector<std::string> diffs;
            // Two-pointer walk: equal keys → check position; A-only →
            // removed; B-only → added.
            size_t i_a = 0, i_b = 0;
            while (i_a < av.size() || i_b < bv.size()) {
                if (i_a < av.size() && i_b < bv.size() &&
                    av[i_a].kind == bv[i_b].kind &&
                    av[i_a].name == bv[i_b].name) {
                    glm::vec3 d = bv[i_b].pos - av[i_a].pos;
                    float dlen = glm::length(d);
                    if (dlen > 0.5f) {
                        char buf[256];
                        std::snprintf(buf, sizeof(buf),
                            "  moved   %-9s %-30s by (%+.1f, %+.1f, %+.1f)",
                            av[i_a].kind.c_str(),
                            av[i_a].name.substr(0, 30).c_str(),
                            d.x, d.y, d.z);
                        diffs.push_back(buf);
                        moved++;
                    } else {
                        same++;
                    }
                    i_a++; i_b++;
                } else if (i_b == bv.size() ||
                           (i_a < av.size() && cmp(av[i_a], bv[i_b]))) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                        "  removed %-9s %s",
                        av[i_a].kind.c_str(),
                        av[i_a].name.substr(0, 60).c_str());
                    diffs.push_back(buf);
                    removed++;
                    i_a++;
                } else {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                        "  added   %-9s %s",
                        bv[i_b].kind.c_str(),
                        bv[i_b].name.substr(0, 60).c_str());
                    diffs.push_back(buf);
                    added++;
                    i_b++;
                }
            }
            std::printf("diff-zone-spawns: %s -> %s\n",
                        aDir.c_str(), bDir.c_str());
            std::printf("  added   : %d\n", added);
            std::printf("  removed : %d\n", removed);
            std::printf("  moved   : %d (>0.5y)\n", moved);
            std::printf("  same    : %d\n", same);
            if (!diffs.empty()) {
                std::printf("\n");
                for (const auto& d : diffs) std::printf("%s\n", d.c_str());
            }
            return (added + removed + moved) == 0 ? 0 : 1;
        } else if (std::strcmp(argv[i], "--info-spawn") == 0 && i + 3 < argc) {
            // Detailed view of one creature or object by index. The
            // list-zone-spawns table only shows headline fields; this
            // dumps every field including AI behavior, faction,
            // patrol path waypoints, etc.
            std::string zoneDir = argv[++i];
            std::string kind = argv[++i];
            int idx = -1;
            try { idx = std::stoi(argv[++i]); }
            catch (...) {
                std::fprintf(stderr,
                    "info-spawn: <index> must be an integer\n");
                return 1;
            }
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            std::transform(kind.begin(), kind.end(), kind.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (kind == "creature") {
                wowee::editor::NpcSpawner spawner;
                if (!spawner.loadFromFile(zoneDir + "/creatures.json")) {
                    std::fprintf(stderr,
                        "info-spawn: %s has no creatures.json\n",
                        zoneDir.c_str());
                    return 1;
                }
                const auto& spawns = spawner.getSpawns();
                if (idx < 0 || static_cast<size_t>(idx) >= spawns.size()) {
                    std::fprintf(stderr,
                        "info-spawn: index %d out of range (have %zu)\n",
                        idx, spawns.size());
                    return 1;
                }
                const auto& s = spawns[idx];
                static const char* behaviors[] = {
                    "Stationary", "Patrol", "Wander", "Scripted"
                };
                int bIdx = static_cast<int>(s.behavior);
                if (bIdx < 0 || bIdx > 3) bIdx = 0;
                if (jsonOut) {
                    nlohmann::json j;
                    j["zone"] = zoneDir;
                    j["kind"] = "creature";
                    j["index"] = idx;
                    j["id"] = s.id;
                    j["name"] = s.name;
                    j["modelPath"] = s.modelPath;
                    j["displayId"] = s.displayId;
                    j["position"] = {s.position.x, s.position.y, s.position.z};
                    j["orientation"] = s.orientation;
                    j["level"] = s.level;
                    j["health"] = s.health;
                    j["mana"] = s.mana;
                    j["faction"] = s.faction;
                    j["scale"] = s.scale;
                    j["behavior"] = behaviors[bIdx];
                    j["wanderRadius"] = s.wanderRadius;
                    j["aggroRadius"] = s.aggroRadius;
                    j["leashRadius"] = s.leashRadius;
                    j["respawnTimeMs"] = s.respawnTimeMs;
                    j["hostile"] = s.hostile;
                    j["questgiver"] = s.questgiver;
                    j["vendor"] = s.vendor;
                    j["trainer"] = s.trainer;
                    j["patrolPathSize"] = s.patrolPath.size();
                    std::printf("%s\n", j.dump(2).c_str());
                    return 0;
                }
                std::printf("Creature spawn %d in %s\n", idx, zoneDir.c_str());
                std::printf("  id            : %u\n", s.id);
                std::printf("  name          : %s\n", s.name.c_str());
                std::printf("  modelPath     : %s\n",
                            s.modelPath.empty() ? "(template)" : s.modelPath.c_str());
                std::printf("  displayId     : %u\n", s.displayId);
                std::printf("  position      : (%.2f, %.2f, %.2f)\n",
                            s.position.x, s.position.y, s.position.z);
                std::printf("  orientation   : %.1f°\n", s.orientation);
                std::printf("  level         : %u\n", s.level);
                std::printf("  health/mana   : %u / %u\n", s.health, s.mana);
                std::printf("  faction       : %u\n", s.faction);
                std::printf("  scale         : %.2f\n", s.scale);
                std::printf("  behavior      : %s\n", behaviors[bIdx]);
                std::printf("  wander/aggro  : %.1f / %.1f y\n",
                            s.wanderRadius, s.aggroRadius);
                std::printf("  leash         : %.1f y\n", s.leashRadius);
                std::printf("  respawn       : %.0f s\n", s.respawnTimeMs / 1000.0f);
                std::printf("  flags         : %s%s%s%s\n",
                            s.hostile ? "hostile " : "",
                            s.questgiver ? "questgiver " : "",
                            s.vendor ? "vendor " : "",
                            s.trainer ? "trainer " : "");
                std::printf("  patrol path   : %zu waypoint(s)\n",
                            s.patrolPath.size());
                return 0;
            } else if (kind == "object") {
                wowee::editor::ObjectPlacer placer;
                if (!placer.loadFromFile(zoneDir + "/objects.json")) {
                    std::fprintf(stderr,
                        "info-spawn: %s has no objects.json\n",
                        zoneDir.c_str());
                    return 1;
                }
                const auto& objs = placer.getObjects();
                if (idx < 0 || static_cast<size_t>(idx) >= objs.size()) {
                    std::fprintf(stderr,
                        "info-spawn: index %d out of range (have %zu)\n",
                        idx, objs.size());
                    return 1;
                }
                const auto& o = objs[idx];
                if (jsonOut) {
                    nlohmann::json j;
                    j["zone"] = zoneDir;
                    j["kind"] = "object";
                    j["index"] = idx;
                    j["uniqueId"] = o.uniqueId;
                    j["path"] = o.path;
                    j["type"] = o.type == wowee::editor::PlaceableType::M2 ? "m2" : "wmo";
                    j["position"] = {o.position.x, o.position.y, o.position.z};
                    j["rotation"] = {o.rotation.x, o.rotation.y, o.rotation.z};
                    j["scale"] = o.scale;
                    std::printf("%s\n", j.dump(2).c_str());
                    return 0;
                }
                std::printf("Object spawn %d in %s\n", idx, zoneDir.c_str());
                std::printf("  uniqueId  : %u\n", o.uniqueId);
                std::printf("  path      : %s\n", o.path.c_str());
                std::printf("  type      : %s\n",
                            o.type == wowee::editor::PlaceableType::M2 ? "m2" : "wmo");
                std::printf("  position  : (%.2f, %.2f, %.2f)\n",
                            o.position.x, o.position.y, o.position.z);
                std::printf("  rotation  : (%.2f, %.2f, %.2f) rad\n",
                            o.rotation.x, o.rotation.y, o.rotation.z);
                std::printf("  scale     : %.2f\n", o.scale);
                return 0;
            }
            std::fprintf(stderr,
                "info-spawn: kind must be 'creature' or 'object' (got '%s')\n",
                kind.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--list-project-spawns") == 0 && i + 1 < argc) {
            // Project-wide companion to --list-zone-spawns. Combines
            // creatures + objects across every zone into one big
            // listing keyed by (zone, kind, name). Useful for project-
            // wide review and for piping into spreadsheets via --json.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "list-project-spawns: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            int totalCreat = 0, totalObj = 0;
            struct Row {
                std::string zone, kind, name;
                float x, y, z;
                std::string extra;
            };
            std::vector<Row> rows;
            for (const auto& zoneDir : zones) {
                std::string zname = fs::path(zoneDir).filename().string();
                wowee::editor::NpcSpawner spawner;
                if (spawner.loadFromFile(zoneDir + "/creatures.json")) {
                    for (const auto& s : spawner.getSpawns()) {
                        Row r;
                        r.zone = zname;
                        r.kind = "creature";
                        r.name = s.name;
                        r.x = s.position.x; r.y = s.position.y;
                        r.z = s.position.z;
                        r.extra = "lvl " + std::to_string(s.level);
                        rows.push_back(r);
                        totalCreat++;
                    }
                }
                wowee::editor::ObjectPlacer placer;
                if (placer.loadFromFile(zoneDir + "/objects.json")) {
                    for (const auto& o : placer.getObjects()) {
                        Row r;
                        r.zone = zname;
                        r.kind = "object";
                        r.name = o.path;
                        r.x = o.position.x; r.y = o.position.y;
                        r.z = o.position.z;
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "scale %.2f", o.scale);
                        r.extra = buf;
                        rows.push_back(r);
                        totalObj++;
                    }
                }
            }
            if (jsonOut) {
                nlohmann::json j;
                j["project"] = projectDir;
                j["zoneCount"] = zones.size();
                j["creatureCount"] = totalCreat;
                j["objectCount"] = totalObj;
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& r : rows) {
                    arr.push_back({{"zone", r.zone},
                                    {"kind", r.kind},
                                    {"name", r.name},
                                    {"x", r.x}, {"y", r.y}, {"z", r.z},
                                    {"extra", r.extra}});
                }
                j["spawns"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Project spawns: %s\n", projectDir.c_str());
            std::printf("  zones      : %zu\n", zones.size());
            std::printf("  creatures  : %d\n", totalCreat);
            std::printf("  objects    : %d\n", totalObj);
            if (rows.empty()) {
                std::printf("\n  *no spawns in any zone*\n");
                return 0;
            }
            std::printf("\n  zone                  kind      x         y         z         info       name\n");
            for (const auto& r : rows) {
                std::printf("  %-20s  %-8s  %8.1f  %8.1f  %8.1f  %-10s %s\n",
                            r.zone.substr(0, 20).c_str(),
                            r.kind.c_str(),
                            r.x, r.y, r.z,
                            r.extra.c_str(),
                            r.name.substr(0, 60).c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--audit-project-spawns") == 0 && i + 1 < argc) {
            // Project-wide wrapper around --audit-zone-spawns. Spawns
            // the binary per-zone (only those with creatures.json or
            // objects.json), aggregates how many issues each zone has,
            // and exits 1 if any zone reports problems. CI-friendly
            // pre-release placement check.
            std::string projectDir = argv[++i];
            std::string thresholdArg;
            if (i + 2 < argc && std::strcmp(argv[i + 1], "--threshold") == 0) {
                thresholdArg = argv[i + 2];
                i += 2;
            }
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "audit-project-spawns: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                bool hasContent = fs::exists(entry.path() / "creatures.json") ||
                                   fs::exists(entry.path() / "objects.json");
                if (!hasContent) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            if (zones.empty()) {
                std::printf("audit-project-spawns: %s\n", projectDir.c_str());
                std::printf("  no zones with creatures.json or objects.json\n");
                return 0;
            }
            std::string self = argv[0];
            int passed = 0, failed = 0;
            std::printf("audit-project-spawns: %s\n", projectDir.c_str());
            std::printf("  zones to audit : %zu\n", zones.size());
            if (!thresholdArg.empty()) {
                std::printf("  threshold      : %s yards\n", thresholdArg.c_str());
            }
            std::printf("\n");
            for (const auto& zoneDir : zones) {
                std::printf("--- %s ---\n",
                            fs::path(zoneDir).filename().string().c_str());
                std::fflush(stdout);
                std::string cmd = "\"" + self + "\" --audit-zone-spawns \"" +
                                   zoneDir + "\"";
                if (!thresholdArg.empty()) {
                    cmd += " --threshold " + thresholdArg;
                }
                int rc = std::system(cmd.c_str());
                if (rc == 0) passed++;
                else failed++;
            }
            std::printf("\n--- summary ---\n");
            std::printf("  passed : %d\n", passed);
            std::printf("  failed : %d\n", failed);
            if (failed == 0) {
                std::printf("\n  ALL ZONES PASSED\n");
                return 0;
            }
            std::printf("\n  Run --snap-project-to-ground to fix in bulk.\n");
            return 1;
        } else if (std::strcmp(argv[i], "--snap-project-to-ground") == 0 && i + 1 < argc) {
            // Orchestrator wrapper around --snap-zone-to-ground. Spawns
            // the binary per-zone (only zones with at least one of
            // creatures.json or objects.json since pure-terrain zones
            // have nothing to snap), aggregates a final summary.
            std::string projectDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "snap-project-to-ground: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                bool hasContent = fs::exists(entry.path() / "creatures.json") ||
                                   fs::exists(entry.path() / "objects.json");
                if (!hasContent) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            if (zones.empty()) {
                std::printf("snap-project-to-ground: %s\n", projectDir.c_str());
                std::printf("  no zones with creatures.json or objects.json\n");
                return 0;
            }
            std::string self = argv[0];
            int passed = 0, failed = 0;
            std::printf("snap-project-to-ground: %s\n", projectDir.c_str());
            std::printf("  zones to snap : %zu\n\n", zones.size());
            for (const auto& zoneDir : zones) {
                std::printf("--- %s ---\n",
                            fs::path(zoneDir).filename().string().c_str());
                std::fflush(stdout);
                std::string cmd = "\"" + self + "\" --snap-zone-to-ground \"" +
                                   zoneDir + "\"";
                int rc = std::system(cmd.c_str());
                if (rc == 0) passed++;
                else failed++;
            }
            std::printf("\n--- summary ---\n");
            std::printf("  zones snapped : %d\n", passed);
            std::printf("  failed        : %d\n", failed);
            return failed == 0 ? 0 : 1;
        } else if (std::strcmp(argv[i], "--list-items") == 0 && i + 1 < argc) {
            // Inspect <zoneDir>/items.json. Pretty-prints id / quality
            // / item level / display id / name as a table; also
            // supports --json for machine-readable output.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            std::string path = zoneDir + "/items.json";
            if (!fs::exists(path)) {
                std::fprintf(stderr,
                    "list-items: %s has no items.json\n", zoneDir.c_str());
                return 1;
            }
            nlohmann::json doc;
            try {
                std::ifstream in(path);
                in >> doc;
            } catch (...) {
                std::fprintf(stderr,
                    "list-items: %s is not valid JSON\n", path.c_str());
                return 1;
            }
            if (!doc.contains("items") || !doc["items"].is_array()) {
                std::fprintf(stderr,
                    "list-items: %s has no 'items' array\n", path.c_str());
                return 1;
            }
            const auto& items = doc["items"];
            if (jsonOut) {
                std::printf("%s\n", items.dump(2).c_str());
                return 0;
            }
            static const char* qualityNames[] = {
                "poor", "common", "uncommon", "rare", "epic",
                "legendary", "artifact"
            };
            std::printf("Zone items: %s\n", path.c_str());
            std::printf("  count : %zu\n\n", items.size());
            if (items.empty()) {
                std::printf("  *no items*\n");
                return 0;
            }
            std::printf("  idx   id     ilvl   stack   quality      displayId   name\n");
            for (size_t k = 0; k < items.size(); ++k) {
                const auto& it = items[k];
                uint32_t id = it.value("id", 0u);
                uint32_t quality = it.value("quality", 1u);
                uint32_t ilvl = it.value("itemLevel", 1u);
                uint32_t displayId = it.value("displayId", 0u);
                uint32_t stack = it.value("stackable", 1u);
                std::string name = it.value("name", std::string());
                if (quality > 6) quality = 0;
                std::printf("  %3zu   %5u   %4u   %5u   %-10s   %9u   %s\n",
                            k, id, ilvl, stack,
                            qualityNames[quality], displayId, name.c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-item") == 0 && i + 2 < argc) {
            // Single-item detail view. Lookup is by id by default;
            // prefix the argument with '#' (e.g., "#3") to look up by
            // 0-based array index instead. Useful for inspecting all
            // fields of a single record without sifting through the
            // full --list-items table.
            std::string zoneDir = argv[++i];
            std::string lookup = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            std::string path = zoneDir + "/items.json";
            if (!fs::exists(path)) {
                std::fprintf(stderr,
                    "info-item: %s has no items.json\n", zoneDir.c_str());
                return 1;
            }
            nlohmann::json doc;
            try {
                std::ifstream in(path);
                in >> doc;
            } catch (...) {
                std::fprintf(stderr,
                    "info-item: %s is not valid JSON\n", path.c_str());
                return 1;
            }
            if (!doc.contains("items") || !doc["items"].is_array()) {
                std::fprintf(stderr,
                    "info-item: %s has no 'items' array\n", path.c_str());
                return 1;
            }
            const auto& items = doc["items"];
            int foundIdx = -1;
            if (!lookup.empty() && lookup[0] == '#') {
                try {
                    int idx = std::stoi(lookup.substr(1));
                    if (idx >= 0 && static_cast<size_t>(idx) < items.size())
                        foundIdx = idx;
                } catch (...) {}
            } else {
                uint32_t targetId = 0;
                try { targetId = static_cast<uint32_t>(std::stoul(lookup)); }
                catch (...) {
                    std::fprintf(stderr,
                        "info-item: lookup '%s' is not a number "
                        "(use '#N' for index lookup)\n", lookup.c_str());
                    return 1;
                }
                for (size_t k = 0; k < items.size(); ++k) {
                    if (items[k].contains("id") &&
                        items[k]["id"].is_number_unsigned() &&
                        items[k]["id"].get<uint32_t>() == targetId) {
                        foundIdx = static_cast<int>(k);
                        break;
                    }
                }
            }
            if (foundIdx < 0) {
                std::fprintf(stderr,
                    "info-item: no match for '%s' in %s\n",
                    lookup.c_str(), path.c_str());
                return 1;
            }
            const auto& it = items[foundIdx];
            if (jsonOut) {
                std::printf("%s\n", it.dump(2).c_str());
                return 0;
            }
            static const char* qualityNames[] = {
                "poor", "common", "uncommon", "rare", "epic",
                "legendary", "artifact"
            };
            uint32_t quality = it.value("quality", 1u);
            if (quality > 6) quality = 0;
            std::printf("Item %d in %s\n", foundIdx, path.c_str());
            std::printf("  id          : %u\n", it.value("id", 0u));
            std::printf("  name        : %s\n",
                        it.value("name", std::string("(unnamed)")).c_str());
            std::printf("  quality     : %u (%s)\n",
                        quality, qualityNames[quality]);
            std::printf("  itemLevel   : %u\n", it.value("itemLevel", 1u));
            std::printf("  displayId   : %u\n", it.value("displayId", 0u));
            std::printf("  stackable   : %u\n", it.value("stackable", 1u));
            // Surface any extra fields the user added by hand so
            // info-item stays useful as the schema evolves.
            std::vector<std::string> extras;
            for (auto& [k, v] : it.items()) {
                if (k == "id" || k == "name" || k == "quality" ||
                    k == "itemLevel" || k == "displayId" ||
                    k == "stackable") continue;
                extras.push_back(k);
            }
            if (!extras.empty()) {
                std::printf("\n  Extra fields:\n");
                for (const auto& k : extras) {
                    std::printf("    %s = %s\n",
                                k.c_str(), it[k].dump().c_str());
                }
            }
            return 0;
        } else if (std::strcmp(argv[i], "--set-item") == 0 && i + 2 < argc) {
            // Edit fields on an existing item in place. Lookup is by
            // id by default; '#N' for index lookup. Only specified
            // flags are changed; everything else is preserved
            // verbatim — including any extra fields added by hand.
            //
            // Supported flags: --name, --quality, --displayId,
            // --itemLevel, --stackable. Each takes one positional
            // argument that follows the flag.
            std::string zoneDir = argv[++i];
            std::string lookup = argv[++i];
            namespace fs = std::filesystem;
            std::string path = zoneDir + "/items.json";
            if (!fs::exists(path)) {
                std::fprintf(stderr,
                    "set-item: %s has no items.json\n", zoneDir.c_str());
                return 1;
            }
            nlohmann::json doc;
            try {
                std::ifstream in(path);
                in >> doc;
            } catch (...) {
                std::fprintf(stderr,
                    "set-item: %s is not valid JSON\n", path.c_str());
                return 1;
            }
            if (!doc.contains("items") || !doc["items"].is_array()) {
                std::fprintf(stderr,
                    "set-item: %s has no 'items' array\n", path.c_str());
                return 1;
            }
            auto& items = doc["items"];
            int foundIdx = -1;
            if (!lookup.empty() && lookup[0] == '#') {
                try {
                    int idx = std::stoi(lookup.substr(1));
                    if (idx >= 0 && static_cast<size_t>(idx) < items.size())
                        foundIdx = idx;
                } catch (...) {}
            } else {
                uint32_t targetId = 0;
                try { targetId = static_cast<uint32_t>(std::stoul(lookup)); }
                catch (...) {
                    std::fprintf(stderr,
                        "set-item: lookup '%s' is not a number\n",
                        lookup.c_str());
                    return 1;
                }
                for (size_t k = 0; k < items.size(); ++k) {
                    if (items[k].contains("id") &&
                        items[k]["id"].is_number_unsigned() &&
                        items[k]["id"].get<uint32_t>() == targetId) {
                        foundIdx = static_cast<int>(k);
                        break;
                    }
                }
            }
            if (foundIdx < 0) {
                std::fprintf(stderr,
                    "set-item: no match for '%s' in %s\n",
                    lookup.c_str(), path.c_str());
                return 1;
            }
            auto& it = items[foundIdx];
            std::vector<std::string> changes;
            // Walk the remaining args looking for known --field value
            // pairs. Anything unrecognized is reported and aborts so
            // typos don't silently no-op.
            while (i + 2 < argc) {
                std::string flag = argv[i + 1];
                std::string val = argv[i + 2];
                if (flag.size() < 2 || flag[0] != '-' || flag[1] != '-') break;
                if (flag == "--name") {
                    it["name"] = val;
                    changes.push_back("name=" + val);
                } else if (flag == "--quality") {
                    try {
                        uint32_t q = static_cast<uint32_t>(std::stoul(val));
                        if (q > 6) {
                            std::fprintf(stderr,
                                "set-item: quality %u out of range (0..6)\n", q);
                            return 1;
                        }
                        it["quality"] = q;
                        changes.push_back("quality=" + val);
                    } catch (...) {
                        std::fprintf(stderr,
                            "set-item: --quality needs a number\n");
                        return 1;
                    }
                } else if (flag == "--displayId") {
                    try {
                        it["displayId"] = static_cast<uint32_t>(std::stoul(val));
                        changes.push_back("displayId=" + val);
                    } catch (...) {
                        std::fprintf(stderr,
                            "set-item: --displayId needs a number\n");
                        return 1;
                    }
                } else if (flag == "--itemLevel") {
                    try {
                        it["itemLevel"] = static_cast<uint32_t>(std::stoul(val));
                        changes.push_back("itemLevel=" + val);
                    } catch (...) {
                        std::fprintf(stderr,
                            "set-item: --itemLevel needs a number\n");
                        return 1;
                    }
                } else if (flag == "--stackable") {
                    try {
                        uint32_t s = static_cast<uint32_t>(std::stoul(val));
                        if (s == 0 || s > 1000) {
                            std::fprintf(stderr,
                                "set-item: stackable %u out of range (1..1000)\n", s);
                            return 1;
                        }
                        it["stackable"] = s;
                        changes.push_back("stackable=" + val);
                    } catch (...) {
                        std::fprintf(stderr,
                            "set-item: --stackable needs a number\n");
                        return 1;
                    }
                } else {
                    std::fprintf(stderr,
                        "set-item: unknown flag '%s' (typo?)\n", flag.c_str());
                    return 1;
                }
                i += 2;
            }
            if (changes.empty()) {
                std::fprintf(stderr,
                    "set-item: no field flags supplied — nothing to change\n");
                return 1;
            }
            std::ofstream out(path);
            if (!out) {
                std::fprintf(stderr,
                    "set-item: failed to write %s\n", path.c_str());
                return 1;
            }
            out << doc.dump(2);
            out.close();
            std::printf("Updated item %d in %s:\n", foundIdx, path.c_str());
            for (const auto& c : changes) {
                std::printf("  %s\n", c.c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--export-zone-items-md") == 0 && i + 1 < argc) {
            // Render items.json as a Markdown table grouped by
            // quality. Useful for design docs, PR descriptions, and
            // GitHub Pages — one rendered page communicates the loot
            // landscape better than scrolling through JSON.
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            std::string path = zoneDir + "/items.json";
            if (!fs::exists(path)) {
                std::fprintf(stderr,
                    "export-zone-items-md: %s has no items.json\n",
                    zoneDir.c_str());
                return 1;
            }
            nlohmann::json doc;
            try {
                std::ifstream in(path);
                in >> doc;
            } catch (...) {
                std::fprintf(stderr,
                    "export-zone-items-md: %s is not valid JSON\n",
                    path.c_str());
                return 1;
            }
            if (!doc.contains("items") || !doc["items"].is_array()) {
                std::fprintf(stderr,
                    "export-zone-items-md: %s has no 'items' array\n",
                    path.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/ITEMS.md";
            const auto& items = doc["items"];
            static const char* qualityNames[] = {
                "Poor", "Common", "Uncommon", "Rare", "Epic",
                "Legendary", "Artifact"
            };
            // Bucket by quality so the report reads top-down from
            // best loot to filler. Reverse iteration over the buckets.
            std::map<int, std::vector<size_t>> byQuality;
            for (size_t k = 0; k < items.size(); ++k) {
                uint32_t q = items[k].value("quality", 1u);
                if (q > 6) q = 0;
                byQuality[q].push_back(k);
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "export-zone-items-md: cannot write %s\n", outPath.c_str());
                return 1;
            }
            std::string zoneName = fs::path(zoneDir).filename().string();
            out << "# Items: " << zoneName << "\n\n";
            out << "Source: `" << path << "`  \n";
            out << "Total items: **" << items.size() << "**\n\n";
            // Quality histogram up top.
            out << "## Quality breakdown\n\n";
            out << "| Quality | Count |\n|---|---:|\n";
            for (int q = 6; q >= 0; --q) {
                auto it = byQuality.find(q);
                if (it == byQuality.end()) continue;
                out << "| " << qualityNames[q] << " | "
                    << it->second.size() << " |\n";
            }
            out << "\n";
            // Per-quality sections, best first.
            for (int q = 6; q >= 0; --q) {
                auto qit = byQuality.find(q);
                if (qit == byQuality.end()) continue;
                out << "## " << qualityNames[q] << "\n\n";
                out << "| ID | Name | iLvl | Display | Stack |\n";
                out << "|---:|---|---:|---:|---:|\n";
                for (size_t k : qit->second) {
                    const auto& it = items[k];
                    std::string name = it.value("name", std::string("(unnamed)"));
                    out << "| " << it.value("id", 0u) << " | "
                        << name << " | "
                        << it.value("itemLevel", 1u) << " | "
                        << it.value("displayId", 0u) << " | "
                        << it.value("stackable", 1u) << " |\n";
                }
                out << "\n";
            }
            out.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  total items : %zu\n", items.size());
            std::printf("  qualities   : %zu (used)\n", byQuality.size());
            return 0;
        } else if (std::strcmp(argv[i], "--export-project-items-md") == 0 && i + 1 < argc) {
            // Project-wide items markdown. Walks every zone in
            // <projectDir> and emits one document with: project-wide
            // header + total + quality histogram, then per-zone
            // sections each containing a table (ID/name/quality/
            // ilvl/displayId/stack). Easier to scan than running
            // --export-zone-items-md N times.
            std::string projectDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "export-project-items-md: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = projectDir + "/ITEMS.md";
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                if (!fs::exists(entry.path() / "items.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            static const char* qualityNames[] = {
                "Poor", "Common", "Uncommon", "Rare", "Epic",
                "Legendary", "Artifact"
            };
            int totalItems = 0;
            std::map<int, int> globalQ;
            // Per-zone collected items so we don't have to re-read
            // each items.json twice.
            struct ZItems {
                std::string name;
                nlohmann::json items;
            };
            std::vector<ZItems> zoneItems;
            for (const auto& zoneDir : zones) {
                std::string ipath = zoneDir + "/items.json";
                nlohmann::json doc;
                try {
                    std::ifstream in(ipath);
                    in >> doc;
                } catch (...) { continue; }
                if (!doc.contains("items") || !doc["items"].is_array()) continue;
                ZItems z;
                z.name = fs::path(zoneDir).filename().string();
                z.items = doc["items"];
                for (const auto& it : z.items) {
                    int q = static_cast<int>(it.value("quality", 1u));
                    if (q < 0 || q > 6) q = 0;
                    globalQ[q]++;
                    totalItems++;
                }
                zoneItems.push_back(std::move(z));
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "export-project-items-md: cannot write %s\n",
                    outPath.c_str());
                return 1;
            }
            out << "# Project Items: "
                << fs::path(projectDir).filename().string() << "\n\n";
            out << "Source: `" << projectDir << "`  \n";
            out << "Zones with items: **" << zoneItems.size() << "**  \n";
            out << "Total items: **" << totalItems << "**\n\n";
            out << "## Project quality breakdown\n\n";
            out << "| Quality | Count |\n|---|---:|\n";
            for (int q = 6; q >= 0; --q) {
                auto it = globalQ.find(q);
                if (it == globalQ.end()) continue;
                out << "| " << qualityNames[q] << " | "
                    << it->second << " |\n";
            }
            out << "\n";
            for (const auto& z : zoneItems) {
                out << "## Zone: " << z.name << "\n\n";
                out << "Items: **" << z.items.size() << "**\n\n";
                out << "| ID | Name | Quality | iLvl | Display | Stack |\n";
                out << "|---:|---|---|---:|---:|---:|\n";
                for (const auto& it : z.items) {
                    int q = static_cast<int>(it.value("quality", 1u));
                    if (q < 0 || q > 6) q = 0;
                    std::string name = it.value("name", std::string("(unnamed)"));
                    out << "| " << it.value("id", 0u) << " | "
                        << name << " | "
                        << qualityNames[q] << " | "
                        << it.value("itemLevel", 1u) << " | "
                        << it.value("displayId", 0u) << " | "
                        << it.value("stackable", 1u) << " |\n";
                }
                out << "\n";
            }
            out.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  zones with items : %zu\n", zoneItems.size());
            std::printf("  total items      : %d\n", totalItems);
            return 0;
        } else if (std::strcmp(argv[i], "--export-project-items-csv") == 0 && i + 1 < argc) {
            // Single CSV with every item across every zone. The
            // zone name is the first column so a pivot table can
            // group by it; everything else mirrors --export-zone-csv
            // items columns. Saves running the per-zone CSV exporter
            // N times and concatenating manually.
            std::string projectDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "export-project-items-csv: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = projectDir + "/items.csv";
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                if (!fs::exists(entry.path() / "items.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            // CSV-escape the same way --export-zone-csv does.
            auto csvEsc = [](const std::string& s) {
                bool needs = s.find(',') != std::string::npos ||
                             s.find('"') != std::string::npos ||
                             s.find('\n') != std::string::npos;
                if (!needs) return s;
                std::string out = "\"";
                for (char c : s) {
                    if (c == '"') out += "\"\"";
                    else out += c;
                }
                out += "\"";
                return out;
            };
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "export-project-items-csv: cannot write %s\n",
                    outPath.c_str());
                return 1;
            }
            out << "zone,index,id,name,quality,itemLevel,displayId,stackable\n";
            int totalRows = 0;
            for (const auto& zoneDir : zones) {
                std::string zoneName = fs::path(zoneDir).filename().string();
                std::string ipath = zoneDir + "/items.json";
                nlohmann::json doc;
                try {
                    std::ifstream in(ipath);
                    in >> doc;
                } catch (...) { continue; }
                if (!doc.contains("items") || !doc["items"].is_array()) continue;
                const auto& items = doc["items"];
                for (size_t k = 0; k < items.size(); ++k) {
                    const auto& it = items[k];
                    out << csvEsc(zoneName) << "," << k << ","
                        << it.value("id", 0u) << ","
                        << csvEsc(it.value("name", std::string())) << ","
                        << it.value("quality", 1u) << ","
                        << it.value("itemLevel", 1u) << ","
                        << it.value("displayId", 0u) << ","
                        << it.value("stackable", 1u) << "\n";
                    totalRows++;
                }
            }
            out.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  zones with items : %zu\n", zones.size());
            std::printf("  rows             : %d\n", totalRows);
            return 0;
        } else if (std::strcmp(argv[i], "--remove-item") == 0 && i + 2 < argc) {
            // Remove the item at given 0-based index from <zoneDir>/
            // items.json. Mirrors --remove-creature/--remove-object/
            // --remove-quest semantics — bounds-checked, file rewrites
            // on success, exit 1 on out-of-range.
            std::string zoneDir = argv[++i];
            int idx = -1;
            try { idx = std::stoi(argv[++i]); }
            catch (...) {
                std::fprintf(stderr,
                    "remove-item: index must be an integer\n");
                return 1;
            }
            namespace fs = std::filesystem;
            std::string path = zoneDir + "/items.json";
            if (!fs::exists(path)) {
                std::fprintf(stderr,
                    "remove-item: %s has no items.json\n", zoneDir.c_str());
                return 1;
            }
            nlohmann::json doc;
            try {
                std::ifstream in(path);
                in >> doc;
            } catch (...) {
                std::fprintf(stderr,
                    "remove-item: %s is not valid JSON\n", path.c_str());
                return 1;
            }
            if (!doc.contains("items") || !doc["items"].is_array()) {
                std::fprintf(stderr,
                    "remove-item: %s has no 'items' array\n", path.c_str());
                return 1;
            }
            auto& items = doc["items"];
            if (idx < 0 || static_cast<size_t>(idx) >= items.size()) {
                std::fprintf(stderr,
                    "remove-item: index %d out of range (have %zu)\n",
                    idx, items.size());
                return 1;
            }
            std::string removedName = items[idx].value("name", std::string("(unnamed)"));
            uint32_t removedId = items[idx].value("id", 0u);
            items.erase(items.begin() + idx);
            std::ofstream out(path);
            if (!out) {
                std::fprintf(stderr,
                    "remove-item: failed to write %s\n", path.c_str());
                return 1;
            }
            out << doc.dump(2);
            out.close();
            std::printf("Removed item '%s' (id=%u) from %s (now %zu total)\n",
                        removedName.c_str(), removedId,
                        path.c_str(), items.size());
            return 0;
        } else if (std::strcmp(argv[i], "--copy-zone-items") == 0 && i + 2 < argc) {
            // Copy items from one zone to another. Default mode
            // replaces the destination items.json wholesale; --merge
            // appends each source item to the existing destination
            // list, re-id'ing on collision so the destination's
            // existing IDs are preserved and the source's new
            // entries get fresh ones.
            std::string fromZone = argv[++i];
            std::string toZone = argv[++i];
            bool mergeMode = false;
            if (i + 1 < argc && std::strcmp(argv[i + 1], "--merge") == 0) {
                mergeMode = true; i++;
            }
            namespace fs = std::filesystem;
            std::string srcPath = fromZone + "/items.json";
            if (!fs::exists(srcPath)) {
                std::fprintf(stderr,
                    "copy-zone-items: %s has no items.json\n", fromZone.c_str());
                return 1;
            }
            if (!fs::exists(toZone) || !fs::is_directory(toZone)) {
                std::fprintf(stderr,
                    "copy-zone-items: dest %s is not a directory\n",
                    toZone.c_str());
                return 1;
            }
            nlohmann::json src;
            try {
                std::ifstream in(srcPath);
                in >> src;
            } catch (...) {
                std::fprintf(stderr,
                    "copy-zone-items: %s is not valid JSON\n", srcPath.c_str());
                return 1;
            }
            if (!src.contains("items") || !src["items"].is_array()) {
                std::fprintf(stderr,
                    "copy-zone-items: %s has no 'items' array\n",
                    srcPath.c_str());
                return 1;
            }
            std::string dstPath = toZone + "/items.json";
            nlohmann::json dst = nlohmann::json::object({{"items",
                                  nlohmann::json::array()}});
            int copied = 0, reIded = 0;
            if (mergeMode && fs::exists(dstPath)) {
                try {
                    std::ifstream in(dstPath);
                    in >> dst;
                } catch (...) {}
                if (!dst.contains("items") || !dst["items"].is_array()) {
                    dst["items"] = nlohmann::json::array();
                }
                std::set<uint32_t> usedIds;
                for (const auto& it : dst["items"]) {
                    if (it.contains("id") && it["id"].is_number_unsigned()) {
                        usedIds.insert(it["id"].get<uint32_t>());
                    }
                }
                for (const auto& it : src["items"]) {
                    nlohmann::json newItem = it;
                    uint32_t srcId = it.value("id", 0u);
                    if (srcId == 0 || usedIds.count(srcId)) {
                        // Pick the next free id.
                        uint32_t fresh = 1;
                        while (usedIds.count(fresh)) ++fresh;
                        newItem["id"] = fresh;
                        usedIds.insert(fresh);
                        if (srcId != 0) reIded++;
                    } else {
                        usedIds.insert(srcId);
                    }
                    dst["items"].push_back(newItem);
                    copied++;
                }
            } else {
                // Replace mode: destination becomes a verbatim copy of
                // the source items array.
                dst["items"] = src["items"];
                copied = static_cast<int>(src["items"].size());
            }
            std::ofstream out(dstPath);
            if (!out) {
                std::fprintf(stderr,
                    "copy-zone-items: failed to write %s\n", dstPath.c_str());
                return 1;
            }
            out << dst.dump(2);
            out.close();
            std::printf("Copied %d item(s) from %s to %s\n",
                        copied, fromZone.c_str(), toZone.c_str());
            std::printf("  mode      : %s\n",
                        mergeMode ? "merge (append + re-id)" : "replace");
            std::printf("  dst total : %zu\n", dst["items"].size());
            if (reIded > 0) {
                std::printf("  re-ided   : %d (id collisions)\n", reIded);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--clone-item") == 0 && i + 2 < argc) {
            // Duplicate the item at given 0-based index. Auto-assigns
            // the smallest unused positive id; optional <newName>
            // overrides the cloned name (without it the new entry
            // gets " (copy)" appended).
            std::string zoneDir = argv[++i];
            int idx = -1;
            try { idx = std::stoi(argv[++i]); }
            catch (...) {
                std::fprintf(stderr,
                    "clone-item: index must be an integer\n");
                return 1;
            }
            std::string newName;
            if (i + 1 < argc && argv[i + 1][0] != '-') newName = argv[++i];
            namespace fs = std::filesystem;
            std::string path = zoneDir + "/items.json";
            if (!fs::exists(path)) {
                std::fprintf(stderr,
                    "clone-item: %s has no items.json\n", zoneDir.c_str());
                return 1;
            }
            nlohmann::json doc;
            try {
                std::ifstream in(path);
                in >> doc;
            } catch (...) {
                std::fprintf(stderr,
                    "clone-item: %s is not valid JSON\n", path.c_str());
                return 1;
            }
            if (!doc.contains("items") || !doc["items"].is_array()) {
                std::fprintf(stderr,
                    "clone-item: %s has no 'items' array\n", path.c_str());
                return 1;
            }
            auto& items = doc["items"];
            if (idx < 0 || static_cast<size_t>(idx) >= items.size()) {
                std::fprintf(stderr,
                    "clone-item: index %d out of range (have %zu)\n",
                    idx, items.size());
                return 1;
            }
            // Pick the next free id.
            std::set<uint32_t> used;
            for (const auto& it : items) {
                if (it.contains("id") && it["id"].is_number_unsigned()) {
                    used.insert(it["id"].get<uint32_t>());
                }
            }
            uint32_t newId = 1;
            while (used.count(newId)) ++newId;
            nlohmann::json clone = items[idx];
            clone["id"] = newId;
            if (!newName.empty()) {
                clone["name"] = newName;
            } else {
                std::string oldName = clone.value("name", std::string("(unnamed)"));
                clone["name"] = oldName + " (copy)";
            }
            items.push_back(clone);
            std::ofstream out(path);
            if (!out) {
                std::fprintf(stderr,
                    "clone-item: failed to write %s\n", path.c_str());
                return 1;
            }
            out << doc.dump(2);
            out.close();
            std::printf("Cloned item idx %d to '%s' (id=%u) in %s (now %zu total)\n",
                        idx, clone["name"].get<std::string>().c_str(),
                        newId, path.c_str(), items.size());
            return 0;
        } else if (std::strcmp(argv[i], "--validate-items") == 0 && i + 1 < argc) {
            // Schema validator for items.json. Catches what
            // --add-item / --clone-item only enforce on insertion
            // (e.g., duplicate ids if the file was hand-edited),
            // plus general field-range issues. Exit 1 if any error.
            std::string zoneDir = argv[++i];
            namespace fs = std::filesystem;
            std::string path = zoneDir + "/items.json";
            if (!fs::exists(path)) {
                std::fprintf(stderr,
                    "validate-items: %s has no items.json\n", zoneDir.c_str());
                return 1;
            }
            nlohmann::json doc;
            try {
                std::ifstream in(path);
                in >> doc;
            } catch (...) {
                std::fprintf(stderr,
                    "validate-items: %s is not valid JSON\n", path.c_str());
                return 1;
            }
            if (!doc.contains("items") || !doc["items"].is_array()) {
                std::fprintf(stderr,
                    "validate-items: %s has no 'items' array\n", path.c_str());
                return 1;
            }
            const auto& items = doc["items"];
            std::vector<std::string> errors;
            std::map<uint32_t, std::vector<size_t>> idIndices;  // id -> [item indices]
            for (size_t k = 0; k < items.size(); ++k) {
                const auto& it = items[k];
                if (!it.is_object()) {
                    errors.push_back("item " + std::to_string(k) +
                                      ": not a JSON object");
                    continue;
                }
                if (!it.contains("id") || !it["id"].is_number_unsigned() ||
                    it["id"].get<uint32_t>() == 0) {
                    errors.push_back("item " + std::to_string(k) +
                                      ": missing/invalid 'id' (must be positive uint)");
                } else {
                    idIndices[it["id"].get<uint32_t>()].push_back(k);
                }
                if (!it.contains("name") || !it["name"].is_string() ||
                    it["name"].get<std::string>().empty()) {
                    errors.push_back("item " + std::to_string(k) +
                                      ": missing/empty 'name'");
                }
                if (it.contains("quality") && it["quality"].is_number_unsigned()) {
                    uint32_t q = it["quality"].get<uint32_t>();
                    if (q > 6) {
                        errors.push_back("item " + std::to_string(k) +
                                          ": quality " + std::to_string(q) +
                                          " out of range (must be 0..6)");
                    }
                }
                // itemLevel / stackable should be reasonable; flag
                // pathological values that almost certainly indicate
                // a typo (e.g., million-level item).
                if (it.contains("itemLevel") &&
                    it["itemLevel"].is_number_unsigned()) {
                    uint32_t lvl = it["itemLevel"].get<uint32_t>();
                    if (lvl > 1000) {
                        errors.push_back("item " + std::to_string(k) +
                                          ": itemLevel " + std::to_string(lvl) +
                                          " is suspiciously high (>1000)");
                    }
                }
                if (it.contains("stackable") &&
                    it["stackable"].is_number_unsigned()) {
                    uint32_t s = it["stackable"].get<uint32_t>();
                    if (s == 0 || s > 1000) {
                        errors.push_back("item " + std::to_string(k) +
                                          ": stackable " + std::to_string(s) +
                                          " out of range (must be 1..1000)");
                    }
                }
            }
            for (const auto& [id, indices] : idIndices) {
                if (indices.size() > 1) {
                    std::string idxList;
                    for (size_t v : indices) {
                        if (!idxList.empty()) idxList += ", ";
                        idxList += std::to_string(v);
                    }
                    errors.push_back("duplicate id " + std::to_string(id) +
                                      " at item indices [" + idxList + "]");
                }
            }
            std::printf("validate-items: %s\n", path.c_str());
            std::printf("  items checked : %zu\n", items.size());
            std::printf("  errors        : %zu\n", errors.size());
            if (errors.empty()) {
                std::printf("\n  PASSED\n");
                return 0;
            }
            std::printf("\n  Errors:\n");
            for (const auto& e : errors) {
                std::printf("    - %s\n", e.c_str());
            }
            return 1;
        } else if (std::strcmp(argv[i], "--validate-project-items") == 0 && i + 1 < argc) {
            // Project-wide wrapper around --validate-items. Spawns
            // the binary per-zone (only zones that have items.json)
            // so each zone's full error report streams through, then
            // aggregates a final tally. Exit 1 if any zone fails.
            //
            // Skips zones without items.json — those have nothing to
            // validate and shouldn't count as failures.
            std::string projectDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "validate-project-items: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                if (!fs::exists(entry.path() / "items.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            if (zones.empty()) {
                std::printf("validate-project-items: %s\n", projectDir.c_str());
                std::printf("  no zones with items.json — nothing to validate\n");
                return 0;
            }
            std::string self = argv[0];
            int passed = 0, failed = 0;
            std::printf("validate-project-items: %s\n", projectDir.c_str());
            std::printf("  zones with items : %zu\n\n", zones.size());
            for (const auto& zoneDir : zones) {
                std::printf("--- %s ---\n",
                            fs::path(zoneDir).filename().string().c_str());
                std::fflush(stdout);
                std::string cmd = "\"" + self + "\" --validate-items \"" +
                                   zoneDir + "\"";
                int rc = std::system(cmd.c_str());
                if (rc == 0) passed++;
                else failed++;
            }
            std::printf("\n--- summary ---\n");
            std::printf("  passed : %d\n", passed);
            std::printf("  failed : %d\n", failed);
            if (failed == 0) {
                std::printf("\n  ALL ZONES PASSED\n");
                return 0;
            }
            return 1;
        } else if (std::strcmp(argv[i], "--info-project-items") == 0 && i + 1 < argc) {
            // Project-wide rollup of items.json across zones. Reports
            // per-zone item counts plus project-wide totals and a
            // quality histogram. Useful for "do my zones have enough
            // loot variety?" capacity checks.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "info-project-items: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            static const char* qualityNames[] = {
                "poor", "common", "uncommon", "rare", "epic",
                "legendary", "artifact"
            };
            struct ZRow {
                std::string name;
                int count = 0;
                int qHist[7] = {};
            };
            std::vector<ZRow> rows;
            int totalItems = 0;
            int globalQHist[7] = {};
            for (const auto& zoneDir : zones) {
                ZRow r;
                r.name = fs::path(zoneDir).filename().string();
                std::string path = zoneDir + "/items.json";
                if (fs::exists(path)) {
                    nlohmann::json doc;
                    try {
                        std::ifstream in(path);
                        in >> doc;
                    } catch (...) {}
                    if (doc.contains("items") && doc["items"].is_array()) {
                        r.count = static_cast<int>(doc["items"].size());
                        for (const auto& it : doc["items"]) {
                            uint32_t q = it.value("quality", 1u);
                            if (q > 6) q = 0;
                            r.qHist[q]++;
                            globalQHist[q]++;
                        }
                    }
                }
                totalItems += r.count;
                rows.push_back(r);
            }
            if (jsonOut) {
                nlohmann::json j;
                j["project"] = projectDir;
                j["zoneCount"] = zones.size();
                j["totalItems"] = totalItems;
                nlohmann::json qual;
                for (int q = 0; q <= 6; ++q) qual[qualityNames[q]] = globalQHist[q];
                j["quality"] = qual;
                nlohmann::json zarr = nlohmann::json::array();
                for (const auto& r : rows) {
                    nlohmann::json zq;
                    for (int q = 0; q <= 6; ++q) zq[qualityNames[q]] = r.qHist[q];
                    zarr.push_back({{"name", r.name},
                                    {"count", r.count},
                                    {"quality", zq}});
                }
                j["zones"] = zarr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Project items: %s\n", projectDir.c_str());
            std::printf("  zones        : %zu\n", zones.size());
            std::printf("  total items  : %d\n\n", totalItems);
            std::printf("  Quality histogram (project-wide):\n");
            for (int q = 0; q <= 6; ++q) {
                if (globalQHist[q] == 0) continue;
                std::printf("    %-10s : %d\n", qualityNames[q], globalQHist[q]);
            }
            std::printf("\n  zone                  items   poor common uncommon rare epic legend art\n");
            for (const auto& r : rows) {
                std::printf("  %-20s  %5d  %5d  %6d  %8d  %4d  %4d  %6d  %3d\n",
                            r.name.substr(0, 20).c_str(), r.count,
                            r.qHist[0], r.qHist[1], r.qHist[2],
                            r.qHist[3], r.qHist[4], r.qHist[5], r.qHist[6]);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--scaffold-zone") == 0 && i + 1 < argc) {
            // Generate a minimal valid empty zone — useful for kickstarting
            // a new authoring session without needing to launch the GUI.
            std::string rawName = argv[++i];
            int sx = 32, sy = 32;
            if (i + 2 < argc) {
                int parsedX = std::atoi(argv[i + 1]);
                int parsedY = std::atoi(argv[i + 2]);
                if (parsedX >= 0 && parsedX <= 63 &&
                    parsedY >= 0 && parsedY <= 63) {
                    sx = parsedX; sy = parsedY;
                    i += 2;
                }
            }
            // Slugify name to match unpackZone / server module rules.
            std::string slug;
            for (char c : rawName) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-') {
                    slug += c;
                } else if (c == ' ') {
                    slug += '_';
                }
            }
            if (slug.empty()) {
                std::fprintf(stderr, "--scaffold-zone: name '%s' has no valid characters\n",
                             rawName.c_str());
                return 1;
            }
            namespace fs = std::filesystem;
            std::string dir = "custom_zones/" + slug;
            if (fs::exists(dir)) {
                std::fprintf(stderr, "--scaffold-zone: directory already exists: %s\n",
                             dir.c_str());
                return 1;
            }
            fs::create_directories(dir);

            // Blank flat terrain at the requested tile.
            auto terrain = wowee::editor::TerrainEditor::createBlankTerrain(
                sx, sy, 100.0f, wowee::editor::Biome::Grassland);
            std::string base = dir + "/" + slug + "_" +
                               std::to_string(sx) + "_" + std::to_string(sy);
            wowee::editor::WoweeTerrain::exportOpen(terrain, base, sx, sy);

            // Minimal zone.json
            wowee::editor::ZoneManifest manifest;
            manifest.mapName = slug;
            manifest.displayName = rawName;
            manifest.mapId = 9000;
            manifest.baseHeight = 100.0f;
            manifest.tiles.push_back({sx, sy});
            manifest.save(dir + "/zone.json");

            std::printf("Scaffolded zone: %s\n", dir.c_str());
            std::printf("  tile     : (%d, %d)\n", sx, sy);
            std::printf("  files    : %s.wot, %s.whm, zone.json\n",
                        slug.c_str(), slug.c_str());
            std::printf("  next step: run editor without args, then File → Open Zone\n");
            return 0;
        } else if (std::strcmp(argv[i], "--mvp-zone") == 0 && i + 1 < argc) {
            // Quick-start: scaffold + populate one of each content type
            // (1 creature, 1 object, 1 quest with objective + reward).
            // Useful for demos, screenshot bait, smoke tests of the
            // bake/validate pipeline. The zone goes from empty to
            // 'something to look at' in one command.
            std::string rawName = argv[++i];
            int sx = 32, sy = 32;
            if (i + 2 < argc) {
                int parsedX = std::atoi(argv[i + 1]);
                int parsedY = std::atoi(argv[i + 2]);
                if (parsedX >= 0 && parsedX <= 63 &&
                    parsedY >= 0 && parsedY <= 63) {
                    sx = parsedX; sy = parsedY;
                    i += 2;
                }
            }
            // Reuse scaffold-zone's slug logic.
            std::string slug;
            for (char c : rawName) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-') slug += c;
                else if (c == ' ') slug += '_';
            }
            if (slug.empty()) {
                std::fprintf(stderr,
                    "mvp-zone: name '%s' has no valid characters\n",
                    rawName.c_str());
                return 1;
            }
            namespace fs = std::filesystem;
            std::string dir = "custom_zones/" + slug;
            if (fs::exists(dir)) {
                std::fprintf(stderr,
                    "mvp-zone: directory already exists: %s\n", dir.c_str());
                return 1;
            }
            fs::create_directories(dir);
            // Scaffold terrain.
            auto terrain = wowee::editor::TerrainEditor::createBlankTerrain(
                sx, sy, 100.0f, wowee::editor::Biome::Grassland);
            std::string base = dir + "/" + slug + "_" +
                                std::to_string(sx) + "_" + std::to_string(sy);
            wowee::editor::WoweeTerrain::exportOpen(terrain, base, sx, sy);
            // Manifest.
            wowee::editor::ZoneManifest zm;
            zm.mapName = slug;
            zm.displayName = rawName;
            zm.mapId = 9000;
            zm.baseHeight = 100.0f;
            zm.tiles.push_back({sx, sy});
            zm.hasCreatures = true;
            zm.save(dir + "/zone.json");
            // Position the demo content roughly centered in the tile.
            // Tile (32, 32) is the WoW map origin; tile centers are at
            // 533.33-yard intervals from there.
            float centerX = (32.0f - sy) * 533.33333f - 266.667f;
            float centerY = (32.0f - sx) * 533.33333f - 266.667f;
            float centerZ = 100.0f;
            // Demo creature.
            wowee::editor::NpcSpawner sp;
            wowee::editor::CreatureSpawn c;
            c.name = "Demo Wolf";
            c.position = {centerX, centerY, centerZ};
            c.level = 5;
            c.health = 100;
            c.minDamage = 5; c.maxDamage = 10;
            c.displayId = 11430;  // any valid id; renderer falls back if absent
            sp.getSpawns().push_back(c);
            sp.saveToFile(dir + "/creatures.json");
            // Demo object — a tree placement near the creature.
            wowee::editor::ObjectPlacer op;
            wowee::editor::PlacedObject po;
            po.type = wowee::editor::PlaceableType::M2;
            po.path = "World/Generic/Tree.m2";
            po.position = {centerX + 5.0f, centerY, centerZ};
            po.scale = 1.0f;
            op.getObjects().push_back(po);
            op.saveToFile(dir + "/objects.json");
            // Demo quest with objective + XP reward.
            wowee::editor::QuestEditor qe;
            wowee::editor::Quest q;
            q.title = "Welcome to " + rawName;
            q.requiredLevel = 1;
            q.questGiverNpcId = c.id;  // self-referential so refs check passes
            q.turnInNpcId = c.id;
            q.reward.xp = 100;
            wowee::editor::QuestObjective obj;
            obj.type = wowee::editor::QuestObjectiveType::KillCreature;
            obj.targetName = "Demo Wolf";
            obj.targetCount = 1;
            obj.description = "Slay the Demo Wolf";
            q.objectives.push_back(obj);
            qe.addQuest(q);
            qe.saveToFile(dir + "/quests.json");
            std::printf("Created demo zone: %s\n", dir.c_str());
            std::printf("  tile     : (%d, %d)\n", sx, sy);
            std::printf("  contents : 1 creature, 1 object, 1 quest (with objective + reward)\n");
            std::printf("  next     : wowee_editor --info-zone-tree %s\n", dir.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--add-tile") == 0 && i + 3 < argc) {
            // Extend an existing zone with another ADT tile. Zones can
            // span multiple tiles (e.g. a continent fragment), but
            // --scaffold-zone only creates one. This adds another:
            //   wowee_editor --add-tile custom_zones/MyZone 29 30
            // Generates a fresh blank-flat WHM/WOT pair at the new tile
            // and appends to the zone manifest's tiles list.
            std::string zoneDir = argv[++i];
            int tx, ty;
            try {
                tx = std::stoi(argv[++i]);
                ty = std::stoi(argv[++i]);
            } catch (...) {
                std::fprintf(stderr, "add-tile: bad coordinates\n");
                return 1;
            }
            float baseHeight = 100.0f;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try { baseHeight = std::stof(argv[++i]); }
                catch (...) {}
            }
            if (tx < 0 || tx >= 64 || ty < 0 || ty >= 64) {
                std::fprintf(stderr, "add-tile: tile coord (%d, %d) out of WoW grid [0, 64)\n",
                             tx, ty);
                return 1;
            }
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr, "add-tile: %s has no zone.json — not a zone dir\n",
                             zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "add-tile: failed to parse %s\n", manifestPath.c_str());
                return 1;
            }
            // Reject duplicates so we don't silently overwrite an existing
            // tile's heightmap when the user makes a typo.
            for (const auto& [ex, ey] : zm.tiles) {
                if (ex == tx && ey == ty) {
                    std::fprintf(stderr,
                        "add-tile: tile (%d, %d) already in manifest\n", tx, ty);
                    return 1;
                }
            }
            // Also bail if the file would clobber an existing one outside
            // the manifest (e.g. user hand-created tiles without updating
            // zone.json). Catches drift between disk and manifest.
            std::string base = zoneDir + "/" + zm.mapName + "_" +
                               std::to_string(tx) + "_" + std::to_string(ty);
            if (fs::exists(base + ".whm") || fs::exists(base + ".wot")) {
                std::fprintf(stderr,
                    "add-tile: %s.{whm,wot} already exists on disk (manifest out of sync?)\n",
                    base.c_str());
                return 1;
            }
            // Generate the new heightmap. Reuses the same factory that
            // --scaffold-zone uses, so the output is consistent.
            auto terrain = wowee::editor::TerrainEditor::createBlankTerrain(
                tx, ty, baseHeight, wowee::editor::Biome::Grassland);
            wowee::editor::WoweeTerrain::exportOpen(terrain, base, tx, ty);
            // Append + save manifest. ZoneManifest::save rebuilds the
            // files block from the tiles list, so the new adt_tx_ty entry
            // appears automatically in zone.json.
            zm.tiles.push_back({tx, ty});
            if (!zm.save(manifestPath)) {
                std::fprintf(stderr, "add-tile: failed to save %s\n", manifestPath.c_str());
                return 1;
            }
            std::printf("Added tile (%d, %d) to %s\n", tx, ty, zoneDir.c_str());
            std::printf("  files     : %s.whm, %s.wot\n",
                        (zm.mapName + "_" + std::to_string(tx) + "_" + std::to_string(ty)).c_str(),
                        (zm.mapName + "_" + std::to_string(tx) + "_" + std::to_string(ty)).c_str());
            std::printf("  tiles now : %zu total\n", zm.tiles.size());
            return 0;
        } else if (std::strcmp(argv[i], "--remove-tile") == 0 && i + 3 < argc) {
            // Symmetric counterpart to --add-tile. Drops the entry from
            // ZoneManifest::tiles AND deletes the WHM/WOT/WOC files on
            // disk so the zone is left consistent (no orphan sidecars).
            std::string zoneDir = argv[++i];
            int tx, ty;
            try {
                tx = std::stoi(argv[++i]);
                ty = std::stoi(argv[++i]);
            } catch (...) {
                std::fprintf(stderr, "remove-tile: bad coordinates\n");
                return 1;
            }
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr, "remove-tile: %s has no zone.json — not a zone dir\n",
                             zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "remove-tile: failed to parse %s\n", manifestPath.c_str());
                return 1;
            }
            auto it = std::find_if(zm.tiles.begin(), zm.tiles.end(),
                [&](const std::pair<int,int>& p) { return p.first == tx && p.second == ty; });
            if (it == zm.tiles.end()) {
                std::fprintf(stderr,
                    "remove-tile: tile (%d, %d) not in manifest\n", tx, ty);
                return 1;
            }
            // Don't strand a zone with zero tiles — server module gen and
            // pack-wcp both expect at least one. The user can --rename-zone
            // or rm -rf if they want the zone gone entirely.
            if (zm.tiles.size() == 1) {
                std::fprintf(stderr,
                    "remove-tile: refusing to remove last tile (zone would be empty)\n");
                return 1;
            }
            zm.tiles.erase(it);
            // Delete the slug-prefixed files for this tile. Use error_code
            // so we don't throw on missing files — partial removal from
            // earlier failures shouldn't block cleanup of what's left.
            std::string base = zoneDir + "/" + zm.mapName + "_" +
                               std::to_string(tx) + "_" + std::to_string(ty);
            int deleted = 0;
            std::error_code ec;
            for (const char* ext : {".whm", ".wot", ".woc"}) {
                if (fs::remove(base + ext, ec)) deleted++;
            }
            if (!zm.save(manifestPath)) {
                std::fprintf(stderr, "remove-tile: failed to save %s\n", manifestPath.c_str());
                return 1;
            }
            std::printf("Removed tile (%d, %d) from %s\n", tx, ty, zoneDir.c_str());
            std::printf("  deleted   : %d file(s) (.whm/.wot/.woc)\n", deleted);
            std::printf("  tiles now : %zu remaining\n", zm.tiles.size());
            return 0;
        } else if (std::strcmp(argv[i], "--list-tiles") == 0 && i + 1 < argc) {
            // Enumerate every tile in the zone manifest with on-disk
            // file presence — useful for spotting missing/orphan files
            // before pack-wcp would fail.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr, "list-tiles: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "list-tiles: failed to parse %s\n", manifestPath.c_str());
                return 1;
            }
            auto baseFor = [&](int tx, int ty) {
                return zoneDir + "/" + zm.mapName + "_" +
                       std::to_string(tx) + "_" + std::to_string(ty);
            };
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["mapName"] = zm.mapName;
                j["count"] = zm.tiles.size();
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& [tx, ty] : zm.tiles) {
                    std::string b = baseFor(tx, ty);
                    arr.push_back({
                        {"x", tx}, {"y", ty},
                        {"whm", fs::exists(b + ".whm")},
                        {"wot", fs::exists(b + ".wot")},
                        {"woc", fs::exists(b + ".woc")},
                    });
                }
                j["tiles"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone: %s (%s, %zu tile(s))\n",
                        zoneDir.c_str(), zm.mapName.c_str(), zm.tiles.size());
            std::printf("   tx   ty   whm  wot  woc\n");
            for (const auto& [tx, ty] : zm.tiles) {
                std::string b = baseFor(tx, ty);
                std::printf("  %3d  %3d   %s    %s    %s\n",
                            tx, ty,
                            fs::exists(b + ".whm") ? "y" : "-",
                            fs::exists(b + ".wot") ? "y" : "-",
                            fs::exists(b + ".woc") ? "y" : "-");
            }
            return 0;
        } else if (std::strcmp(argv[i], "--copy-zone") == 0 && i + 2 < argc) {
            // Duplicate a zone — copy every file then rename slug-prefixed
            // ones (heightmap/terrain/collision sidecars carry the slug in
            // their filenames, e.g. "Sample_28_30.whm") so the new zone is
            // self-consistent. Useful for templating: scaffold once, then
            // copy-zone N times to create variants.
            std::string srcDir = argv[++i];
            std::string rawName = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(srcDir) || !fs::is_directory(srcDir)) {
                std::fprintf(stderr, "copy-zone: source dir not found: %s\n",
                             srcDir.c_str());
                return 1;
            }
            if (!fs::exists(srcDir + "/zone.json")) {
                std::fprintf(stderr, "copy-zone: %s has no zone.json — not a zone dir\n",
                             srcDir.c_str());
                return 1;
            }
            // Slugify new name (matches scaffold-zone rules so the result
            // round-trips through unpackZone / server module gen).
            std::string newSlug;
            for (char c : rawName) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-') {
                    newSlug += c;
                } else if (c == ' ') {
                    newSlug += '_';
                }
            }
            if (newSlug.empty()) {
                std::fprintf(stderr, "copy-zone: name '%s' has no valid characters\n",
                             rawName.c_str());
                return 1;
            }
            std::string dstDir = "custom_zones/" + newSlug;
            if (fs::exists(dstDir)) {
                std::fprintf(stderr, "copy-zone: destination already exists: %s\n",
                             dstDir.c_str());
                return 1;
            }
            // Read the source slug from its zone.json so we know what
            // prefix to rewrite. Don't trust the directory name — a user
            // could have renamed the dir without touching the manifest.
            wowee::editor::ZoneManifest src;
            if (!src.load(srcDir + "/zone.json")) {
                std::fprintf(stderr, "copy-zone: failed to parse %s/zone.json\n",
                             srcDir.c_str());
                return 1;
            }
            std::string oldSlug = src.mapName;
            if (oldSlug == newSlug) {
                std::fprintf(stderr, "copy-zone: new slug matches old (%s); nothing to do\n",
                             oldSlug.c_str());
                return 1;
            }
            // Recursive copy preserves any subdirs (e.g. data/ for DBC sidecars).
            std::error_code ec;
            fs::create_directories(dstDir);
            fs::copy(srcDir, dstDir,
                     fs::copy_options::recursive | fs::copy_options::copy_symlinks,
                     ec);
            if (ec) {
                std::fprintf(stderr, "copy-zone: copy failed: %s\n", ec.message().c_str());
                return 1;
            }
            // Rename slug-prefixed files inside the destination. Match
            // "<oldSlug>_..." or "<oldSlug>." so we catch both
            // "Sample_28_30.whm" and a hypothetical "Sample.wdt".
            int renamed = 0;
            for (const auto& entry : fs::recursive_directory_iterator(dstDir)) {
                if (!entry.is_regular_file()) continue;
                std::string fname = entry.path().filename().string();
                bool match = (fname.size() > oldSlug.size() + 1 &&
                              fname.compare(0, oldSlug.size(), oldSlug) == 0 &&
                              (fname[oldSlug.size()] == '_' ||
                               fname[oldSlug.size()] == '.'));
                if (!match) continue;
                std::string newName = newSlug + fname.substr(oldSlug.size());
                fs::rename(entry.path(), entry.path().parent_path() / newName, ec);
                if (!ec) renamed++;
            }
            // Rewrite the destination's zone.json with the new slug so its
            // files-block (rebuilt from mapName by save()) matches the
            // renamed files on disk.
            wowee::editor::ZoneManifest dst = src;
            dst.mapName = newSlug;
            dst.displayName = rawName;
            if (!dst.save(dstDir + "/zone.json")) {
                std::fprintf(stderr, "copy-zone: failed to write %s/zone.json\n",
                             dstDir.c_str());
                return 1;
            }
            std::printf("Copied %s -> %s\n", srcDir.c_str(), dstDir.c_str());
            std::printf("  mapName  : %s -> %s\n", oldSlug.c_str(), newSlug.c_str());
            std::printf("  renamed  : %d slug-prefixed file(s)\n", renamed);
            return 0;
        } else if (std::strcmp(argv[i], "--rename-zone") == 0 && i + 2 < argc) {
            // In-place rename — like --copy-zone but no copy. Useful when
            // the user wants to fix a typo or change a name without
            // doubling disk usage. Renames the directory itself too
            // (Old/ -> New/ under the same parent), so paths shift.
            std::string srcDir = argv[++i];
            std::string rawName = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(srcDir) || !fs::is_directory(srcDir)) {
                std::fprintf(stderr, "rename-zone: source dir not found: %s\n",
                             srcDir.c_str());
                return 1;
            }
            if (!fs::exists(srcDir + "/zone.json")) {
                std::fprintf(stderr, "rename-zone: %s has no zone.json — not a zone dir\n",
                             srcDir.c_str());
                return 1;
            }
            std::string newSlug;
            for (char c : rawName) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-') {
                    newSlug += c;
                } else if (c == ' ') {
                    newSlug += '_';
                }
            }
            if (newSlug.empty()) {
                std::fprintf(stderr, "rename-zone: name '%s' has no valid characters\n",
                             rawName.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(srcDir + "/zone.json")) {
                std::fprintf(stderr, "rename-zone: failed to parse %s/zone.json\n",
                             srcDir.c_str());
                return 1;
            }
            std::string oldSlug = zm.mapName;
            if (oldSlug == newSlug && rawName == zm.displayName) {
                std::fprintf(stderr,
                    "rename-zone: nothing to do (slug=%s, displayName=%s already match)\n",
                    oldSlug.c_str(), rawName.c_str());
                return 1;
            }
            // Compute target directory: same parent, new slug name. If the
            // current directory name already matches the new slug, skip
            // the dir rename (only manifest + slug-prefixed files change).
            fs::path srcPath = fs::absolute(srcDir);
            fs::path parent = srcPath.parent_path();
            fs::path dstPath = parent / newSlug;
            bool needDirRename = (srcPath.filename() != newSlug);
            if (needDirRename && fs::exists(dstPath)) {
                std::fprintf(stderr, "rename-zone: target dir already exists: %s\n",
                             dstPath.string().c_str());
                return 1;
            }
            // Rename slug-prefixed files inside the source dir BEFORE
            // moving the directory — fewer paths to fix up if anything
            // fails midway. fs::rename is atomic per-call.
            std::error_code ec;
            int renamed = 0;
            for (const auto& entry : fs::recursive_directory_iterator(srcDir)) {
                if (!entry.is_regular_file()) continue;
                std::string fname = entry.path().filename().string();
                bool match = (oldSlug != newSlug &&
                              fname.size() > oldSlug.size() + 1 &&
                              fname.compare(0, oldSlug.size(), oldSlug) == 0 &&
                              (fname[oldSlug.size()] == '_' ||
                               fname[oldSlug.size()] == '.'));
                if (!match) continue;
                std::string newName = newSlug + fname.substr(oldSlug.size());
                fs::rename(entry.path(), entry.path().parent_path() / newName, ec);
                if (!ec) renamed++;
            }
            // Update manifest and save BEFORE the dir rename so the file
            // exists at the path we're saving to.
            zm.mapName = newSlug;
            zm.displayName = rawName;
            if (!zm.save(srcDir + "/zone.json")) {
                std::fprintf(stderr, "rename-zone: failed to write zone.json\n");
                return 1;
            }
            // Now move the directory itself.
            std::string finalDir = srcDir;
            if (needDirRename) {
                fs::rename(srcPath, dstPath, ec);
                if (ec) {
                    std::fprintf(stderr,
                        "rename-zone: dir rename failed (%s); manifest already updated\n",
                        ec.message().c_str());
                    return 1;
                }
                finalDir = dstPath.string();
            }
            std::printf("Renamed %s -> %s\n", srcDir.c_str(), finalDir.c_str());
            std::printf("  mapName  : %s -> %s\n", oldSlug.c_str(), newSlug.c_str());
            std::printf("  renamed  : %d slug-prefixed file(s)\n", renamed);
            return 0;
        } else if (std::strcmp(argv[i], "--remove-zone") == 0 && i + 1 < argc) {
            // Delete a zone directory entirely. Requires --confirm to
            // actually delete (defense against accidental destruction
            // and against shell glob mishaps). Without --confirm,
            // just lists what would be deleted.
            std::string zoneDir = argv[++i];
            bool confirm = false;
            if (i + 1 < argc && std::strcmp(argv[i + 1], "--confirm") == 0) {
                confirm = true; i++;
            }
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir)) {
                std::fprintf(stderr,
                    "remove-zone: %s does not exist\n", zoneDir.c_str());
                return 1;
            }
            if (!fs::exists(zoneDir + "/zone.json")) {
                // Belt-and-suspenders: refuse to wipe anything that doesn't
                // look like a zone dir, even with --confirm. Catches typos
                // like '--remove-zone .' that would nuke the whole project.
                std::fprintf(stderr,
                    "remove-zone: %s has no zone.json — refusing to delete (not a zone dir)\n",
                    zoneDir.c_str());
                return 1;
            }
            // Read manifest for the user-facing name.
            wowee::editor::ZoneManifest zm;
            std::string zoneName = zoneDir;
            if (zm.load(zoneDir + "/zone.json")) {
                zoneName = zm.displayName.empty() ? zm.mapName : zm.displayName;
            }
            // Walk for what would be removed (counts + total bytes).
            int fileCount = 0;
            uint64_t totalBytes = 0;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file()) continue;
                fileCount++;
                totalBytes += e.file_size(ec);
            }
            if (!confirm) {
                std::printf("remove-zone: %s ('%s')\n",
                            zoneDir.c_str(), zoneName.c_str());
                std::printf("  would delete: %d file(s), %.1f KB\n",
                            fileCount, totalBytes / 1024.0);
                std::printf("  re-run with --confirm to actually delete\n");
                return 0;
            }
            // Confirmed — wipe it.
            uintmax_t removed = fs::remove_all(zoneDir, ec);
            if (ec) {
                std::fprintf(stderr,
                    "remove-zone: failed to remove %s (%s)\n",
                    zoneDir.c_str(), ec.message().c_str());
                return 1;
            }
            std::printf("Removed %s ('%s')\n", zoneDir.c_str(), zoneName.c_str());
            std::printf("  deleted: %ju filesystem entries, %.1f KB freed\n",
                        static_cast<uintmax_t>(removed), totalBytes / 1024.0);
            return 0;
        } else if (std::strcmp(argv[i], "--clear-zone-content") == 0 && i + 1 < argc) {
            // Wipe content files (creatures.json / objects.json /
            // quests.json) from a zone while keeping terrain + manifest
            // intact. Useful for templating: --copy-zone gives you a
            // duplicate; --clear-zone-content turns it into an empty
            // shell ready for fresh population.
            //
            // Pass --creatures / --objects / --quests to wipe individually,
            // or --all to wipe everything. At least one selector is required.
            std::string zoneDir = argv[++i];
            bool wipeCreatures = false, wipeObjects = false, wipeQuests = false;
            while (i + 1 < argc && argv[i + 1][0] == '-') {
                std::string opt = argv[i + 1];
                if      (opt == "--creatures") { wipeCreatures = true; ++i; }
                else if (opt == "--objects")   { wipeObjects = true;   ++i; }
                else if (opt == "--quests")    { wipeQuests = true;    ++i; }
                else if (opt == "--all") {
                    wipeCreatures = wipeObjects = wipeQuests = true; ++i;
                }
                else break;  // unknown flag — stop consuming, surface the error
            }
            if (!wipeCreatures && !wipeObjects && !wipeQuests) {
                std::fprintf(stderr,
                    "clear-zone-content: pass --creatures / --objects / --quests / --all\n");
                return 1;
            }
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "clear-zone-content: %s has no zone.json — not a zone dir\n",
                    zoneDir.c_str());
                return 1;
            }
            // Delete (not blank-write) so the next --info-* doesn't see
            // an empty file and report 'total: 0' as if data existed.
            // Missing files are the canonical 'no content' state.
            int deleted = 0;
            std::error_code ec;
            auto wipe = [&](const std::string& fname) {
                std::string p = zoneDir + "/" + fname;
                if (fs::exists(p) && fs::remove(p, ec)) {
                    ++deleted;
                    std::printf("  removed  : %s\n", fname.c_str());
                } else if (fs::exists(p)) {
                    std::fprintf(stderr,
                        "  WARN: failed to remove %s (%s)\n",
                        p.c_str(), ec.message().c_str());
                } else {
                    std::printf("  skipped  : %s (already absent)\n", fname.c_str());
                }
            };
            std::printf("Cleared content from %s\n", zoneDir.c_str());
            if (wipeCreatures) wipe("creatures.json");
            if (wipeObjects)   wipe("objects.json");
            if (wipeQuests)    wipe("quests.json");
            // Also reset manifest.hasCreatures so server module gen
            // doesn't expect an NPC table that's no longer there.
            if (wipeCreatures) {
                wowee::editor::ZoneManifest zm;
                if (zm.load(zoneDir + "/zone.json")) {
                    if (zm.hasCreatures) {
                        zm.hasCreatures = false;
                        zm.save(zoneDir + "/zone.json");
                        std::printf("  updated  : zone.json hasCreatures = false\n");
                    }
                }
            }
            std::printf("  removed  : %d file(s) total\n", deleted);
            return 0;
        } else if (std::strcmp(argv[i], "--strip-zone") == 0 && i + 1 < argc) {
            // Cleanup pass: remove the derived outputs (.glb/.obj/.stl/
            // .html/.dot/.csv/ZONE.md/DEPS.md) leaving only source files
            // (zone.json + content JSONs + open binary formats). Useful
            // before --pack-wcp so the archive doesn't carry redundant
            // exports, or before committing to git so derived blobs
            // don't bloat history.
            //
            // Optional --dry-run flag previews what would be removed
            // without actually deleting anything.
            std::string zoneDir = argv[++i];
            bool dryRun = false;
            if (i + 1 < argc && std::strcmp(argv[i + 1], "--dry-run") == 0) {
                dryRun = true;
                i++;
            }
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "strip-zone: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            // Whitelist of derived extensions. PNG is special-cased: it
            // can be either a derived export (heightmap preview at zone
            // root) or a source sidecar (BLP→PNG inside data/). Only
            // strip PNGs at the top-level zone dir.
            auto isDerivedExt = [](const std::string& ext) {
                return ext == ".glb" || ext == ".obj" || ext == ".stl" ||
                       ext == ".html" || ext == ".dot" || ext == ".csv";
            };
            auto isDerivedFilename = [](const std::string& name) {
                return name == "ZONE.md" || name == "DEPS.md" ||
                       name == "quests.dot";
            };
            int removed = 0;
            uint64_t bytesFreed = 0;
            std::error_code ec;
            // Top-level only — do NOT recurse into data/ (those are
            // source sidecars).
            for (const auto& e : fs::directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file()) continue;
                std::string ext = e.path().extension().string();
                std::string name = e.path().filename().string();
                bool kill = false;
                if (isDerivedExt(ext)) kill = true;
                if (isDerivedFilename(name)) kill = true;
                // PNG at zone root is derived (--export-png); PNGs inside
                // data/ are source. Top-level loop only sees the root
                // dir, so .png here is always derived.
                if (ext == ".png") kill = true;
                if (!kill) continue;
                uint64_t sz = e.file_size(ec);
                if (dryRun) {
                    std::printf("  would remove: %s (%llu bytes)\n",
                                name.c_str(),
                                static_cast<unsigned long long>(sz));
                } else {
                    if (fs::remove(e.path(), ec)) {
                        std::printf("  removed: %s (%llu bytes)\n",
                                    name.c_str(),
                                    static_cast<unsigned long long>(sz));
                        removed++;
                        bytesFreed += sz;
                    } else {
                        std::fprintf(stderr,
                            "  WARN: failed to remove %s (%s)\n",
                            name.c_str(), ec.message().c_str());
                    }
                }
            }
            std::printf("\nstrip-zone: %s%s\n",
                        zoneDir.c_str(), dryRun ? " (dry-run)" : "");
            if (dryRun) {
                std::printf("  pass --dry-run off to actually delete\n");
            } else {
                std::printf("  removed  : %d file(s)\n", removed);
                std::printf("  freed    : %.1f KB\n", bytesFreed / 1024.0);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--strip-project") == 0 && i + 1 < argc) {
            // Project-wide wrapper around --strip-zone. Walks every zone
            // in <projectDir>, removes derived outputs at each zone's
            // top level, and reports per-zone removed/freed counts plus
            // an aggregate. Honors --dry-run for safe previews.
            std::string projectDir = argv[++i];
            bool dryRun = false;
            if (i + 1 < argc && std::strcmp(argv[i + 1], "--dry-run") == 0) {
                dryRun = true;
                i++;
            }
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "strip-project: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            // Same derived-classifier as --strip-zone — keep in sync.
            auto isDerivedExt = [](const std::string& ext) {
                return ext == ".glb" || ext == ".obj" || ext == ".stl" ||
                       ext == ".html" || ext == ".dot" || ext == ".csv";
            };
            auto isDerivedFilename = [](const std::string& name) {
                return name == "ZONE.md" || name == "DEPS.md" ||
                       name == "quests.dot";
            };
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            struct ZRow { std::string name; int removed = 0; uint64_t freed = 0; };
            std::vector<ZRow> rows;
            int totalRemoved = 0;
            uint64_t totalFreed = 0;
            int totalFailed = 0;
            for (const auto& zoneDir : zones) {
                ZRow r;
                r.name = fs::path(zoneDir).filename().string();
                std::error_code ec;
                for (const auto& e : fs::directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    std::string ext = e.path().extension().string();
                    std::string name = e.path().filename().string();
                    bool kill = false;
                    if (isDerivedExt(ext)) kill = true;
                    if (isDerivedFilename(name)) kill = true;
                    if (ext == ".png") kill = true;
                    if (!kill) continue;
                    uint64_t sz = e.file_size(ec);
                    if (dryRun) {
                        r.removed++;
                        r.freed += sz;
                    } else {
                        if (fs::remove(e.path(), ec)) {
                            r.removed++;
                            r.freed += sz;
                        } else {
                            std::fprintf(stderr,
                                "  WARN: failed to remove %s/%s (%s)\n",
                                r.name.c_str(), name.c_str(),
                                ec.message().c_str());
                            totalFailed++;
                        }
                    }
                }
                totalRemoved += r.removed;
                totalFreed += r.freed;
                rows.push_back(r);
            }
            std::printf("strip-project: %s%s\n",
                        projectDir.c_str(), dryRun ? " (dry-run)" : "");
            std::printf("  zones    : %zu\n", zones.size());
            std::printf("\n  zone                       removed       freed\n");
            for (const auto& r : rows) {
                std::printf("  %-26s  %5d   %9.1f KB\n",
                            r.name.substr(0, 26).c_str(),
                            r.removed, r.freed / 1024.0);
            }
            std::printf("\n  totals%s : %d file(s), %.1f KB\n",
                        dryRun ? " (would-remove)" : "          ",
                        totalRemoved, totalFreed / 1024.0);
            if (dryRun) {
                std::printf("  pass --dry-run off to actually delete\n");
            }
            return totalFailed == 0 ? 0 : 1;
        } else if (std::strcmp(argv[i], "--gen-texture") == 0 && i + 2 < argc) {
            // Synthesize a placeholder PNG texture. Lets users add a
            // working texture to their project without an external
            // image editor — useful for prototyping new meshes,
            // filling out a zone before art is final, or generating
            // test fixtures.
            //
            // <colorHex|pattern>:
            //   "RRGGBB" or "RGB" hex (case-insensitive) → solid color
            //   "checker" → 32x32 black/white checkerboard
            //   "grid"    → black background with white 1-px grid every 16
            std::string outPath = argv[++i];
            std::string spec = argv[++i];
            int W = 256, H = 256;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try { W = std::stoi(argv[++i]); } catch (...) {}
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try { H = std::stoi(argv[++i]); } catch (...) {}
            }
            if (W < 1 || H < 1 || W > 8192 || H > 8192) {
                std::fprintf(stderr,
                    "gen-texture: invalid size %dx%d (must be 1..8192)\n", W, H);
                return 1;
            }
            std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3, 0);
            std::string lower = spec;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower == "checker") {
                for (int y = 0; y < H; ++y) {
                    for (int x = 0; x < W; ++x) {
                        bool dark = ((x / 32) + (y / 32)) & 1;
                        uint8_t v = dark ? 16 : 240;
                        size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
                        pixels[i2 + 0] = v;
                        pixels[i2 + 1] = v;
                        pixels[i2 + 2] = v;
                    }
                }
            } else if (lower == "grid") {
                for (int y = 0; y < H; ++y) {
                    for (int x = 0; x < W; ++x) {
                        bool line = (x % 16 == 0) || (y % 16 == 0);
                        uint8_t v = line ? 240 : 32;
                        size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
                        pixels[i2 + 0] = v;
                        pixels[i2 + 1] = v;
                        pixels[i2 + 2] = v;
                    }
                }
            } else {
                // Hex color. Accept "RGB" (3 chars) or "RRGGBB" (6 chars),
                // optional leading '#'.
                std::string hex = lower;
                if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
                auto fromHex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                    return -1;
                };
                uint8_t r = 0, g = 0, b = 0;
                if (hex.size() == 6) {
                    int hi, lo;
                    if ((hi = fromHex(hex[0])) < 0) goto bad_color;
                    if ((lo = fromHex(hex[1])) < 0) goto bad_color;
                    r = static_cast<uint8_t>((hi << 4) | lo);
                    if ((hi = fromHex(hex[2])) < 0) goto bad_color;
                    if ((lo = fromHex(hex[3])) < 0) goto bad_color;
                    g = static_cast<uint8_t>((hi << 4) | lo);
                    if ((hi = fromHex(hex[4])) < 0) goto bad_color;
                    if ((lo = fromHex(hex[5])) < 0) goto bad_color;
                    b = static_cast<uint8_t>((hi << 4) | lo);
                } else if (hex.size() == 3) {
                    int v0, v1, v2;
                    if ((v0 = fromHex(hex[0])) < 0) goto bad_color;
                    if ((v1 = fromHex(hex[1])) < 0) goto bad_color;
                    if ((v2 = fromHex(hex[2])) < 0) goto bad_color;
                    r = static_cast<uint8_t>((v0 << 4) | v0);
                    g = static_cast<uint8_t>((v1 << 4) | v1);
                    b = static_cast<uint8_t>((v2 << 4) | v2);
                } else {
                    goto bad_color;
                }
                for (int y = 0; y < H; ++y) {
                    for (int x = 0; x < W; ++x) {
                        size_t i2 = (static_cast<size_t>(y) * W + x) * 3;
                        pixels[i2 + 0] = r;
                        pixels[i2 + 1] = g;
                        pixels[i2 + 2] = b;
                    }
                }
                goto color_ok;
              bad_color:
                std::fprintf(stderr,
                    "gen-texture: '%s' is not a valid hex color or 'checker'/'grid'\n",
                    spec.c_str());
                return 1;
              color_ok: ;
            }
            if (!stbi_write_png(outPath.c_str(), W, H, 3,
                                pixels.data(), W * 3)) {
                std::fprintf(stderr,
                    "gen-texture: stbi_write_png failed for %s\n", outPath.c_str());
                return 1;
            }
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  size      : %dx%d\n", W, H);
            std::printf("  spec      : %s\n", spec.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--add-texture-to-zone") == 0 && i + 2 < argc) {
            // Import an existing PNG into a zone directory. Useful
            // for the "I have an artist-painted texture, get it into
            // my project" workflow — complements --gen-texture
            // (procedural placeholder) and --convert-blp-png (legacy
            // BLP migration).
            //
            // Optional <renameTo> argument lets the user store the
            // PNG under a project-specific name (e.g., a generic
            // "stone.png" downloaded from a tileset becomes
            // "courtyard_floor.png" in the zone).
            //
            // Refuses to overwrite an existing destination unless the
            // source and destination are byte-identical (idempotent
            // re-runs are safe).
            std::string zoneDir = argv[++i];
            std::string srcPng = argv[++i];
            std::string renameTo;
            if (i + 1 < argc && argv[i + 1][0] != '-') renameTo = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir) || !fs::is_directory(zoneDir)) {
                std::fprintf(stderr,
                    "add-texture-to-zone: %s is not a directory\n",
                    zoneDir.c_str());
                return 1;
            }
            if (!fs::exists(srcPng) || !fs::is_regular_file(srcPng)) {
                std::fprintf(stderr,
                    "add-texture-to-zone: %s is not a file\n",
                    srcPng.c_str());
                return 1;
            }
            // Sanity-check: must end in .png (any case) so users
            // don't accidentally drop a .blp/.tga and get surprised
            // when nothing renders.
            std::string srcExt = fs::path(srcPng).extension().string();
            std::transform(srcExt.begin(), srcExt.end(), srcExt.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (srcExt != ".png") {
                std::fprintf(stderr,
                    "add-texture-to-zone: %s is not a .png "
                    "(use --convert-blp-png for .blp first)\n",
                    srcPng.c_str());
                return 1;
            }
            std::string destLeaf = renameTo.empty()
                                   ? fs::path(srcPng).filename().string()
                                   : renameTo;
            // If the rename arg lacks an extension, append .png so
            // common typos ("stone" -> "stone.png") just work.
            if (fs::path(destLeaf).extension().string().empty()) {
                destLeaf += ".png";
            }
            std::string destPath = zoneDir + "/" + destLeaf;
            std::error_code ec;
            if (fs::exists(destPath)) {
                // Allow re-running if the bytes already match — makes
                // makefile-driven workflows idempotent.
                if (fs::file_size(srcPng, ec) == fs::file_size(destPath, ec)) {
                    std::ifstream a(srcPng, std::ios::binary);
                    std::ifstream b(destPath, std::ios::binary);
                    std::stringstream sa, sb;
                    sa << a.rdbuf(); sb << b.rdbuf();
                    if (sa.str() == sb.str()) {
                        std::printf("Already present: %s (no-op)\n",
                                    destPath.c_str());
                        return 0;
                    }
                }
                std::fprintf(stderr,
                    "add-texture-to-zone: %s already exists with different "
                    "content (delete it first if intentional)\n",
                    destPath.c_str());
                return 1;
            }
            fs::copy_file(srcPng, destPath, ec);
            if (ec) {
                std::fprintf(stderr,
                    "add-texture-to-zone: copy failed (%s)\n",
                    ec.message().c_str());
                return 1;
            }
            uint64_t bytes = fs::file_size(destPath, ec);
            std::printf("Imported %s -> %s\n",
                        srcPng.c_str(), destPath.c_str());
            std::printf("  bytes : %llu\n",
                        static_cast<unsigned long long>(bytes));
            std::printf("  next  : --add-texture-to-mesh <wom-base> %s\n",
                        destPath.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--repair-zone") == 0 && i + 1 < argc) {
            // Auto-fix the common manifest-vs-disk drift issues that
            // accumulate when a zone is hand-edited or partially copied:
            //   - WHM/WOT files exist on disk but tile not in manifest
            //     -> add to tiles
            //   - manifest hasCreatures=false but creatures.json exists
            //     and is non-empty -> set true
            //   - manifest hasCreatures=true but no creatures.json or
            //     empty -> clear false
            //
            // Tiles in manifest with NO disk files are NOT auto-removed
            // (they may indicate work-in-progress); they're warned about
            // so the user can decide.
            //
            // --dry-run flag previews changes without writing.
            std::string zoneDir = argv[++i];
            bool dryRun = false;
            if (i + 1 < argc && std::strcmp(argv[i + 1], "--dry-run") == 0) {
                dryRun = true; i++;
            }
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "repair-zone: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "repair-zone: parse failed\n");
                return 1;
            }
            int fixes = 0, warnings = 0;
            // Pass 1: scan disk for WHM files matching mapName_X_Y.whm
            // pattern. Match against manifest tiles. Anything on disk
            // but missing from manifest gets queued for addition.
            std::set<std::pair<int,int>> manifestTiles(
                zm.tiles.begin(), zm.tiles.end());
            std::set<std::pair<int,int>> diskTiles;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file()) continue;
                std::string name = e.path().filename().string();
                if (e.path().extension() != ".whm") continue;
                // Expect "<mapName>_TX_TY.whm". Parse out the two
                // integers between the last two underscores.
                std::string stem = name.substr(0, name.size() - 4);
                std::string prefix = zm.mapName + "_";
                if (stem.size() <= prefix.size() ||
                    stem.substr(0, prefix.size()) != prefix) {
                    continue;  // doesn't match map slug
                }
                std::string coords = stem.substr(prefix.size());
                auto under = coords.find('_');
                if (under == std::string::npos) continue;
                try {
                    int tx = std::stoi(coords.substr(0, under));
                    int ty = std::stoi(coords.substr(under + 1));
                    diskTiles.insert({tx, ty});
                } catch (...) {}
            }
            // Tiles on disk but not in manifest -> add.
            std::vector<std::pair<int,int>> toAdd;
            for (const auto& d : diskTiles) {
                if (manifestTiles.count(d) == 0) toAdd.push_back(d);
            }
            for (const auto& [tx, ty] : toAdd) {
                std::printf("  %s tile (%d, %d) to manifest\n",
                            dryRun ? "would add" : "added", tx, ty);
                if (!dryRun) zm.tiles.push_back({tx, ty});
                fixes++;
            }
            // Tiles in manifest but no .whm on disk -> warn (not auto-removed).
            for (const auto& m : manifestTiles) {
                if (diskTiles.count(m) == 0) {
                    std::printf("  WARN: tile (%d, %d) in manifest but no %s_%d_%d.whm on disk\n",
                                m.first, m.second, zm.mapName.c_str(),
                                m.first, m.second);
                    warnings++;
                }
            }
            // hasCreatures flag sync.
            bool creaturesPresent = false;
            wowee::editor::NpcSpawner sp;
            if (sp.loadFromFile(zoneDir + "/creatures.json") &&
                sp.spawnCount() > 0) {
                creaturesPresent = true;
            }
            if (zm.hasCreatures != creaturesPresent) {
                std::printf("  %s hasCreatures: %s -> %s\n",
                            dryRun ? "would set" : "set",
                            zm.hasCreatures ? "true" : "false",
                            creaturesPresent ? "true" : "false");
                if (!dryRun) zm.hasCreatures = creaturesPresent;
                fixes++;
            }
            if (!dryRun && fixes > 0) {
                if (!zm.save(manifestPath)) {
                    std::fprintf(stderr,
                        "repair-zone: failed to write %s\n", manifestPath.c_str());
                    return 1;
                }
            }
            std::printf("\nrepair-zone: %s%s\n",
                        zoneDir.c_str(), dryRun ? " (dry-run)" : "");
            std::printf("  fixes    : %d\n", fixes);
            std::printf("  warnings : %d (manual decision needed)\n", warnings);
            if (dryRun && fixes > 0) {
                std::printf("  re-run without --dry-run to apply\n");
            }
            return 0;
        } else if (std::strcmp(argv[i], "--repair-project") == 0 && i + 1 < argc) {
            // Project-wide wrapper around --repair-zone. Spawns the
            // binary per-zone so each zone's full repair report
            // streams through, then aggregates a final tally. Honors
            // --dry-run for safe previews.
            std::string projectDir = argv[++i];
            bool dryRun = false;
            if (i + 1 < argc && std::strcmp(argv[i + 1], "--dry-run") == 0) {
                dryRun = true; i++;
            }
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "repair-project: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            std::string self = argv[0];
            int totalFailed = 0;
            std::printf("repair-project: %s%s\n",
                        projectDir.c_str(), dryRun ? " (dry-run)" : "");
            std::printf("  zones : %zu\n", zones.size());
            for (const auto& zoneDir : zones) {
                std::printf("\n--- %s ---\n",
                            fs::path(zoneDir).filename().string().c_str());
                // Flush so the section marker lands before the spawned
                // child's stdout — std::system inherits FDs but each
                // process has its own buffer.
                std::fflush(stdout);
                std::string cmd = "\"" + self + "\" --repair-zone \"" +
                                  zoneDir + "\"" + (dryRun ? " --dry-run" : "");
                int rc = std::system(cmd.c_str());
                if (rc != 0) totalFailed++;
            }
            std::printf("\n--- summary ---\n");
            std::printf("  zones processed : %zu\n", zones.size());
            std::printf("  failures        : %d\n", totalFailed);
            if (dryRun) {
                std::printf("  re-run without --dry-run to apply changes\n");
            }
            return totalFailed == 0 ? 0 : 1;
        } else if (std::strcmp(argv[i], "--gen-makefile") == 0 && i + 1 < argc) {
            // Generate a Makefile that rebuilds every derived output for
            // a zone. With this in place, designers can `make` to refresh
            // glb/obj/stl/html/csv/md from sources after editing
            // creatures.json or terrain — without remembering which
            // wowee_editor flag does what. The Makefile uses dependency
            // tracking so only stale outputs get rebuilt.
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "gen-makefile: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "gen-makefile: parse failed\n");
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/Makefile";
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "gen-makefile: cannot write %s\n", outPath.c_str());
                return 1;
            }
            // Use a single absolute editor path so the Makefile works
            // from any cwd (running `make -C custom_zones/MyZone` etc.).
            std::error_code ec;
            std::string editorBin = fs::canonical("/proc/self/exe", ec).string();
            if (ec || editorBin.empty()) editorBin = "wowee_editor";
            // Per-tile WHM/WOT inputs feed the bake targets. Compose the
            // list once so all targets share the same dep set.
            std::string tileWhmDeps;
            for (const auto& [tx, ty] : zm.tiles) {
                tileWhmDeps += " " + zm.mapName + "_" +
                               std::to_string(tx) + "_" + std::to_string(ty) +
                               ".whm";
            }
            std::string slug = zm.mapName;
            out << "# Generated by wowee_editor --gen-makefile\n"
                   "# Zone: " << slug << "\n"
                   "# Run from this directory: `make` to rebuild all\n"
                   "# derived outputs from sources, `make clean` to wipe.\n\n";
            out << "EDITOR := " << editorBin << "\n";
            out << "ZONE   := .\n\n";
            // Source dep aggregations for content-derived outputs.
            out << "CONTENT_SRCS := zone.json $(wildcard creatures.json) "
                   "$(wildcard objects.json) $(wildcard quests.json)\n";
            out << "TERRAIN_SRCS := zone.json" << tileWhmDeps << "\n\n";
            out << ".PHONY: all clean glb obj stl html docs csv graph\n\n";
            out << "all: glb obj stl html docs csv graph\n\n";
            // Each target lists its dependencies so make can skip already-
            // up-to-date outputs.
            out << "glb: " << slug << ".glb\n"
                << slug << ".glb: $(TERRAIN_SRCS)\n"
                << "\t$(EDITOR) --bake-zone-glb $(ZONE)\n\n";
            out << "obj: " << slug << ".obj\n"
                << slug << ".obj: $(TERRAIN_SRCS)\n"
                << "\t$(EDITOR) --bake-zone-obj $(ZONE)\n\n";
            out << "stl: " << slug << ".stl\n"
                << slug << ".stl: $(TERRAIN_SRCS)\n"
                << "\t$(EDITOR) --bake-zone-stl $(ZONE)\n\n";
            out << "html: " << slug << ".html\n"
                << slug << ".html: " << slug << ".glb\n"
                << "\t$(EDITOR) --export-zone-html $(ZONE)\n\n";
            out << "docs: ZONE.md DEPS.md\n";
            out << "ZONE.md: $(CONTENT_SRCS)\n"
                << "\t$(EDITOR) --export-zone-summary-md $(ZONE)\n";
            out << "DEPS.md: zone.json $(wildcard objects.json) $(wildcard *.wob)\n"
                << "\t$(EDITOR) --export-zone-deps-md $(ZONE)\n\n";
            // CSV + graph targets use '-' prefix so missing-content
            // (zone without creatures/quests) doesn't fail the whole
            // 'make all'. The editor prints the error to stderr; make
            // continues with the next target.
            out << "csv:\n"
                << "\t-$(EDITOR) --export-zone-csv $(ZONE)\n\n";
            out << "graph:\n"
                << "\t-$(EDITOR) --export-quest-graph $(ZONE)\n\n";
            out << "clean:\n"
                << "\t$(EDITOR) --strip-zone $(ZONE)\n\n";
            out << "validate:\n"
                << "\t$(EDITOR) --validate-all $(ZONE)\n";
            out.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  zone   : %s\n", slug.c_str());
            std::printf("  tiles  : %zu (terrain dep)\n", zm.tiles.size());
            std::printf("  next   : cd %s && make\n", zoneDir.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--gen-project-makefile") == 0 && i + 1 < argc) {
            // Top-level Makefile that delegates to per-zone Makefiles.
            // Pairs with --gen-makefile (per-zone): one project-level
            // command rebuilds every zone's derived outputs in parallel.
            //
            //   wowee_editor --gen-project-makefile custom_zones
            //   make -C custom_zones -j$(nproc)   # all zones in parallel
            std::string projectDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "gen-project-makefile: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            if (outPath.empty()) outPath = projectDir + "/Makefile";
            // Find zones (dirs with zone.json).
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().filename().string());
            }
            std::sort(zones.begin(), zones.end());
            if (zones.empty()) {
                std::fprintf(stderr,
                    "gen-project-makefile: no zones found in %s\n",
                    projectDir.c_str());
                return 1;
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "gen-project-makefile: cannot write %s\n", outPath.c_str());
                return 1;
            }
            std::error_code ec;
            std::string editorBin = fs::canonical("/proc/self/exe", ec).string();
            if (ec || editorBin.empty()) editorBin = "wowee_editor";
            out << "# Generated by wowee_editor --gen-project-makefile\n"
                   "# Project: " << projectDir << " (" << zones.size() << " zones)\n"
                   "# Run from this dir: `make` to rebuild all zone outputs.\n"
                   "# Pass -j to make for parallel zone builds across cores.\n\n";
            out << "EDITOR := " << editorBin << "\n\n";
            out << "ZONES := ";
            for (const auto& z : zones) out << z << " ";
            out << "\n\n";
            out << ".PHONY: all clean validate index html stats $(addsuffix -bake,$(ZONES)) "
                   "$(addsuffix -clean,$(ZONES)) $(addsuffix -validate,$(ZONES))\n\n";
            // Aggregate phony targets: 'make' rebuilds all zones; 'make
            // ZONE-bake' targets just one. The per-zone Makefile must
            // exist (regenerate via --gen-makefile if not).
            out << "all: $(addsuffix -bake,$(ZONES)) index\n\n";
            for (const auto& z : zones) {
                out << z << "-bake:\n";
                out << "\t@if [ ! -f " << z << "/Makefile ]; then \\\n"
                    << "\t  $(EDITOR) --gen-makefile " << z << " >/dev/null; fi\n";
                out << "\t$(MAKE) -C " << z << " all\n\n";
                out << z << "-clean:\n";
                out << "\t-$(EDITOR) --strip-zone " << z << "\n\n";
                out << z << "-validate:\n";
                out << "\t$(EDITOR) --validate-all " << z << "\n\n";
            }
            // Top-level utility targets.
            out << "clean: $(addsuffix -clean,$(ZONES))\n\n";
            out << "validate: $(addsuffix -validate,$(ZONES))\n\n";
            out << "index:\n"
                << "\t$(EDITOR) --export-project-html .\n\n";
            out << "stats:\n"
                << "\t$(EDITOR) --zone-stats .\n\n";
            out << "tilemap:\n"
                << "\t$(EDITOR) --info-tilemap .\n";
            out.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  %zu zone(s) wired up\n", zones.size());
            std::printf("  next: make -C %s -j$(nproc)\n", projectDir.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--list-zones") == 0) {
            // Optional --json after the flag for machine-readable output.
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            auto zones = wowee::pipeline::CustomZoneDiscovery::scan({"custom_zones", "output"});
            if (jsonOut) {
                nlohmann::json j = nlohmann::json::array();
                for (const auto& z : zones) {
                    nlohmann::json zoneObj;
                    zoneObj["name"] = z.name;
                    zoneObj["directory"] = z.directory;
                    zoneObj["mapId"] = z.mapId;
                    zoneObj["author"] = z.author;
                    zoneObj["description"] = z.description;
                    zoneObj["hasCreatures"] = z.hasCreatures;
                    zoneObj["hasQuests"] = z.hasQuests;
                    nlohmann::json tiles = nlohmann::json::array();
                    for (const auto& t : z.tiles) tiles.push_back({t.first, t.second});
                    zoneObj["tiles"] = tiles;
                    j.push_back(std::move(zoneObj));
                }
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            if (zones.empty()) {
                std::printf("No custom zones found in custom_zones/ or output/\n");
            } else {
                std::printf("Custom zones found:\n");
                for (const auto& z : zones) {
                    std::printf("  %s — %s%s%s\n", z.name.c_str(), z.directory.c_str(),
                             z.hasCreatures ? " [NPCs]" : "",
                             z.hasQuests ? " [Quests]" : "");
                }
            }
            return 0;
        } else if (std::strcmp(argv[i], "--zone-stats") == 0 && i + 1 < argc) {
            // Multi-zone aggregator. Walks <projectDir> for every dir
            // with a zone.json and emits totals across the project:
            // tile counts, creature/object/quest counts, on-disk byte
            // sizes per format. Useful for content-pack release notes
            // and capacity planning.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "zone-stats: %s is not a directory\n", projectDir.c_str());
                return 1;
            }
            // Collect zone dirs.
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (fs::exists(entry.path() / "zone.json")) {
                    zones.push_back(entry.path().string());
                }
            }
            std::sort(zones.begin(), zones.end());
            // Aggregate.
            struct Totals {
                int zoneCount = 0;
                int tileCount = 0;
                int creatures = 0, objects = 0, quests = 0;
                int hostileCreatures = 0;
                int chainedQuests = 0;
                uint64_t totalXp = 0;
                uint64_t whmBytes = 0, wotBytes = 0, wocBytes = 0;
                uint64_t womBytes = 0, wobBytes = 0;
                uint64_t pngBytes = 0, jsonBytes = 0;
                uint64_t otherBytes = 0;
            } T;
            T.zoneCount = static_cast<int>(zones.size());
            // Per-zone breakdown for the table view (kept short — not
            // every field, just the high-signal ones).
            struct ZoneRow {
                std::string name;
                int tiles = 0, creatures = 0, objects = 0, quests = 0;
                uint64_t bytes = 0;
            };
            std::vector<ZoneRow> rows;
            for (const auto& zoneDir : zones) {
                wowee::editor::ZoneManifest zm;
                if (!zm.load(zoneDir + "/zone.json")) continue;
                wowee::editor::NpcSpawner sp;
                sp.loadFromFile(zoneDir + "/creatures.json");
                wowee::editor::ObjectPlacer op;
                op.loadFromFile(zoneDir + "/objects.json");
                wowee::editor::QuestEditor qe;
                qe.loadFromFile(zoneDir + "/quests.json");
                ZoneRow row;
                row.name = zm.mapName.empty()
                    ? fs::path(zoneDir).filename().string()
                    : zm.mapName;
                row.tiles = static_cast<int>(zm.tiles.size());
                row.creatures = static_cast<int>(sp.spawnCount());
                row.objects = static_cast<int>(op.getObjects().size());
                row.quests = static_cast<int>(qe.questCount());
                T.tileCount += row.tiles;
                T.creatures += row.creatures;
                T.objects += row.objects;
                T.quests += row.quests;
                for (const auto& s : sp.getSpawns()) {
                    if (s.hostile) T.hostileCreatures++;
                }
                for (const auto& q : qe.getQuests()) {
                    if (q.nextQuestId != 0) T.chainedQuests++;
                    T.totalXp += q.reward.xp;
                }
                // Walk on-disk files in the zone (recursive — sub-dirs
                // like data/ may hold sidecars). Bucket by extension.
                std::error_code ec;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    uint64_t sz = e.file_size(ec);
                    if (ec) continue;
                    row.bytes += sz;
                    std::string ext = e.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if      (ext == ".whm")  T.whmBytes  += sz;
                    else if (ext == ".wot")  T.wotBytes  += sz;
                    else if (ext == ".woc")  T.wocBytes  += sz;
                    else if (ext == ".wom")  T.womBytes  += sz;
                    else if (ext == ".wob")  T.wobBytes  += sz;
                    else if (ext == ".png")  T.pngBytes  += sz;
                    else if (ext == ".json") T.jsonBytes += sz;
                    else                     T.otherBytes += sz;
                }
                rows.push_back(row);
            }
            uint64_t totalBytes = T.whmBytes + T.wotBytes + T.wocBytes +
                                   T.womBytes + T.wobBytes + T.pngBytes +
                                   T.jsonBytes + T.otherBytes;
            if (jsonOut) {
                nlohmann::json j;
                j["projectDir"] = projectDir;
                j["zoneCount"] = T.zoneCount;
                j["tileCount"] = T.tileCount;
                j["creatures"] = T.creatures;
                j["hostileCreatures"] = T.hostileCreatures;
                j["objects"] = T.objects;
                j["quests"] = T.quests;
                j["chainedQuests"] = T.chainedQuests;
                j["totalXp"] = T.totalXp;
                j["bytes"] = {
                    {"whm", T.whmBytes},  {"wot", T.wotBytes},
                    {"woc", T.wocBytes},  {"wom", T.womBytes},
                    {"wob", T.wobBytes},  {"png", T.pngBytes},
                    {"json", T.jsonBytes}, {"other", T.otherBytes},
                    {"total", totalBytes}
                };
                nlohmann::json zarr = nlohmann::json::array();
                for (const auto& r : rows) {
                    zarr.push_back({
                        {"name", r.name}, {"tiles", r.tiles},
                        {"creatures", r.creatures}, {"objects", r.objects},
                        {"quests", r.quests}, {"bytes", r.bytes}
                    });
                }
                j["zones"] = zarr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone stats: %s\n", projectDir.c_str());
            std::printf("  zones      : %d\n", T.zoneCount);
            std::printf("  tiles      : %d total\n", T.tileCount);
            std::printf("  creatures  : %d (%d hostile)\n",
                        T.creatures, T.hostileCreatures);
            std::printf("  objects    : %d\n", T.objects);
            std::printf("  quests     : %d (%d chained, %llu total XP)\n",
                        T.quests, T.chainedQuests,
                        static_cast<unsigned long long>(T.totalXp));
            constexpr double kKB = 1024.0;
            std::printf("  bytes      : %.1f KB total\n", totalBytes / kKB);
            std::printf("    whm/wot  : %.1f KB / %.1f KB\n",
                        T.whmBytes / kKB, T.wotBytes / kKB);
            std::printf("    woc      : %.1f KB\n", T.wocBytes / kKB);
            std::printf("    wom/wob  : %.1f KB / %.1f KB\n",
                        T.womBytes / kKB, T.wobBytes / kKB);
            std::printf("    png/json : %.1f KB / %.1f KB\n",
                        T.pngBytes / kKB, T.jsonBytes / kKB);
            if (T.otherBytes > 0) {
                std::printf("    other    : %.1f KB\n", T.otherBytes / kKB);
            }
            std::printf("\n  per-zone breakdown:\n");
            std::printf("    name                tiles  creat  obj  quest    bytes\n");
            for (const auto& r : rows) {
                std::printf("    %-18s  %5d  %5d  %3d  %5d  %7.1f KB\n",
                            r.name.substr(0, 18).c_str(),
                            r.tiles, r.creatures, r.objects, r.quests,
                            r.bytes / kKB);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-tilemap") == 0 && i + 1 < argc) {
            // Visualize the WoW 64x64 ADT grid showing which tiles are
            // claimed by which zones across a project. Useful for
            // spotting tile-coord collisions before two zones try to
            // ship overlapping content, and for getting a 'where am I
            // working?' overview of a multi-zone project.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "info-tilemap: %s is not a directory\n", projectDir.c_str());
                return 1;
            }
            // Map (tx, ty) -> vector<zone names> so collision overlaps
            // are visible. Walk every zone in the project.
            std::map<std::pair<int,int>, std::vector<std::string>> claims;
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                wowee::editor::ZoneManifest zm;
                if (!zm.load((entry.path() / "zone.json").string())) continue;
                std::string zname = zm.mapName.empty()
                    ? entry.path().filename().string() : zm.mapName;
                zones.push_back(zname);
                for (const auto& [tx, ty] : zm.tiles) {
                    if (tx >= 0 && tx < 64 && ty >= 0 && ty < 64) {
                        claims[{tx, ty}].push_back(zname);
                    }
                }
            }
            // Per-zone label glyph: first letter of the zone name,
            // uppercased so different zones get distinct chars in the
            // grid. Multi-letter overlap collapses to '*'.
            std::map<std::string, char> zoneGlyph;
            char nextGlyph = 'A';
            for (const auto& z : zones) {
                if (zoneGlyph.count(z)) continue;
                if (!z.empty() && std::isalpha(static_cast<unsigned char>(z[0]))) {
                    zoneGlyph[z] = static_cast<char>(std::toupper(static_cast<unsigned char>(z[0])));
                } else {
                    zoneGlyph[z] = nextGlyph++;
                    if (nextGlyph > 'Z') nextGlyph = 'a';
                }
            }
            int collisions = 0;
            for (const auto& [coord, owners] : claims) {
                if (owners.size() > 1) collisions++;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["projectDir"] = projectDir;
                j["zoneCount"] = zones.size();
                j["claimedTiles"] = claims.size();
                j["collisions"] = collisions;
                nlohmann::json claimsJson = nlohmann::json::array();
                for (const auto& [coord, owners] : claims) {
                    claimsJson.push_back({{"x", coord.first},
                                           {"y", coord.second},
                                           {"zones", owners}});
                }
                j["claims"] = claimsJson;
                std::printf("%s\n", j.dump(2).c_str());
                return collisions == 0 ? 0 : 1;
            }
            std::printf("Tilemap: %s\n", projectDir.c_str());
            std::printf("  zones      : %zu\n", zones.size());
            std::printf("  tiles used : %zu\n", claims.size());
            std::printf("  collisions : %d (multiple zones claiming same tile)\n",
                        collisions);
            std::printf("  legend     :");
            for (const auto& [name, glyph] : zoneGlyph) {
                std::printf(" %c=%s", glyph, name.c_str());
            }
            std::printf("\n\n");
            // Render 64x64 grid. Print column header in groups of 10
            // for readability.
            std::printf("       ");
            for (int x = 0; x < 64; ++x) {
                std::printf("%c", (x % 10 == 0) ? '0' + (x / 10) : ' ');
            }
            std::printf("\n");
            std::printf("       ");
            for (int x = 0; x < 64; ++x) std::printf("%d", x % 10);
            std::printf("\n");
            for (int y = 0; y < 64; ++y) {
                // Skip rows that have no tiles claimed — keeps the
                // output bounded for projects in one corner of the map.
                bool rowHasContent = false;
                for (int x = 0; x < 64 && !rowHasContent; ++x) {
                    if (claims.count({x, y})) rowHasContent = true;
                }
                if (!rowHasContent) continue;
                std::printf("  y=%2d ", y);
                for (int x = 0; x < 64; ++x) {
                    auto it = claims.find({x, y});
                    if (it == claims.end()) {
                        std::printf(".");
                    } else if (it->second.size() > 1) {
                        std::printf("*");  // collision
                    } else {
                        std::printf("%c", zoneGlyph[it->second[0]]);
                    }
                }
                std::printf("\n");
            }
            if (collisions > 0) {
                std::printf("\n  COLLISIONS:\n");
                for (const auto& [coord, owners] : claims) {
                    if (owners.size() < 2) continue;
                    std::printf("    (%d, %d) claimed by:", coord.first, coord.second);
                    for (const auto& o : owners) std::printf(" %s", o.c_str());
                    std::printf("\n");
                }
            }
            return collisions == 0 ? 0 : 1;
        } else if (std::strcmp(argv[i], "--list-zone-deps") == 0 && i + 1 < argc) {
            // Enumerate every external model path a zone references —
            // both directly placed (objects.json) and indirectly via
            // doodad placements inside any WOB sitting next to the
            // zone manifest. Useful when packaging a content pack to
            // confirm every needed asset will ship.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "list-zone-deps: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            // Collect with usage counts so duplicates report '×N' instead
            // of cluttering the table.
            std::map<std::string, int> directM2;   // m2 placements
            std::map<std::string, int> directWMO;  // wmo placements
            std::map<std::string, int> doodadM2;   // m2s referenced inside WOBs
            wowee::editor::ObjectPlacer op;
            if (op.loadFromFile(zoneDir + "/objects.json")) {
                for (const auto& o : op.getObjects()) {
                    if (o.type == wowee::editor::PlaceableType::M2) directM2[o.path]++;
                    else if (o.type == wowee::editor::PlaceableType::WMO) directWMO[o.path]++;
                }
            }
            // Walk WOBs in the zone directory recursively and pull in
            // their doodad model paths. Sub-dirs caught too in case the
            // user organizes buildings under a buildings/ subfolder.
            int wobCount = 0;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file()) continue;
                std::string ext = e.path().extension().string();
                if (ext != ".wob") continue;
                wobCount++;
                std::string base = e.path().string();
                if (base.size() >= 4) base = base.substr(0, base.size() - 4);
                auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
                for (const auto& d : bld.doodads) {
                    if (d.modelPath.empty()) continue;
                    doodadM2[d.modelPath]++;
                }
            }
            // For each direct WMO placement, also recurse into the WOB
            // sitting at that path (relative to the zone) so transitive
            // doodad deps surface — this matches the runtime's actual
            // load chain.
            for (const auto& [path, count] : directWMO) {
                // Strip extension since loader takes a base path.
                std::string base = path;
                if (base.size() >= 4 && base.substr(base.size() - 4) == ".wmo")
                    base = base.substr(0, base.size() - 4);
                // Try relative-to-zone first, then absolute.
                std::string trial = zoneDir + "/" + base;
                if (!wowee::pipeline::WoweeBuildingLoader::exists(trial)) trial = base;
                if (!wowee::pipeline::WoweeBuildingLoader::exists(trial)) continue;
                auto bld = wowee::pipeline::WoweeBuildingLoader::load(trial);
                for (const auto& d : bld.doodads) {
                    if (d.modelPath.empty()) continue;
                    doodadM2[d.modelPath]++;
                }
            }
            size_t totalUnique = directM2.size() + directWMO.size() + doodadM2.size();
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["wobCount"] = wobCount;
                j["totalUnique"] = totalUnique;
                auto toArr = [](const std::map<std::string, int>& m) {
                    nlohmann::json a = nlohmann::json::array();
                    for (const auto& [path, count] : m) {
                        a.push_back({{"path", path}, {"count", count}});
                    }
                    return a;
                };
                j["directM2"] = toArr(directM2);
                j["directWMO"] = toArr(directWMO);
                j["doodadM2"] = toArr(doodadM2);
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Zone deps: %s\n", zoneDir.c_str());
            std::printf("  WOBs scanned        : %d\n", wobCount);
            std::printf("  unique paths total  : %zu\n", totalUnique);
            auto emit = [](const char* tag, const std::map<std::string, int>& m) {
                std::printf("\n  %s (%zu unique):\n", tag, m.size());
                if (m.empty()) {
                    std::printf("    *none*\n");
                    return;
                }
                for (const auto& [path, count] : m) {
                    if (count > 1) std::printf("    %s ×%d\n", path.c_str(), count);
                    else            std::printf("    %s\n",     path.c_str());
                }
            };
            emit("Direct M2 placements",  directM2);
            emit("Direct WMO placements", directWMO);
            emit("WOB doodad M2 refs",    doodadM2);
            return 0;
        } else if (std::strcmp(argv[i], "--list-project-orphans") == 0 && i + 1 < argc) {
            // Inverse of --list-zone-deps. Walks every zone in
            // <projectDir>, collects the set of .wom/.wob files
            // sitting on disk and the set of paths actually
            // referenced by objects.json placements + WOB doodad
            // lists. Files in the first set but not the second are
            // orphans — candidates for removal before --pack-wcp so
            // the archive doesn't carry dead weight.
            //
            // Comparison is by basename (extension stripped) since
            // the reference paths sometimes include the extension and
            // sometimes don't.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "list-project-orphans: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            // Project-wide reference set. Normalize by stripping
            // extension and any leading "./".
            auto normalize = [](std::string p) {
                while (p.size() >= 2 && p[0] == '.' && p[1] == '/') p.erase(0, 2);
                std::string ext = fs::path(p).extension().string();
                if (ext == ".wom" || ext == ".wob" || ext == ".m2" || ext == ".wmo") {
                    p = p.substr(0, p.size() - ext.size());
                }
                return p;
            };
            std::set<std::string> referencedBases;  // normalized basenames
            for (const auto& zoneDir : zones) {
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile(zoneDir + "/objects.json")) {
                    for (const auto& o : op.getObjects()) {
                        if (o.path.empty()) continue;
                        // Reference can be relative to zone or just a
                        // bare model name; record both forms for the
                        // membership test.
                        std::string norm = normalize(o.path);
                        referencedBases.insert(norm);
                        // Also try the leaf basename so unqualified
                        // refs match.
                        referencedBases.insert(fs::path(norm).filename().string());
                    }
                }
                std::error_code ec;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    if (e.path().extension() != ".wob") continue;
                    std::string base = e.path().string();
                    if (base.size() >= 4) base = base.substr(0, base.size() - 4);
                    auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
                    for (const auto& d : bld.doodads) {
                        if (d.modelPath.empty()) continue;
                        std::string norm = normalize(d.modelPath);
                        referencedBases.insert(norm);
                        referencedBases.insert(fs::path(norm).filename().string());
                    }
                }
            }
            // Now walk every zone again and flag orphan .wom/.wob files.
            struct Orphan { std::string zone, path; uint64_t bytes; };
            std::vector<Orphan> orphans;
            uint64_t totalOrphanBytes = 0;
            for (const auto& zoneDir : zones) {
                std::string zoneName = fs::path(zoneDir).filename().string();
                std::error_code ec;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    std::string ext = e.path().extension().string();
                    if (ext != ".wom" && ext != ".wob") continue;
                    std::string rel = fs::relative(e.path(), zoneDir, ec).string();
                    if (ec) rel = e.path().filename().string();
                    std::string normRel = rel.substr(0, rel.size() - ext.size());
                    std::string leaf = e.path().stem().string();
                    if (referencedBases.count(normRel) ||
                        referencedBases.count(leaf)) {
                        continue;  // referenced, not orphan
                    }
                    uint64_t sz = e.file_size(ec);
                    if (ec) sz = 0;
                    orphans.push_back({zoneName, rel, sz});
                    totalOrphanBytes += sz;
                }
            }
            std::sort(orphans.begin(), orphans.end(),
                      [](const Orphan& a, const Orphan& b) {
                          if (a.zone != b.zone) return a.zone < b.zone;
                          return a.path < b.path;
                      });
            if (jsonOut) {
                nlohmann::json j;
                j["project"] = projectDir;
                j["referencedCount"] = referencedBases.size();
                j["orphanCount"] = orphans.size();
                j["orphanBytes"] = totalOrphanBytes;
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& o : orphans) {
                    arr.push_back({{"zone", o.zone},
                                    {"path", o.path},
                                    {"bytes", o.bytes}});
                }
                j["orphans"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("Project orphans: %s\n", projectDir.c_str());
            std::printf("  zones scanned    : %zu\n", zones.size());
            std::printf("  refs collected   : %zu (normalized basenames)\n",
                        referencedBases.size());
            std::printf("  orphan .wom/.wob : %zu file(s), %.1f KB\n",
                        orphans.size(), totalOrphanBytes / 1024.0);
            if (orphans.empty()) {
                std::printf("\n  (no orphans — every model file is referenced)\n");
                return 0;
            }
            std::printf("\n  zone                  bytes      path\n");
            for (const auto& o : orphans) {
                std::printf("  %-20s  %8llu   %s\n",
                            o.zone.substr(0, 20).c_str(),
                            static_cast<unsigned long long>(o.bytes),
                            o.path.c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--remove-project-orphans") == 0 && i + 1 < argc) {
            // Destructive companion to --list-project-orphans. Reuses
            // the same reference-collection + orphan-detection logic
            // and then deletes the resulting files. --dry-run shows
            // what would be removed without touching anything.
            std::string projectDir = argv[++i];
            bool dryRun = false;
            if (i + 1 < argc && std::strcmp(argv[i + 1], "--dry-run") == 0) {
                dryRun = true; i++;
            }
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "remove-project-orphans: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            // Same normalize + reference collection as --list-project-orphans.
            // Keep both functions in sync if the matching rules evolve.
            auto normalize = [](std::string p) {
                while (p.size() >= 2 && p[0] == '.' && p[1] == '/') p.erase(0, 2);
                std::string ext = fs::path(p).extension().string();
                if (ext == ".wom" || ext == ".wob" || ext == ".m2" || ext == ".wmo") {
                    p = p.substr(0, p.size() - ext.size());
                }
                return p;
            };
            std::set<std::string> referencedBases;
            for (const auto& zoneDir : zones) {
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile(zoneDir + "/objects.json")) {
                    for (const auto& o : op.getObjects()) {
                        if (o.path.empty()) continue;
                        std::string norm = normalize(o.path);
                        referencedBases.insert(norm);
                        referencedBases.insert(fs::path(norm).filename().string());
                    }
                }
                std::error_code ec;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    if (e.path().extension() != ".wob") continue;
                    std::string base = e.path().string();
                    if (base.size() >= 4) base = base.substr(0, base.size() - 4);
                    auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
                    for (const auto& d : bld.doodads) {
                        if (d.modelPath.empty()) continue;
                        std::string norm = normalize(d.modelPath);
                        referencedBases.insert(norm);
                        referencedBases.insert(fs::path(norm).filename().string());
                    }
                }
            }
            int removed = 0, failed = 0;
            uint64_t freedBytes = 0;
            for (const auto& zoneDir : zones) {
                std::string zoneName = fs::path(zoneDir).filename().string();
                std::error_code ec;
                std::vector<fs::path> toRemove;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    std::string ext = e.path().extension().string();
                    if (ext != ".wom" && ext != ".wob") continue;
                    std::string rel = fs::relative(e.path(), zoneDir, ec).string();
                    if (ec) rel = e.path().filename().string();
                    std::string normRel = rel.substr(0, rel.size() - ext.size());
                    std::string leaf = e.path().stem().string();
                    if (referencedBases.count(normRel) ||
                        referencedBases.count(leaf)) continue;
                    toRemove.push_back(e.path());
                }
                // Materialize the deletion list before removing so we
                // don't mutate the directory while iterating.
                for (const auto& p : toRemove) {
                    uint64_t sz = fs::file_size(p, ec);
                    if (ec) sz = 0;
                    std::string rel = fs::relative(p, zoneDir, ec).string();
                    if (ec) rel = p.filename().string();
                    if (dryRun) {
                        std::printf("  would remove: %s/%s (%llu bytes)\n",
                                    zoneName.c_str(), rel.c_str(),
                                    static_cast<unsigned long long>(sz));
                        removed++;
                        freedBytes += sz;
                    } else {
                        if (fs::remove(p, ec)) {
                            std::printf("  removed: %s/%s (%llu bytes)\n",
                                        zoneName.c_str(), rel.c_str(),
                                        static_cast<unsigned long long>(sz));
                            removed++;
                            freedBytes += sz;
                        } else {
                            std::fprintf(stderr,
                                "  WARN: failed to remove %s (%s)\n",
                                p.c_str(), ec.message().c_str());
                            failed++;
                        }
                    }
                }
            }
            std::printf("\nremove-project-orphans: %s%s\n",
                        projectDir.c_str(), dryRun ? " (dry-run)" : "");
            std::printf("  zones    : %zu\n", zones.size());
            std::printf("  refs     : %zu (normalized basenames)\n",
                        referencedBases.size());
            std::printf("  %s : %d file(s)\n",
                        dryRun ? "would remove" : "removed     ", removed);
            std::printf("  freed    : %.1f KB\n", freedBytes / 1024.0);
            if (failed > 0) {
                std::printf("  FAILED   : %d (see stderr)\n", failed);
            }
            if (dryRun && removed > 0) {
                std::printf("  re-run without --dry-run to apply\n");
            }
            return failed == 0 ? 0 : 1;
        } else if (std::strcmp(argv[i], "--export-zone-deps-md") == 0 && i + 1 < argc) {
            // Markdown counterpart to --list-zone-deps. Writes a sortable
            // GitHub-rendered table of every external model the zone
            // references plus on-disk presence (so PR reviewers see at a
            // glance whether dependencies are accounted for in the
            // accompanying asset bundle).
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "export-zone-deps-md: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            zm.load(zoneDir + "/zone.json");
            if (outPath.empty()) outPath = zoneDir + "/DEPS.md";
            // Same dep-collection pass as --list-zone-deps.
            std::map<std::string, int> directM2;
            std::map<std::string, int> directWMO;
            std::map<std::string, int> doodadM2;
            wowee::editor::ObjectPlacer op;
            if (op.loadFromFile(zoneDir + "/objects.json")) {
                for (const auto& o : op.getObjects()) {
                    if (o.type == wowee::editor::PlaceableType::M2)  directM2[o.path]++;
                    else if (o.type == wowee::editor::PlaceableType::WMO) directWMO[o.path]++;
                }
            }
            int wobCount = 0;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file() ||
                    e.path().extension() != ".wob") continue;
                wobCount++;
                std::string base = e.path().string();
                if (base.size() >= 4) base = base.substr(0, base.size() - 4);
                auto bld = wowee::pipeline::WoweeBuildingLoader::load(base);
                for (const auto& d : bld.doodads) {
                    if (!d.modelPath.empty()) doodadM2[d.modelPath]++;
                }
            }
            // Resolve dep on disk. Same heuristic as --check-zone-refs:
            // try both open + proprietary in conventional roots.
            auto stripExt = [](const std::string& p, const char* ext) {
                size_t n = std::strlen(ext);
                if (p.size() >= n) {
                    std::string tail = p.substr(p.size() - n);
                    std::string lower = tail;
                    for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
                    if (lower == ext) return p.substr(0, p.size() - n);
                }
                return p;
            };
            auto resolveStatus = [&](const std::string& path, bool isWMO) {
                std::string base, openExt, propExt;
                if (isWMO) {
                    base = stripExt(path, ".wmo");
                    openExt = ".wob"; propExt = ".wmo";
                } else {
                    base = stripExt(path, ".m2");
                    openExt = ".wom"; propExt = ".m2";
                }
                std::vector<std::string> roots = {
                    "", zoneDir + "/", "output/", "custom_zones/", "Data/"
                };
                bool hasOpen = false, hasProp = false;
                for (const auto& root : roots) {
                    if (fs::exists(root + base + openExt)) hasOpen = true;
                    if (fs::exists(root + base + propExt)) hasProp = true;
                }
                if (hasOpen && hasProp) return "open + proprietary";
                if (hasOpen) return "open only";
                if (hasProp) return "proprietary only";
                return "MISSING";
            };
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "export-zone-deps-md: cannot write %s\n", outPath.c_str());
                return 1;
            }
            out << "# Dependencies — " <<
                (zm.displayName.empty() ? zm.mapName : zm.displayName) << "\n\n";
            out << "*Auto-generated by `wowee_editor --export-zone-deps-md`. "
                   "Status is best-effort — checks zone-local, output/, "
                   "custom_zones/, Data/ roots in that order.*\n\n";
            auto emitTable = [&](const char* heading,
                                  const std::map<std::string,int>& m,
                                  bool isWMO) {
                out << "## " << heading << " (" << m.size() << ")\n\n";
                if (m.empty()) {
                    out << "*None.*\n\n";
                    return;
                }
                out << "| Refs | Path | Status |\n";
                out << "|---:|---|---|\n";
                for (const auto& [path, count] : m) {
                    out << "| " << count << " | `" << path << "` | "
                        << resolveStatus(path, isWMO) << " |\n";
                }
                out << "\n";
            };
            emitTable("Direct M2 placements",  directM2,  false);
            emitTable("Direct WMO placements", directWMO, true);
            emitTable("WOB doodad M2 refs",    doodadM2,  false);
            out << "## Summary\n\n";
            out << "- Zone: `" << zm.mapName << "`\n";
            out << "- WOBs scanned: " << wobCount << "\n";
            out << "- Unique dependencies: " <<
                directM2.size() + directWMO.size() + doodadM2.size() << "\n";
            out.close();
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  %zu M2 placements, %zu WMO placements, %zu WOB doodad refs\n",
                        directM2.size(), directWMO.size(), doodadM2.size());
            return 0;
        } else if (std::strcmp(argv[i], "--export-zone-spawn-png") == 0 && i + 1 < argc) {
            // Top-down PNG of spawn positions colored by type. Bound by
            // the zone's tile range so the image is properly framed.
            // Useful for design review (does the spawn distribution
            // match the intended encounter design?) and for showing
            // collaborators 'where are the mobs'.
            std::string zoneDir = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "export-zone-spawn-png: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr,
                    "export-zone-spawn-png: parse failed\n");
                return 1;
            }
            if (zm.tiles.empty()) {
                std::fprintf(stderr, "export-zone-spawn-png: zone has no tiles\n");
                return 1;
            }
            if (outPath.empty()) outPath = zoneDir + "/" + zm.mapName + "_spawns.png";
            // Compute world-space bounds from manifest tiles. Same math
            // as --info-zone-extents.
            constexpr float kTileSize = 533.33333f;
            int tileMinX = 64, tileMaxX = -1;
            int tileMinY = 64, tileMaxY = -1;
            for (const auto& [tx, ty] : zm.tiles) {
                tileMinX = std::min(tileMinX, tx);
                tileMaxX = std::max(tileMaxX, tx);
                tileMinY = std::min(tileMinY, ty);
                tileMaxY = std::max(tileMaxY, ty);
            }
            float worldMinX = (32.0f - tileMaxY - 1) * kTileSize;
            float worldMaxX = (32.0f - tileMinY)     * kTileSize;
            float worldMinY = (32.0f - tileMaxX - 1) * kTileSize;
            float worldMaxY = (32.0f - tileMinX)     * kTileSize;
            // Image dimensions: 256px per tile so detail is visible
            // without inflating per-pixel cost.
            int tilesX = tileMaxY - tileMinY + 1;  // tile.y maps to world.x
            int tilesY = tileMaxX - tileMinX + 1;
            const int kPxPerTile = 256;
            int imgW = tilesX * kPxPerTile;
            int imgH = tilesY * kPxPerTile;
            // Cap output size — 16-tile-wide projects shouldn't exceed
            // 4096 wide. Scale down if needed.
            int maxDim = std::max(imgW, imgH);
            if (maxDim > 4096) {
                int divisor = (maxDim + 4095) / 4096;
                imgW = std::max(64, imgW / divisor);
                imgH = std::max(64, imgH / divisor);
            }
            std::vector<uint8_t> img(imgW * imgH * 3, 32);  // dark grey background
            // Tile-grid lines so the boundary is visible.
            for (int t = 1; t < tilesX; ++t) {
                int x = (t * imgW) / tilesX;
                if (x >= 0 && x < imgW) {
                    for (int y = 0; y < imgH; ++y) {
                        size_t off = (y * imgW + x) * 3;
                        img[off] = img[off+1] = img[off+2] = 64;
                    }
                }
            }
            for (int t = 1; t < tilesY; ++t) {
                int y = (t * imgH) / tilesY;
                if (y >= 0 && y < imgH) {
                    for (int x = 0; x < imgW; ++x) {
                        size_t off = (y * imgW + x) * 3;
                        img[off] = img[off+1] = img[off+2] = 64;
                    }
                }
            }
            // Plot spawn points. Map world (X, Y) to image (px, py):
            //   px = (worldMaxX - X) / (worldMaxX - worldMinX) * imgW
            //   py = (worldMaxY - Y) / (worldMaxY - worldMinY) * imgH
            // since +X world is north (up) and +Y world is west (left)
            // in WoW coords.
            float wRangeX = worldMaxX - worldMinX;
            float wRangeY = worldMaxY - worldMinY;
            auto plotPoint = [&](float wx, float wy, uint8_t r, uint8_t g, uint8_t b) {
                if (wRangeX <= 0 || wRangeY <= 0) return;
                int px = static_cast<int>((worldMaxX - wx) / wRangeX * imgW);
                int py = static_cast<int>((worldMaxY - wy) / wRangeY * imgH);
                // 3×3 dot.
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int x = px + dx, y = py + dy;
                        if (x < 0 || x >= imgW || y < 0 || y >= imgH) continue;
                        size_t off = (y * imgW + x) * 3;
                        img[off] = r; img[off+1] = g; img[off+2] = b;
                    }
                }
            };
            // Creatures = red.
            wowee::editor::NpcSpawner sp;
            int creaturesPlotted = 0;
            if (sp.loadFromFile(zoneDir + "/creatures.json")) {
                for (const auto& s : sp.getSpawns()) {
                    plotPoint(s.position.x, s.position.y, 220, 60, 60);
                    creaturesPlotted++;
                }
            }
            // Objects = green (M2) / blue (WMO).
            wowee::editor::ObjectPlacer op;
            int objectsPlotted = 0;
            if (op.loadFromFile(zoneDir + "/objects.json")) {
                for (const auto& o : op.getObjects()) {
                    if (o.type == wowee::editor::PlaceableType::M2) {
                        plotPoint(o.position.x, o.position.y, 60, 200, 60);
                    } else {
                        plotPoint(o.position.x, o.position.y, 60, 120, 220);
                    }
                    objectsPlotted++;
                }
            }
            if (!stbi_write_png(outPath.c_str(), imgW, imgH, 3,
                                 img.data(), imgW * 3)) {
                std::fprintf(stderr,
                    "export-zone-spawn-png: stbi_write_png failed\n");
                return 1;
            }
            std::printf("Wrote %s\n", outPath.c_str());
            std::printf("  %dx%d px, tile grid %dx%d, %d creatures (red), %d objects (green/blue)\n",
                        imgW, imgH, tilesX, tilesY, creaturesPlotted, objectsPlotted);
            return 0;
        } else if (std::strcmp(argv[i], "--check-zone-refs") == 0 && i + 1 < argc) {
            // Cross-reference checker: every model path in objects.json
            // must resolve as either an open WOM/WOB sidecar or a
            // proprietary M2/WMO; every quest's giver/turnIn NPC ID must
            // appear in creatures.json (when the zone has creatures).
            // Catches dangling references that --validate doesn't, since
            // --validate only checks open-format file presence.
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "check-zone-refs: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            // Try to find a model on disk in any of the conventional
            // locations (zone-local, output/, custom_zones/, Data/).
            // Strips extension and tries each open + proprietary variant.
            auto stripExt = [](const std::string& p, const char* ext) {
                size_t n = std::strlen(ext);
                if (p.size() >= n) {
                    std::string tail = p.substr(p.size() - n);
                    std::string lower = tail;
                    for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
                    if (lower == ext) return p.substr(0, p.size() - n);
                }
                return p;
            };
            auto modelExists = [&](const std::string& path, bool isWMO) {
                std::string base;
                std::vector<std::string> exts;
                if (isWMO) {
                    base = stripExt(path, ".wmo");
                    exts = {".wob", ".wmo"};
                } else {
                    base = stripExt(path, ".m2");
                    exts = {".wom", ".m2"};
                }
                std::vector<std::string> roots = {
                    "", zoneDir + "/", "output/", "custom_zones/", "Data/"
                };
                for (const auto& root : roots) {
                    for (const auto& ext : exts) {
                        if (fs::exists(root + base + ext)) return true;
                        // Case-fold fallback for case-sensitive filesystems
                        // (designers usually type Mixed Case but Linux
                        // stores asset paths lowercase after extraction).
                        std::string lower = base + ext;
                        for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
                        if (fs::exists(root + lower)) return true;
                    }
                }
                return false;
            };
            std::vector<std::string> errors;
            // Object placements -> models on disk
            wowee::editor::ObjectPlacer op;
            int objectsChecked = 0, objectsMissing = 0;
            if (op.loadFromFile(zoneDir + "/objects.json")) {
                for (size_t k = 0; k < op.getObjects().size(); ++k) {
                    const auto& o = op.getObjects()[k];
                    objectsChecked++;
                    bool isWMO = (o.type == wowee::editor::PlaceableType::WMO);
                    if (!modelExists(o.path, isWMO)) {
                        objectsMissing++;
                        if (errors.size() < 30) {
                            errors.push_back("object[" + std::to_string(k) +
                                             "] missing: " + o.path);
                        }
                    }
                }
            }
            // Quest NPCs -> creatures.json IDs (only when creatures exist;
            // otherwise NPC IDs may legitimately reference upstream content
            // outside the zone).
            wowee::editor::NpcSpawner sp;
            wowee::editor::QuestEditor qe;
            int questsChecked = 0, questsMissing = 0;
            bool hasCreatures = sp.loadFromFile(zoneDir + "/creatures.json");
            std::unordered_set<uint32_t> creatureIds;
            if (hasCreatures) {
                for (const auto& s : sp.getSpawns()) creatureIds.insert(s.id);
            }
            if (qe.loadFromFile(zoneDir + "/quests.json") && hasCreatures) {
                for (size_t k = 0; k < qe.getQuests().size(); ++k) {
                    const auto& q = qe.getQuests()[k];
                    questsChecked++;
                    bool localGiver = (q.questGiverNpcId != 0 &&
                                       creatureIds.count(q.questGiverNpcId) == 0);
                    bool localTurn  = (q.turnInNpcId != 0 &&
                                       q.turnInNpcId != q.questGiverNpcId &&
                                       creatureIds.count(q.turnInNpcId) == 0);
                    // Only flag IDs that look 'small' (likely zone-local).
                    // Production uses 6-digit IDs that reference upstream
                    // content; designers wire those in deliberately.
                    if (localGiver && q.questGiverNpcId < 100000) {
                        questsMissing++;
                        if (errors.size() < 30) {
                            errors.push_back("quest[" + std::to_string(k) + "] '" +
                                             q.title + "' giver " +
                                             std::to_string(q.questGiverNpcId) +
                                             " not in creatures.json");
                        }
                    }
                    if (localTurn && q.turnInNpcId < 100000) {
                        questsMissing++;
                        if (errors.size() < 30) {
                            errors.push_back("quest[" + std::to_string(k) + "] '" +
                                             q.title + "' turn-in " +
                                             std::to_string(q.turnInNpcId) +
                                             " not in creatures.json");
                        }
                    }
                }
            }
            int totalErrors = objectsMissing + questsMissing;
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["objectsChecked"] = objectsChecked;
                j["objectsMissing"] = objectsMissing;
                j["questsChecked"] = questsChecked;
                j["questsMissing"] = questsMissing;
                j["errors"] = errors;
                j["passed"] = (totalErrors == 0);
                std::printf("%s\n", j.dump(2).c_str());
                return totalErrors == 0 ? 0 : 1;
            }
            std::printf("Zone refs: %s\n", zoneDir.c_str());
            std::printf("  objects checked  : %d (%d missing)\n",
                        objectsChecked, objectsMissing);
            std::printf("  quests checked   : %d (%d bad NPC refs)\n",
                        questsChecked, questsMissing);
            if (totalErrors == 0) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %d issue(s):\n", totalErrors);
            for (const auto& e : errors) std::printf("    - %s\n", e.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--check-zone-content") == 0 && i + 1 < argc) {
            // Sanity-check creature/object/quest fields for plausible
            // values. --check-zone-refs catches dangling references;
            // this catches data-quality issues like creatures with 0 HP,
            // objects with negative scale, quests with no objectives.
            // Both are needed — a quest can have valid NPC IDs (refs OK)
            // AND no objectives (content broken).
            std::string zoneDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir + "/zone.json")) {
                std::fprintf(stderr,
                    "check-zone-content: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            std::vector<std::string> warnings;
            int creatureWarn = 0, objectWarn = 0, questWarn = 0;
            // Creatures
            wowee::editor::NpcSpawner sp;
            if (sp.loadFromFile(zoneDir + "/creatures.json")) {
                for (size_t k = 0; k < sp.spawnCount(); ++k) {
                    const auto& s = sp.getSpawns()[k];
                    if (s.name.empty()) {
                        warnings.push_back("creature[" + std::to_string(k) + "] has empty name");
                        creatureWarn++;
                    }
                    if (s.health == 0) {
                        warnings.push_back("creature[" + std::to_string(k) + "] '" +
                                          s.name + "' has 0 health");
                        creatureWarn++;
                    }
                    if (s.level == 0) {
                        warnings.push_back("creature[" + std::to_string(k) + "] '" +
                                          s.name + "' has level 0");
                        creatureWarn++;
                    }
                    if (s.minDamage > s.maxDamage) {
                        warnings.push_back("creature[" + std::to_string(k) + "] '" +
                                          s.name + "' has minDamage > maxDamage");
                        creatureWarn++;
                    }
                    if (s.scale <= 0.0f || !std::isfinite(s.scale)) {
                        warnings.push_back("creature[" + std::to_string(k) + "] '" +
                                          s.name + "' has non-positive or non-finite scale");
                        creatureWarn++;
                    }
                    if (s.displayId == 0) {
                        warnings.push_back("creature[" + std::to_string(k) + "] '" +
                                          s.name + "' has displayId=0 (will render invisibly)");
                        creatureWarn++;
                    }
                }
            }
            // Objects
            wowee::editor::ObjectPlacer op;
            if (op.loadFromFile(zoneDir + "/objects.json")) {
                for (size_t k = 0; k < op.getObjects().size(); ++k) {
                    const auto& o = op.getObjects()[k];
                    if (o.path.empty()) {
                        warnings.push_back("object[" + std::to_string(k) + "] has empty path");
                        objectWarn++;
                    }
                    if (o.scale <= 0.0f || !std::isfinite(o.scale)) {
                        warnings.push_back("object[" + std::to_string(k) +
                                          "] has non-positive or non-finite scale");
                        objectWarn++;
                    }
                    if (!std::isfinite(o.position.x) ||
                        !std::isfinite(o.position.y) ||
                        !std::isfinite(o.position.z)) {
                        warnings.push_back("object[" + std::to_string(k) +
                                          "] has non-finite position");
                        objectWarn++;
                    }
                }
            }
            // Quests
            wowee::editor::QuestEditor qe;
            if (qe.loadFromFile(zoneDir + "/quests.json")) {
                for (size_t k = 0; k < qe.questCount(); ++k) {
                    const auto& q = qe.getQuests()[k];
                    if (q.title.empty()) {
                        warnings.push_back("quest[" + std::to_string(k) + "] has empty title");
                        questWarn++;
                    }
                    if (q.objectives.empty()) {
                        warnings.push_back("quest[" + std::to_string(k) + "] '" +
                                          q.title + "' has no objectives (uncompletable)");
                        questWarn++;
                    }
                    if (q.reward.xp == 0 && q.reward.itemRewards.empty() &&
                        q.reward.gold == 0 && q.reward.silver == 0 && q.reward.copper == 0) {
                        warnings.push_back("quest[" + std::to_string(k) + "] '" +
                                          q.title + "' has no reward at all");
                        questWarn++;
                    }
                    if (q.requiredLevel == 0) {
                        warnings.push_back("quest[" + std::to_string(k) + "] '" +
                                          q.title + "' has requiredLevel=0");
                        questWarn++;
                    }
                }
            }
            int total = creatureWarn + objectWarn + questWarn;
            if (jsonOut) {
                nlohmann::json j;
                j["zone"] = zoneDir;
                j["creatureWarnings"] = creatureWarn;
                j["objectWarnings"] = objectWarn;
                j["questWarnings"] = questWarn;
                j["totalWarnings"] = total;
                j["warnings"] = warnings;
                j["passed"] = (total == 0);
                std::printf("%s\n", j.dump(2).c_str());
                return total == 0 ? 0 : 1;
            }
            std::printf("Zone content: %s\n", zoneDir.c_str());
            std::printf("  creature warnings: %d\n", creatureWarn);
            std::printf("  object warnings  : %d\n", objectWarn);
            std::printf("  quest warnings   : %d\n", questWarn);
            if (total == 0) {
                std::printf("  PASSED\n");
                return 0;
            }
            std::printf("  FAILED — %d total warning(s):\n", total);
            for (const auto& w : warnings) std::printf("    - %s\n", w.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--check-project-content") == 0 && i + 1 < argc) {
            // Project-level content sanity check. Walks every zone and
            // runs the same per-zone checks that --check-zone-content
            // does, aggregating warnings per zone. Exit 1 if any zone
            // has any warning. Designed for CI gates before --pack-wcp.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "check-project-content: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            // Same per-zone walks as --check-zone-content. Reuse the
            // logic by counting issues directly here (cheaper than
            // shelling out to a sub-invocation per zone).
            struct ZoneRow { std::string name; int creatureWarn, objectWarn, questWarn; };
            std::vector<ZoneRow> rows;
            int projectFailedZones = 0;
            for (const auto& zoneDir : zones) {
                ZoneRow row{fs::path(zoneDir).filename().string(), 0, 0, 0};
                wowee::editor::NpcSpawner sp;
                if (sp.loadFromFile(zoneDir + "/creatures.json")) {
                    for (const auto& s : sp.getSpawns()) {
                        if (s.name.empty()) row.creatureWarn++;
                        if (s.health == 0) row.creatureWarn++;
                        if (s.level == 0) row.creatureWarn++;
                        if (s.minDamage > s.maxDamage) row.creatureWarn++;
                        if (s.scale <= 0.0f || !std::isfinite(s.scale)) row.creatureWarn++;
                        if (s.displayId == 0) row.creatureWarn++;
                    }
                }
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile(zoneDir + "/objects.json")) {
                    for (const auto& o : op.getObjects()) {
                        if (o.path.empty()) row.objectWarn++;
                        if (o.scale <= 0.0f || !std::isfinite(o.scale)) row.objectWarn++;
                        if (!std::isfinite(o.position.x) ||
                            !std::isfinite(o.position.y) ||
                            !std::isfinite(o.position.z)) row.objectWarn++;
                    }
                }
                wowee::editor::QuestEditor qe;
                if (qe.loadFromFile(zoneDir + "/quests.json")) {
                    for (const auto& q : qe.getQuests()) {
                        if (q.title.empty()) row.questWarn++;
                        if (q.objectives.empty()) row.questWarn++;
                        if (q.reward.xp == 0 && q.reward.itemRewards.empty() &&
                            q.reward.gold == 0 && q.reward.silver == 0 &&
                            q.reward.copper == 0) row.questWarn++;
                        if (q.requiredLevel == 0) row.questWarn++;
                    }
                }
                int rowTotal = row.creatureWarn + row.objectWarn + row.questWarn;
                if (rowTotal > 0) projectFailedZones++;
                rows.push_back(row);
            }
            int allPassed = (projectFailedZones == 0);
            int totalWarn = 0;
            for (const auto& r : rows) totalWarn += r.creatureWarn + r.objectWarn + r.questWarn;
            if (jsonOut) {
                nlohmann::json j;
                j["projectDir"] = projectDir;
                j["totalZones"] = zones.size();
                j["failedZones"] = projectFailedZones;
                j["totalWarnings"] = totalWarn;
                j["passed"] = bool(allPassed);
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& r : rows) {
                    arr.push_back({{"zone", r.name},
                                    {"creatureWarn", r.creatureWarn},
                                    {"objectWarn", r.objectWarn},
                                    {"questWarn", r.questWarn}});
                }
                j["zones"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return allPassed ? 0 : 1;
            }
            std::printf("check-project-content: %s\n", projectDir.c_str());
            std::printf("  zones        : %zu (%d failed)\n",
                        zones.size(), projectFailedZones);
            std::printf("  total warns  : %d\n", totalWarn);
            std::printf("\n  zone                       creat  object   quest  status\n");
            for (const auto& r : rows) {
                int rowTotal = r.creatureWarn + r.objectWarn + r.questWarn;
                std::printf("  %-26s  %5d   %5d   %5d  %s\n",
                            r.name.substr(0, 26).c_str(),
                            r.creatureWarn, r.objectWarn, r.questWarn,
                            rowTotal == 0 ? "PASS" : "FAIL");
            }
            if (allPassed) {
                std::printf("\n  ALL ZONES PASSED\n");
                return 0;
            }
            std::printf("\n  %d zone(s) have content warnings\n",
                        projectFailedZones);
            return 1;
        } else if (std::strcmp(argv[i], "--check-project-refs") == 0 && i + 1 < argc) {
            // Project-level cross-reference checker. Walks every zone
            // and runs the same model-path / NPC-id checks as
            // --check-zone-refs. Aggregates per zone with file-level
            // breakdown. Exit 1 if any zone has dangling refs.
            std::string projectDir = argv[++i];
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "check-project-refs: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            // Same model-resolve logic as --check-zone-refs, applied
            // per zone with the appropriate root list.
            auto stripExt = [](const std::string& p, const char* ext) {
                size_t n = std::strlen(ext);
                if (p.size() >= n) {
                    std::string tail = p.substr(p.size() - n);
                    std::string lower = tail;
                    for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
                    if (lower == ext) return p.substr(0, p.size() - n);
                }
                return p;
            };
            struct ZoneRow { std::string name; int objCheck, objMiss, qCheck, qMiss; };
            std::vector<ZoneRow> rows;
            int projectFailedZones = 0;
            for (const auto& zoneDir : zones) {
                ZoneRow row{fs::path(zoneDir).filename().string(), 0, 0, 0, 0};
                auto modelExists = [&](const std::string& path, bool isWMO) {
                    std::string base;
                    std::vector<std::string> exts;
                    if (isWMO) {
                        base = stripExt(path, ".wmo");
                        exts = {".wob", ".wmo"};
                    } else {
                        base = stripExt(path, ".m2");
                        exts = {".wom", ".m2"};
                    }
                    std::vector<std::string> roots = {
                        "", zoneDir + "/", "output/", "custom_zones/", "Data/"
                    };
                    for (const auto& root : roots) {
                        for (const auto& ext : exts) {
                            if (fs::exists(root + base + ext)) return true;
                            std::string lower = base + ext;
                            for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
                            if (fs::exists(root + lower)) return true;
                        }
                    }
                    return false;
                };
                wowee::editor::ObjectPlacer op;
                if (op.loadFromFile(zoneDir + "/objects.json")) {
                    for (const auto& o : op.getObjects()) {
                        row.objCheck++;
                        bool isWMO = (o.type == wowee::editor::PlaceableType::WMO);
                        if (!modelExists(o.path, isWMO)) row.objMiss++;
                    }
                }
                wowee::editor::NpcSpawner sp;
                wowee::editor::QuestEditor qe;
                bool hasCreatures = sp.loadFromFile(zoneDir + "/creatures.json");
                std::unordered_set<uint32_t> creatureIds;
                if (hasCreatures) {
                    for (const auto& s : sp.getSpawns()) creatureIds.insert(s.id);
                }
                if (qe.loadFromFile(zoneDir + "/quests.json") && hasCreatures) {
                    for (const auto& q : qe.getQuests()) {
                        row.qCheck++;
                        bool localGiver = (q.questGiverNpcId != 0 &&
                                            q.questGiverNpcId < 100000 &&
                                            creatureIds.count(q.questGiverNpcId) == 0);
                        bool localTurn  = (q.turnInNpcId != 0 &&
                                            q.turnInNpcId < 100000 &&
                                            q.turnInNpcId != q.questGiverNpcId &&
                                            creatureIds.count(q.turnInNpcId) == 0);
                        if (localGiver) row.qMiss++;
                        if (localTurn) row.qMiss++;
                    }
                }
                if (row.objMiss + row.qMiss > 0) projectFailedZones++;
                rows.push_back(row);
            }
            int allPassed = (projectFailedZones == 0);
            int totalMiss = 0;
            for (const auto& r : rows) totalMiss += r.objMiss + r.qMiss;
            if (jsonOut) {
                nlohmann::json j;
                j["projectDir"] = projectDir;
                j["totalZones"] = zones.size();
                j["failedZones"] = projectFailedZones;
                j["totalMissing"] = totalMiss;
                j["passed"] = bool(allPassed);
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& r : rows) {
                    arr.push_back({{"zone", r.name},
                                    {"objectsChecked", r.objCheck},
                                    {"objectsMissing", r.objMiss},
                                    {"questsChecked", r.qCheck},
                                    {"questsMissing", r.qMiss}});
                }
                j["zones"] = arr;
                std::printf("%s\n", j.dump(2).c_str());
                return allPassed ? 0 : 1;
            }
            std::printf("check-project-refs: %s\n", projectDir.c_str());
            std::printf("  zones        : %zu (%d failed)\n",
                        zones.size(), projectFailedZones);
            std::printf("  total missing: %d\n", totalMiss);
            std::printf("\n  zone                       obj_chk obj_miss  q_chk  q_miss  status\n");
            for (const auto& r : rows) {
                int rowMiss = r.objMiss + r.qMiss;
                std::printf("  %-26s   %5d    %5d  %5d   %5d  %s\n",
                            r.name.substr(0, 26).c_str(),
                            r.objCheck, r.objMiss, r.qCheck, r.qMiss,
                            rowMiss == 0 ? "PASS" : "FAIL");
            }
            if (allPassed) {
                std::printf("\n  ALL ZONES PASSED\n");
                return 0;
            }
            std::printf("\n  %d zone(s) have dangling refs\n", projectFailedZones);
            return 1;
        } else if (std::strcmp(argv[i], "--for-each-zone") == 0 && i + 1 < argc) {
            // Batch runner: enumerates zones in <projectDir> and runs the
            // command after '--' for each one. '{}' in the command is
            // substituted with the zone path (find -exec convention).
            //
            //   wowee_editor --for-each-zone custom_zones -- \\
            //     wowee_editor --validate-all {}
            //
            // Returns the count of failed runs as the exit code (capped
            // at 255 so the shell can still see it).
            std::string projectDir = argv[++i];
            // The literal '--' separates the projectDir from the command.
            // Skip it; everything after is the command template.
            if (i + 1 < argc && std::strcmp(argv[i + 1], "--") == 0) ++i;
            if (i + 1 >= argc) {
                std::fprintf(stderr,
                    "for-each-zone: need command after '--'\n");
                return 1;
            }
            // Collect command tokens until end of argv. Don't try to be
            // clever about quoting — just escape each token for shell
            // safety using single quotes (' inside is escaped as '\\'').
            std::vector<std::string> cmdTokens;
            for (int k = i + 1; k < argc; ++k) cmdTokens.push_back(argv[k]);
            i = argc - 1;  // consume rest of argv
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr, "for-each-zone: %s is not a directory\n",
                             projectDir.c_str());
                return 1;
            }
            // Find every child dir that contains a zone.json — that's the
            // canonical 'is this a zone?' test the rest of the editor uses.
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (fs::exists(entry.path() / "zone.json")) {
                    zones.push_back(entry.path().string());
                }
            }
            std::sort(zones.begin(), zones.end());
            if (zones.empty()) {
                std::fprintf(stderr, "for-each-zone: no zones found in %s\n",
                             projectDir.c_str());
                return 1;
            }
            auto shellEscape = [](const std::string& s) {
                std::string out = "'";
                for (char c : s) {
                    if (c == '\'') out += "'\\''";
                    else out += c;
                }
                out += "'";
                return out;
            };
            int failed = 0;
            for (const auto& zone : zones) {
                std::string cmd;
                for (size_t k = 0; k < cmdTokens.size(); ++k) {
                    if (k > 0) cmd += " ";
                    std::string token = cmdTokens[k];
                    // Replace {} with zone path (every occurrence).
                    size_t pos;
                    while ((pos = token.find("{}")) != std::string::npos) {
                        token.replace(pos, 2, zone);
                    }
                    cmd += shellEscape(token);
                }
                std::printf("[%s]\n", zone.c_str());
                // Flush before std::system so the header lands above the
                // child's output rather than after (parent stdout is line-
                // buffered, child writes go straight to the terminal).
                std::fflush(stdout);
                int rc = std::system(cmd.c_str());
                if (rc != 0) {
                    failed++;
                    std::fprintf(stderr,
                        "for-each-zone: command exited %d for %s\n",
                        rc, zone.c_str());
                }
            }
            std::printf("\nfor-each-zone: %zu zones, %d failed\n",
                        zones.size(), failed);
            return failed > 255 ? 255 : failed;
        } else if (std::strcmp(argv[i], "--for-each-tile") == 0 && i + 1 < argc) {
            // Per-tile batch runner. --for-each-zone iterates zones in
            // a project; this iterates tiles within a zone. The '{}' in
            // the command template is replaced with the tile-base path
            // (zoneDir/mapName_TX_TY) — the form most tile-level
            // editor commands take.
            //
            //   wowee_editor --for-each-tile MyZone -- \\
            //     wowee_editor --build-woc {}
            //   wowee_editor --for-each-tile MyZone -- \\
            //     wowee_editor --validate-whm {}
            std::string zoneDir = argv[++i];
            if (i + 1 < argc && std::strcmp(argv[i + 1], "--") == 0) ++i;
            if (i + 1 >= argc) {
                std::fprintf(stderr,
                    "for-each-tile: need command after '--'\n");
                return 1;
            }
            std::vector<std::string> cmdTokens;
            for (int k = i + 1; k < argc; ++k) cmdTokens.push_back(argv[k]);
            i = argc - 1;
            namespace fs = std::filesystem;
            std::string manifestPath = zoneDir + "/zone.json";
            if (!fs::exists(manifestPath)) {
                std::fprintf(stderr,
                    "for-each-tile: %s has no zone.json\n", zoneDir.c_str());
                return 1;
            }
            wowee::editor::ZoneManifest zm;
            if (!zm.load(manifestPath)) {
                std::fprintf(stderr, "for-each-tile: parse failed\n");
                return 1;
            }
            if (zm.tiles.empty()) {
                std::fprintf(stderr, "for-each-tile: zone has no tiles\n");
                return 1;
            }
            // Same shell-escape + cmd-substitution as --for-each-zone.
            auto shellEscape = [](const std::string& s) {
                std::string out = "'";
                for (char c : s) {
                    if (c == '\'') out += "'\\''";
                    else out += c;
                }
                out += "'";
                return out;
            };
            int failed = 0;
            // Sort tiles so order is deterministic across runs.
            auto tiles = zm.tiles;
            std::sort(tiles.begin(), tiles.end());
            for (const auto& [tx, ty] : tiles) {
                std::string tileBase = zoneDir + "/" + zm.mapName + "_" +
                                        std::to_string(tx) + "_" + std::to_string(ty);
                std::string cmd;
                for (size_t k = 0; k < cmdTokens.size(); ++k) {
                    if (k > 0) cmd += " ";
                    std::string token = cmdTokens[k];
                    size_t pos;
                    while ((pos = token.find("{}")) != std::string::npos) {
                        token.replace(pos, 2, tileBase);
                    }
                    cmd += shellEscape(token);
                }
                std::printf("[%s (%d, %d)]\n", tileBase.c_str(), tx, ty);
                std::fflush(stdout);
                int rc = std::system(cmd.c_str());
                if (rc != 0) {
                    failed++;
                    std::fprintf(stderr,
                        "for-each-tile: command exited %d for (%d, %d)\n",
                        rc, tx, ty);
                }
            }
            std::printf("\nfor-each-tile: %zu tiles, %d failed\n",
                        tiles.size(), failed);
            return failed > 255 ? 255 : failed;
        } else if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("Wowee World Editor v1.0.0\n");
            std::printf("Open formats: WOT/WHM/WOM/WOB/WOC/WCP + PNG/JSON (all novel)\n");
            std::printf("By Kelsi Davis\n");
            return 0;
        } else if (std::strcmp(argv[i], "--list-commands") == 0) {
            // Capture printUsage's stdout and grep for '--flag' tokens at
            // the start of each line. This auto-tracks the help text as
            // commands are added — no parallel list to maintain. Result
            // is a sorted, deduped, one-per-line list of recognized flags.
            FILE* old = stdout;
            // Temp file lets us read printUsage's output back. fmemopen
            // would be cleaner but isn't available on Windows; tmpfile is
            // portable.
            FILE* tmp = std::tmpfile();
            if (!tmp) { std::fprintf(stderr, "list-commands: tmpfile failed\n"); return 1; }
            stdout = tmp;
            wowee::editor::cli::printUsage(argv[0]);
            stdout = old;
            std::fseek(tmp, 0, SEEK_SET);
            std::set<std::string> commands;
            char line[512];
            while (std::fgets(line, sizeof(line), tmp)) {
                // Match leading whitespace then '--' then [a-z-]+
                const char* p = line;
                while (*p == ' ' || *p == '\t') ++p;
                if (p[0] != '-' || p[1] != '-') continue;
                std::string flag;
                while (*p && (std::isalnum(static_cast<unsigned char>(*p)) ||
                              *p == '-' || *p == '_')) {
                    flag += *p++;
                }
                if (flag.size() > 2) commands.insert(flag);
            }
            std::fclose(tmp);
            // Always include the meta-flags that printUsage describes
            // alongside others (-h/-v aliases) since the regex above only
            // captures double-dash forms.
            commands.insert("--help");
            commands.insert("--version");
            for (const auto& c : commands) std::printf("%s\n", c.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--info-cli-stats") == 0) {
            // Meta-stats on the CLI surface: total command count + per-
            // category breakdown by prefix verb (--info-*, --validate-*,
            // --diff-*, etc.). Useful for tracking growth over time and
            // spotting category imbalances.
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            // Re-use --list-commands' parser. Capture printUsage stdout.
            FILE* old = stdout;
            FILE* tmp = std::tmpfile();
            if (!tmp) { std::fprintf(stderr, "info-cli-stats: tmpfile failed\n"); return 1; }
            stdout = tmp;
            wowee::editor::cli::printUsage(argv[0]);
            stdout = old;
            std::fseek(tmp, 0, SEEK_SET);
            std::set<std::string> commands;
            char line[512];
            while (std::fgets(line, sizeof(line), tmp)) {
                const char* p = line;
                while (*p == ' ' || *p == '\t') ++p;
                if (p[0] != '-' || p[1] != '-') continue;
                std::string flag;
                while (*p && (std::isalnum(static_cast<unsigned char>(*p)) ||
                              *p == '-' || *p == '_')) { flag += *p++; }
                if (flag.size() > 2) commands.insert(flag);
            }
            std::fclose(tmp);
            commands.insert("--help");
            commands.insert("--version");
            // Bucket by category — verb is the second token after '--',
            // up to the next dash. So '--info-zone-tree' -> 'info'.
            std::map<std::string, int> byCategory;
            int maxLen = 0;
            for (const auto& c : commands) {
                if (static_cast<int>(c.size()) > maxLen) maxLen = static_cast<int>(c.size());
                size_t verbStart = 2;  // skip '--'
                size_t verbEnd = c.find('-', verbStart);
                std::string verb = (verbEnd == std::string::npos)
                    ? c.substr(verbStart)
                    : c.substr(verbStart, verbEnd - verbStart);
                byCategory[verb]++;
            }
            if (jsonOut) {
                nlohmann::json j;
                j["totalCommands"] = commands.size();
                j["maxFlagLength"] = maxLen;
                nlohmann::json cats = nlohmann::json::object();
                for (const auto& [v, c] : byCategory) cats[v] = c;
                j["byCategory"] = cats;
                std::printf("%s\n", j.dump(2).c_str());
                return 0;
            }
            std::printf("CLI surface stats\n");
            std::printf("  total commands : %zu\n", commands.size());
            std::printf("  longest flag   : %d chars\n", maxLen);
            std::printf("\n  Categories (by verb prefix, sorted by count):\n");
            // Sort by count descending for the table.
            std::vector<std::pair<std::string, int>> sorted(
                byCategory.begin(), byCategory.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) {
                          return a.second > b.second;
                      });
            for (const auto& [verb, count] : sorted) {
                std::printf("    --%-12s %4d\n", verb.c_str(), count);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-cli-categories") == 0) {
            // Discovery view of every CLI flag grouped by verb prefix.
            // Where --info-cli-stats just counts per category, this
            // lists every command in each category — handy for "I
            // know I want to gen something but what shapes/textures
            // are available?"
            FILE* old = stdout;
            FILE* tmp = std::tmpfile();
            if (!tmp) {
                std::fprintf(stderr, "info-cli-categories: tmpfile failed\n");
                return 1;
            }
            stdout = tmp;
            wowee::editor::cli::printUsage(argv[0]);
            stdout = old;
            std::fseek(tmp, 0, SEEK_SET);
            std::set<std::string> commands;
            char line[512];
            while (std::fgets(line, sizeof(line), tmp)) {
                const char* p = line;
                while (*p == ' ' || *p == '\t') ++p;
                if (p[0] != '-' || p[1] != '-') continue;
                std::string flag;
                while (*p && (std::isalnum(static_cast<unsigned char>(*p)) ||
                              *p == '-' || *p == '_')) { flag += *p++; }
                if (flag.size() > 2) commands.insert(flag);
            }
            std::fclose(tmp);
            commands.insert("--help");
            commands.insert("--version");
            std::map<std::string, std::vector<std::string>> byCategory;
            for (const auto& c : commands) {
                size_t verbStart = 2;
                size_t verbEnd = c.find('-', verbStart);
                std::string verb = (verbEnd == std::string::npos)
                    ? c.substr(verbStart)
                    : c.substr(verbStart, verbEnd - verbStart);
                byCategory[verb].push_back(c);
            }
            std::printf("CLI commands by category (%zu total):\n\n",
                        commands.size());
            // Sort categories by count descending, commands within
            // each alphabetically.
            std::vector<std::pair<std::string, std::vector<std::string>>> sorted(
                byCategory.begin(), byCategory.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) {
                          if (a.second.size() != b.second.size())
                              return a.second.size() > b.second.size();
                          return a.first < b.first;
                      });
            for (const auto& [verb, cmds] : sorted) {
                std::printf("--%s (%zu):\n", verb.c_str(), cmds.size());
                for (const auto& c : cmds) {
                    std::printf("  %s\n", c.c_str());
                }
                std::printf("\n");
            }
            return 0;
        } else if (std::strcmp(argv[i], "--info-cli-help") == 0 && i + 1 < argc) {
            // Substring search through the help text. With 130+ commands,
            // 'is there a thing for X?' is a common ask — this answers it
            // without making the user scroll the full --help output:
            //
            //   wowee_editor --info-cli-help quest
            //   wowee_editor --info-cli-help validate
            //   wowee_editor --info-cli-help glb
            std::string pattern = argv[++i];
            // Lowercase the pattern for case-insensitive match.
            std::string patLower = pattern;
            for (auto& c : patLower) c = std::tolower(static_cast<unsigned char>(c));
            // Capture printUsage stdout, walk line-by-line, print every
            // line containing the pattern (case-insensitive). Continuation
            // lines (the indented description on the line after a flag)
            // are emitted along with the flag line for context.
            FILE* old = stdout;
            FILE* tmp = std::tmpfile();
            if (!tmp) {
                std::fprintf(stderr, "info-cli-help: tmpfile failed\n"); return 1;
            }
            stdout = tmp;
            wowee::editor::cli::printUsage(argv[0]);
            stdout = old;
            std::fseek(tmp, 0, SEEK_SET);
            std::vector<std::string> lines;
            char buf[1024];
            while (std::fgets(buf, sizeof(buf), tmp)) {
                std::string s = buf;
                if (!s.empty() && s.back() == '\n') s.pop_back();
                lines.push_back(std::move(s));
            }
            std::fclose(tmp);
            int matches = 0;
            for (size_t k = 0; k < lines.size(); ++k) {
                std::string lower = lines[k];
                for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
                if (lower.find(patLower) == std::string::npos) continue;
                std::printf("%s\n", lines[k].c_str());
                // Look ahead for a continuation line (indented and not
                // starting with '--'). Print it for context.
                if (k + 1 < lines.size()) {
                    const auto& next = lines[k + 1];
                    if (!next.empty() && next[0] == ' ' &&
                        next.find("--") == std::string::npos) {
                        std::printf("%s\n", next.c_str());
                    }
                }
                matches++;
            }
            if (matches == 0) {
                std::fprintf(stderr, "info-cli-help: no matches for '%s'\n",
                             pattern.c_str());
                return 1;
            }
            std::fprintf(stderr, "\n%d line(s) matched '%s'\n", matches, pattern.c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--validate-cli-help") == 0) {
            // Self-check: every flag we declare in kArgRequired (the list
            // of commands needing positional args) must appear in the
            // help text printUsage emits. Catches drift where someone
            // adds a handler + argument check but forgets the help line.
            bool jsonOut = (i + 1 < argc &&
                            std::strcmp(argv[i + 1], "--json") == 0);
            if (jsonOut) i++;
            // Capture printUsage's stdout.
            FILE* old = stdout;
            FILE* tmp = std::tmpfile();
            if (!tmp) { std::fprintf(stderr, "validate-cli-help: tmpfile failed\n"); return 1; }
            stdout = tmp;
            wowee::editor::cli::printUsage(argv[0]);
            stdout = old;
            std::fseek(tmp, 0, SEEK_SET);
            std::string helpText;
            char chunk[1024];
            while (std::fgets(chunk, sizeof(chunk), tmp)) helpText += chunk;
            std::fclose(tmp);
            // Walk kArgRequired and check each appears in the help.
            std::vector<std::string> missing;
            for (const char* opt : kArgRequired) {
                if (helpText.find(opt) == std::string::npos) {
                    missing.push_back(opt);
                }
            }
            if (jsonOut) {
                nlohmann::json j;
                j["totalArgRequired"] = sizeof(kArgRequired) / sizeof(kArgRequired[0]);
                j["missing"] = missing;
                j["passed"] = missing.empty();
                std::printf("%s\n", j.dump(2).c_str());
                return missing.empty() ? 0 : 1;
            }
            std::printf("CLI help self-check\n");
            std::printf("  kArgRequired entries : %zu\n",
                        sizeof(kArgRequired) / sizeof(kArgRequired[0]));
            if (missing.empty()) {
                std::printf("  PASSED — every kArgRequired flag is documented\n");
                return 0;
            }
            std::printf("  FAILED — %zu flag(s) missing from help text:\n", missing.size());
            for (const auto& m : missing) std::printf("    - %s\n", m.c_str());
            return 1;
        } else if (std::strcmp(argv[i], "--gen-completion") == 0 && i + 1 < argc) {
            // Emit a bash or zsh completion script. Re-execs the editor's
            // own --list-commands at completion time so newly-added flags
            // light up automatically without regenerating the script.
            std::string shell = argv[++i];
            if (shell != "bash" && shell != "zsh") {
                std::fprintf(stderr,
                    "gen-completion: shell must be 'bash' or 'zsh', got '%s'\n",
                    shell.c_str());
                return 1;
            }
            // Use argv[0] as the binary name in the completion so it
            // works whether the user installed it as 'wowee_editor' or
            // a custom alias. Strip directory components for the
            // completion-name registration (bash 'complete -F' expects
            // a basename).
            std::string self = argv[0];
            auto slash = self.find_last_of('/');
            std::string baseName = (slash != std::string::npos)
                ? self.substr(slash + 1)
                : self;
            if (shell == "bash") {
                std::printf(
                    "# wowee_editor bash completion — source from ~/.bashrc:\n"
                    "#   source <(%s --gen-completion bash)\n"
                    "_wowee_editor_complete() {\n"
                    "  local cur prev cmds\n"
                    "  COMPREPLY=()\n"
                    "  cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
                    "  prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
                    "  # Cache the command list per shell session.\n"
                    "  if [[ -z \"$_WOWEE_EDITOR_CMDS\" ]]; then\n"
                    "    _WOWEE_EDITOR_CMDS=$(%s --list-commands 2>/dev/null)\n"
                    "  fi\n"
                    "  if [[ \"$cur\" == --* ]]; then\n"
                    "    COMPREPLY=( $(compgen -W \"$_WOWEE_EDITOR_CMDS\" -- \"$cur\") )\n"
                    "    return 0\n"
                    "  fi\n"
                    "  # Default: complete file paths for arg slots.\n"
                    "  COMPREPLY=( $(compgen -f -- \"$cur\") )\n"
                    "}\n"
                    "complete -F _wowee_editor_complete %s\n",
                    self.c_str(), self.c_str(), baseName.c_str());
            } else {
                // zsh — simpler descriptor-based completion.
                std::printf(
                    "# wowee_editor zsh completion — source from ~/.zshrc:\n"
                    "#   source <(%s --gen-completion zsh)\n"
                    "_wowee_editor_complete() {\n"
                    "  local -a cmds\n"
                    "  if [[ -z \"$_WOWEE_EDITOR_CMDS\" ]]; then\n"
                    "    export _WOWEE_EDITOR_CMDS=$(%s --list-commands 2>/dev/null)\n"
                    "  fi\n"
                    "  cmds=( ${(f)_WOWEE_EDITOR_CMDS} )\n"
                    "  _arguments \"*: :($cmds)\"\n"
                    "}\n"
                    "compdef _wowee_editor_complete %s\n",
                    self.c_str(), self.c_str(), baseName.c_str());
            }
            return 0;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            wowee::editor::cli::printUsage(argv[0]);
            return 0;
        }
    }

    // Batch convert mode: --convert <m2path> converts M2 to WOM
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--convert-m2") == 0 && i + 1 < argc) {
            std::string m2Path = argv[++i];
            std::printf("Converting M2→WOM: %s\n", m2Path.c_str());
            if (dataPath.empty()) dataPath = "Data";
            wowee::pipeline::AssetManager am;
            if (am.initialize(dataPath)) {
                auto wom = wowee::pipeline::WoweeModelLoader::fromM2(m2Path, &am);
                if (wom.isValid()) {
                    std::string outPath = m2Path;
                    auto dot = outPath.rfind('.');
                    if (dot != std::string::npos) outPath = outPath.substr(0, dot);
                    wowee::pipeline::WoweeModelLoader::save(wom, "output/models/" + outPath);
                    std::printf("OK: output/models/%s.wom (v%u, %zu verts, %zu bones, %zu batches)\n",
                        outPath.c_str(), wom.version, wom.vertices.size(),
                        wom.bones.size(), wom.batches.size());
                } else {
                    std::fprintf(stderr, "FAILED: %s\n", m2Path.c_str());
                    am.shutdown();
                    return 1;
                }
                am.shutdown();
            } else {
                std::fprintf(stderr, "FAILED: cannot initialize asset manager\n");
                return 1;
            }
            return 0;
        }
    }

    // Batch convert mode: --convert-wmo converts WMO to WOB
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--convert-wmo") == 0 && i + 1 < argc) {
            std::string wmoPath = argv[++i];
            std::printf("Converting WMO→WOB: %s\n", wmoPath.c_str());
            if (dataPath.empty()) dataPath = "Data";
            wowee::pipeline::AssetManager am;
            if (am.initialize(dataPath)) {
                auto wmoData = am.readFile(wmoPath);
                if (!wmoData.empty()) {
                    auto wmoModel = wowee::pipeline::WMOLoader::load(wmoData);
                    if (wmoModel.nGroups > 0) {
                        std::string wmoBase = wmoPath;
                        if (wmoBase.size() > 4) wmoBase = wmoBase.substr(0, wmoBase.size() - 4);
                        for (uint32_t gi = 0; gi < wmoModel.nGroups; gi++) {
                            char suffix[16];
                            snprintf(suffix, sizeof(suffix), "_%03u.wmo", gi);
                            auto gd = am.readFile(wmoBase + suffix);
                            if (!gd.empty()) wowee::pipeline::WMOLoader::loadGroup(gd, wmoModel, gi);
                        }
                    }
                    auto wob = wowee::pipeline::WoweeBuildingLoader::fromWMO(wmoModel, wmoPath);
                    if (wob.isValid()) {
                        std::string outPath = wmoPath;
                        auto dot = outPath.rfind('.');
                        if (dot != std::string::npos) outPath = outPath.substr(0, dot);
                        wowee::pipeline::WoweeBuildingLoader::save(wob, "output/buildings/" + outPath);
                        std::printf("OK: output/buildings/%s.wob (%zu groups)\n",
                            outPath.c_str(), wob.groups.size());
                    } else {
                        std::fprintf(stderr, "FAILED: %s\n", wmoPath.c_str());
                        am.shutdown();
                        return 1;
                    }
                } else {
                    std::fprintf(stderr, "FAILED: file not found: %s\n", wmoPath.c_str());
                    am.shutdown();
                    return 1;
                }
                am.shutdown();
            } else {
                std::fprintf(stderr, "FAILED: cannot initialize asset manager\n");
                return 1;
            }
            return 0;
        }
        if (std::strcmp(argv[i], "--convert-dbc-json") == 0 && i + 1 < argc) {
            // Standalone DBC -> JSON sidecar conversion. Mirrors what
            // asset_extract --emit-open does for one file at a time, so
            // designers don't have to re-run a full extraction just to
            // refresh one DBC sidecar.
            std::string dbcPath = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (outPath.empty()) {
                outPath = dbcPath;
                if (outPath.size() >= 4 &&
                    outPath.substr(outPath.size() - 4) == ".dbc") {
                    outPath = outPath.substr(0, outPath.size() - 4);
                }
                outPath += ".json";
            }
            std::ifstream in(dbcPath, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "convert-dbc-json: cannot open %s\n", dbcPath.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            wowee::pipeline::DBCFile dbc;
            if (!dbc.load(bytes)) {
                std::fprintf(stderr, "convert-dbc-json: failed to parse %s\n", dbcPath.c_str());
                return 1;
            }
            // Same JSON schema asset_extract emits, so the editor's runtime
            // overlay loader picks the file up without changes.
            nlohmann::json j;
            j["format"] = "wowee-dbc-json-1.0";
            j["source"] = std::filesystem::path(dbcPath).filename().string();
            j["recordCount"] = dbc.getRecordCount();
            j["fieldCount"] = dbc.getFieldCount();
            nlohmann::json records = nlohmann::json::array();
            for (uint32_t r = 0; r < dbc.getRecordCount(); ++r) {
                nlohmann::json row = nlohmann::json::array();
                for (uint32_t f = 0; f < dbc.getFieldCount(); ++f) {
                    // Same heuristic as open_format_emitter::emitJsonFromDbc:
                    // prefer string > float > uint32 based on what the
                    // bytes plausibly are. Round-trips through loadJSON.
                    uint32_t val = dbc.getUInt32(r, f);
                    std::string s = dbc.getString(r, f);
                    if (!s.empty() && s[0] != '\0' && s.size() < 200) {
                        row.push_back(s);
                    } else {
                        float fv = dbc.getFloat(r, f);
                        if (val != 0 && fv != 0.0f && fv > -1e10f && fv < 1e10f &&
                            static_cast<uint32_t>(fv) != val) {
                            row.push_back(fv);
                        } else {
                            row.push_back(val);
                        }
                    }
                }
                records.push_back(std::move(row));
            }
            j["records"] = std::move(records);
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr, "convert-dbc-json: cannot write %s\n", outPath.c_str());
                return 1;
            }
            out << j.dump(2) << "\n";
            std::printf("Converted %s -> %s\n", dbcPath.c_str(), outPath.c_str());
            std::printf("  %u records x %u fields\n",
                        dbc.getRecordCount(), dbc.getFieldCount());
            return 0;
        }
        if (std::strcmp(argv[i], "--convert-json-dbc") == 0 && i + 1 < argc) {
            // Reverse direction — JSON sidecar back to binary DBC. Useful
            // for shipping edited content to private servers (AzerothCore /
            // TrinityCore) which only consume binary DBC. The output is
            // byte-compatible with the original Blizzard format.
            std::string jsonPath = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (outPath.empty()) {
                outPath = jsonPath;
                if (outPath.size() >= 5 &&
                    outPath.substr(outPath.size() - 5) == ".json") {
                    outPath = outPath.substr(0, outPath.size() - 5);
                }
                outPath += ".dbc";
            }
            std::ifstream in(jsonPath);
            if (!in) {
                std::fprintf(stderr, "convert-json-dbc: cannot open %s\n", jsonPath.c_str());
                return 1;
            }
            nlohmann::json doc;
            try { in >> doc; }
            catch (const std::exception& e) {
                std::fprintf(stderr, "convert-json-dbc: bad JSON in %s (%s)\n",
                             jsonPath.c_str(), e.what());
                return 1;
            }
            uint32_t fieldCount = doc.value("fieldCount", 0u);
            if (!doc.contains("records") || !doc["records"].is_array()) {
                std::fprintf(stderr, "convert-json-dbc: missing 'records' array in %s\n",
                             jsonPath.c_str());
                return 1;
            }
            const auto& records = doc["records"];
            uint32_t recordCount = static_cast<uint32_t>(records.size());
            if (fieldCount == 0 && recordCount > 0 && records[0].is_array()) {
                // Tolerate JSON files that drop fieldCount — derive from row.
                fieldCount = static_cast<uint32_t>(records[0].size());
            }
            if (fieldCount == 0) {
                std::fprintf(stderr,
                    "convert-json-dbc: cannot determine fieldCount in %s\n",
                    jsonPath.c_str());
                return 1;
            }
            uint32_t recordSize = fieldCount * 4;
            // Build records + string block. Strings are deduped: identical
            // strings reuse the same offset in the block. The first byte
            // of the block is always '\0' so offset=0 means empty string,
            // matching Blizzard's convention.
            std::vector<uint8_t> recordBytes(recordCount * recordSize, 0);
            std::vector<uint8_t> stringBlock;
            stringBlock.push_back(0);  // leading NUL — empty-string offset
            std::unordered_map<std::string, uint32_t> stringOffsets;
            stringOffsets[""] = 0;
            auto internString = [&](const std::string& s) -> uint32_t {
                if (s.empty()) return 0;
                auto it = stringOffsets.find(s);
                if (it != stringOffsets.end()) return it->second;
                uint32_t off = static_cast<uint32_t>(stringBlock.size());
                for (char c : s) stringBlock.push_back(static_cast<uint8_t>(c));
                stringBlock.push_back(0);
                stringOffsets[s] = off;
                return off;
            };
            int convertErrors = 0;
            for (uint32_t r = 0; r < recordCount; ++r) {
                const auto& row = records[r];
                if (!row.is_array() || row.size() != fieldCount) {
                    convertErrors++;
                    continue;
                }
                uint8_t* dst = recordBytes.data() + r * recordSize;
                for (uint32_t f = 0; f < fieldCount; ++f) {
                    uint32_t val = 0;
                    const auto& cell = row[f];
                    if (cell.is_string()) {
                        val = internString(cell.get<std::string>());
                    } else if (cell.is_number_float()) {
                        float fv = cell.get<float>();
                        std::memcpy(&val, &fv, 4);
                    } else if (cell.is_number_unsigned()) {
                        val = cell.get<uint32_t>();
                    } else if (cell.is_number_integer()) {
                        // Negative ints reinterpret as uint32 (DBC has no
                        // separate signed type; the consumer interprets).
                        int32_t sv = cell.get<int32_t>();
                        std::memcpy(&val, &sv, 4);
                    } else if (cell.is_boolean()) {
                        val = cell.get<bool>() ? 1u : 0u;
                    } else if (cell.is_null()) {
                        val = 0;
                    } else {
                        convertErrors++;
                    }
                    // Little-endian write — DBC is always LE per Blizzard
                    // format spec, regardless of host architecture.
                    dst[f * 4 + 0] =  val        & 0xFF;
                    dst[f * 4 + 1] = (val >>  8) & 0xFF;
                    dst[f * 4 + 2] = (val >> 16) & 0xFF;
                    dst[f * 4 + 3] = (val >> 24) & 0xFF;
                }
            }
            // Header: WDBC magic + 4 uint32s (recordCount, fieldCount,
            // recordSize, stringBlockSize).
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "convert-json-dbc: cannot write %s\n", outPath.c_str());
                return 1;
            }
            uint32_t header[5] = {
                0x43424457u,                       // 'WDBC' little-endian
                recordCount, fieldCount, recordSize,
                static_cast<uint32_t>(stringBlock.size())
            };
            out.write(reinterpret_cast<const char*>(header), sizeof(header));
            out.write(reinterpret_cast<const char*>(recordBytes.data()),
                      recordBytes.size());
            out.write(reinterpret_cast<const char*>(stringBlock.data()),
                      stringBlock.size());
            out.close();
            std::printf("Converted %s -> %s\n", jsonPath.c_str(), outPath.c_str());
            std::printf("  %u records x %u fields, %zu-byte string block\n",
                        recordCount, fieldCount, stringBlock.size());
            if (convertErrors > 0) {
                std::printf("  warning: %d cell(s) had unrecognized types\n", convertErrors);
            }
            return 0;
        }
        if (std::strcmp(argv[i], "--convert-blp-png") == 0 && i + 1 < argc) {
            // Standalone BLP -> PNG conversion. Same code path as
            // asset_extract --emit-open's per-file walker, but for one
            // texture without re-running a full extraction.
            std::string blpPath = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            }
            if (outPath.empty()) {
                outPath = blpPath;
                if (outPath.size() >= 4 &&
                    outPath.substr(outPath.size() - 4) == ".blp") {
                    outPath = outPath.substr(0, outPath.size() - 4);
                }
                outPath += ".png";
            }
            std::ifstream in(blpPath, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "convert-blp-png: cannot open %s\n", blpPath.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            auto img = wowee::pipeline::BLPLoader::load(bytes);
            if (!img.isValid()) {
                std::fprintf(stderr, "convert-blp-png: failed to decode %s\n",
                             blpPath.c_str());
                return 1;
            }
            // Same dimension/buffer-size guards as the asset_extract
            // emitter so we never feed stbi_write_png an invalid buffer.
            const size_t expected = static_cast<size_t>(img.width) * img.height * 4;
            if (img.width <= 0 || img.height <= 0 ||
                img.width > 8192 || img.height > 8192 ||
                img.data.size() < expected) {
                std::fprintf(stderr, "convert-blp-png: invalid dimensions or data (%dx%d, %zu bytes)\n",
                             img.width, img.height, img.data.size());
                return 1;
            }
            // Ensure output directory exists; fs::create_directories with
            // an empty path is a no-op so we don't need to special-case
            // 'png in cwd'.
            std::filesystem::create_directories(
                std::filesystem::path(outPath).parent_path());
            int rc = stbi_write_png(outPath.c_str(),
                                     img.width, img.height, 4,
                                     img.data.data(), img.width * 4);
            if (!rc) {
                std::fprintf(stderr, "convert-blp-png: stbi_write_png failed for %s\n",
                             outPath.c_str());
                return 1;
            }
            std::printf("Converted %s -> %s\n", blpPath.c_str(), outPath.c_str());
            std::printf("  %dx%d, %zu bytes (RGBA8)\n",
                        img.width, img.height, img.data.size());
            return 0;
        }
        if (std::strcmp(argv[i], "--migrate-wom") == 0 && i + 1 < argc) {
            // Upgrade an older WOM (v1=static, v2=animated) to WOM3 by
            // adding a default single-batch entry that covers the whole
            // mesh. WOM3 is a strict superset; tooling that consumes
            // batches (--info-batches, --export-glb per-primitive split,
            // material-aware renderers) becomes useful on previously-
            // batchless content. The save() function picks WOM3 magic
            // automatically once batches.size() > 0.
            std::string base = argv[++i];
            std::string outBase;
            if (i + 1 < argc && argv[i + 1][0] != '-') outBase = argv[++i];
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".wom")
                base = base.substr(0, base.size() - 4);
            if (!wowee::pipeline::WoweeModelLoader::exists(base)) {
                std::fprintf(stderr, "WOM not found: %s.wom\n", base.c_str());
                return 1;
            }
            if (outBase.empty()) outBase = base;
            auto wom = wowee::pipeline::WoweeModelLoader::load(base);
            if (!wom.isValid()) {
                std::fprintf(stderr, "migrate-wom: %s.wom has no geometry\n", base.c_str());
                return 1;
            }
            int oldVersion = wom.version;
            int batchesAdded = 0;
            if (wom.batches.empty()) {
                // Single batch covering the entire index range with the
                // first texture (or 0 if no textures exist). Opaque
                // blend mode + no flags — safe defaults that match how
                // the renderer was treating the whole mesh implicitly.
                wowee::pipeline::WoweeModel::Batch b;
                b.indexStart = 0;
                b.indexCount = static_cast<uint32_t>(wom.indices.size());
                b.textureIndex = wom.texturePaths.empty() ? 0 : 0;
                b.blendMode = 0;
                b.flags = 0;
                wom.batches.push_back(b);
                batchesAdded = 1;
            }
            // version field is recomputed inside save() based on
            // hasBatches/hasAnimation, so we don't need to set it here.
            if (!wowee::pipeline::WoweeModelLoader::save(wom, outBase)) {
                std::fprintf(stderr, "migrate-wom: failed to write %s.wom\n",
                             outBase.c_str());
                return 1;
            }
            // Re-load to verify the new version flag landed correctly.
            auto check = wowee::pipeline::WoweeModelLoader::load(outBase);
            std::printf("Migrated %s.wom -> %s.wom\n", base.c_str(), outBase.c_str());
            std::printf("  version: %d -> %u  batches: %zu -> %zu (added %d)\n",
                        oldVersion, check.version,
                        size_t(0), check.batches.size(), batchesAdded);
            if (batchesAdded == 0) {
                std::printf("  (already had batches; no schema change)\n");
            }
            return 0;
        }
        if (std::strcmp(argv[i], "--migrate-zone") == 0 && i + 1 < argc) {
            // Batch-runs --migrate-wom in-place on every .wom under
            // a zone directory. Idempotent (already-migrated files
            // become no-ops). Useful when wowee_editor adds a new
            // WOM3-only feature and you want to upgrade legacy zones
            // in one shot.
            std::string zoneDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(zoneDir) || !fs::is_directory(zoneDir)) {
                std::fprintf(stderr,
                    "migrate-zone: %s is not a directory\n", zoneDir.c_str());
                return 1;
            }
            int scanned = 0, upgraded = 0, alreadyV3 = 0, failed = 0;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                if (!e.is_regular_file()) continue;
                std::string ext = e.path().extension().string();
                if (ext != ".wom") continue;
                scanned++;
                std::string base = e.path().string();
                if (base.size() >= 4) base = base.substr(0, base.size() - 4);
                auto wom = wowee::pipeline::WoweeModelLoader::load(base);
                if (!wom.isValid()) { failed++; continue; }
                if (!wom.batches.empty()) { alreadyV3++; continue; }
                wowee::pipeline::WoweeModel::Batch b;
                b.indexStart = 0;
                b.indexCount = static_cast<uint32_t>(wom.indices.size());
                b.textureIndex = 0;
                b.blendMode = 0;
                b.flags = 0;
                wom.batches.push_back(b);
                if (wowee::pipeline::WoweeModelLoader::save(wom, base)) {
                    upgraded++;
                    std::printf("  upgraded: %s.wom\n", base.c_str());
                } else {
                    failed++;
                    std::fprintf(stderr, "  FAILED: %s.wom\n", base.c_str());
                }
            }
            std::printf("\nmigrate-zone: %s\n", zoneDir.c_str());
            std::printf("  scanned   : %d WOM file(s)\n", scanned);
            std::printf("  upgraded  : %d (added single-batch entry)\n", upgraded);
            std::printf("  already v3: %d (no change needed)\n", alreadyV3);
            if (failed > 0) {
                std::printf("  FAILED    : %d (see stderr)\n", failed);
            }
            return failed == 0 ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--migrate-project") == 0 && i + 1 < argc) {
            // Project-level wrapper around --migrate-zone. Walks every
            // zone in <projectDir> and upgrades legacy WOMs in-place.
            // Idempotent — already-migrated files become no-ops, safe to
            // run repeatedly.
            std::string projectDir = argv[++i];
            namespace fs = std::filesystem;
            if (!fs::exists(projectDir) || !fs::is_directory(projectDir)) {
                std::fprintf(stderr,
                    "migrate-project: %s is not a directory\n",
                    projectDir.c_str());
                return 1;
            }
            std::vector<std::string> zones;
            for (const auto& entry : fs::directory_iterator(projectDir)) {
                if (!entry.is_directory()) continue;
                if (!fs::exists(entry.path() / "zone.json")) continue;
                zones.push_back(entry.path().string());
            }
            std::sort(zones.begin(), zones.end());
            int totalScanned = 0, totalUpgraded = 0, totalAlreadyV3 = 0, totalFailed = 0;
            // Per-zone breakdown for the summary table.
            struct ZRow { std::string name; int scanned, upgraded, alreadyV3, failed; };
            std::vector<ZRow> rows;
            for (const auto& zoneDir : zones) {
                ZRow r{fs::path(zoneDir).filename().string(), 0, 0, 0, 0};
                std::error_code ec;
                for (const auto& e : fs::recursive_directory_iterator(zoneDir, ec)) {
                    if (!e.is_regular_file()) continue;
                    if (e.path().extension() != ".wom") continue;
                    r.scanned++;
                    std::string base = e.path().string();
                    if (base.size() >= 4) base = base.substr(0, base.size() - 4);
                    auto wom = wowee::pipeline::WoweeModelLoader::load(base);
                    if (!wom.isValid()) { r.failed++; continue; }
                    if (!wom.batches.empty()) { r.alreadyV3++; continue; }
                    wowee::pipeline::WoweeModel::Batch b;
                    b.indexStart = 0;
                    b.indexCount = static_cast<uint32_t>(wom.indices.size());
                    b.textureIndex = 0;
                    b.blendMode = 0;
                    b.flags = 0;
                    wom.batches.push_back(b);
                    if (wowee::pipeline::WoweeModelLoader::save(wom, base)) {
                        r.upgraded++;
                    } else {
                        r.failed++;
                    }
                }
                totalScanned += r.scanned;
                totalUpgraded += r.upgraded;
                totalAlreadyV3 += r.alreadyV3;
                totalFailed += r.failed;
                rows.push_back(r);
            }
            std::printf("migrate-project: %s\n", projectDir.c_str());
            std::printf("  zones      : %zu\n", zones.size());
            std::printf("  totals     : %d scanned, %d upgraded, %d already-v3, %d failed\n",
                        totalScanned, totalUpgraded, totalAlreadyV3, totalFailed);
            if (!rows.empty()) {
                std::printf("\n  zone                       scan  upgrade  v3  failed\n");
                for (const auto& r : rows) {
                    std::printf("  %-26s  %4d   %5d   %3d  %5d\n",
                                r.name.substr(0, 26).c_str(),
                                r.scanned, r.upgraded, r.alreadyV3, r.failed);
                }
            }
            return totalFailed == 0 ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--migrate-jsondbc") == 0 && i + 1 < argc) {
            // Auto-fix common schema problems in JSON DBC sidecars so they
            // pass --validate-jsondbc cleanly. Designed for upgrading
            // sidecars produced by older asset_extract versions or from
            // third-party tools that omit fields the runtime now expects:
            //   - missing 'format' tag → add 'wowee-dbc-json-1.0'
            //   - missing 'source' field → derive from filename
            //   - missing 'fieldCount' → infer from first row
            //   - recordCount mismatch → recompute from actual records[]
            // Wrong-width rows are not silently fixed (data loss risk);
            // they're surfaced as warnings so the user can decide.
            std::string path = argv[++i];
            std::string outPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
            if (outPath.empty()) outPath = path;  // in-place
            std::ifstream in(path);
            if (!in) {
                std::fprintf(stderr,
                    "migrate-jsondbc: cannot open %s\n", path.c_str());
                return 1;
            }
            nlohmann::json doc;
            try { in >> doc; }
            catch (const std::exception& e) {
                std::fprintf(stderr,
                    "migrate-jsondbc: bad JSON in %s (%s)\n",
                    path.c_str(), e.what());
                return 1;
            }
            in.close();
            if (!doc.is_object()) {
                std::fprintf(stderr,
                    "migrate-jsondbc: top-level value is not an object\n");
                return 1;
            }
            int fixes = 0;
            if (!doc.contains("format") || !doc["format"].is_string()) {
                doc["format"] = "wowee-dbc-json-1.0";
                fixes++;
                std::printf("  added: format = 'wowee-dbc-json-1.0'\n");
            } else if (doc["format"] != "wowee-dbc-json-1.0") {
                std::printf("  retained existing format: '%s' (not changed)\n",
                            doc["format"].get<std::string>().c_str());
            }
            if (!doc.contains("source") || !doc["source"].is_string() ||
                doc["source"].get<std::string>().empty()) {
                // Derive from input path's stem + .dbc — best-effort
                // matching the convention asset_extract uses.
                std::string stem = std::filesystem::path(path).stem().string();
                doc["source"] = stem + ".dbc";
                fixes++;
                std::printf("  added: source = '%s'\n",
                            doc["source"].get<std::string>().c_str());
            }
            // recordCount + fieldCount are non-negotiable for re-import.
            if (!doc.contains("records") || !doc["records"].is_array()) {
                std::fprintf(stderr,
                    "migrate-jsondbc: 'records' missing or not an array — cannot fix\n");
                return 1;
            }
            const auto& records = doc["records"];
            uint32_t actualCount = static_cast<uint32_t>(records.size());
            uint32_t headerCount = doc.value("recordCount", 0u);
            if (headerCount != actualCount) {
                doc["recordCount"] = actualCount;
                fixes++;
                std::printf("  fixed: recordCount %u -> %u (matches actual)\n",
                            headerCount, actualCount);
            }
            // Infer fieldCount from first row if missing.
            if (!doc.contains("fieldCount") ||
                !doc["fieldCount"].is_number_integer()) {
                if (!records.empty() && records[0].is_array()) {
                    uint32_t inferred = static_cast<uint32_t>(records[0].size());
                    doc["fieldCount"] = inferred;
                    fixes++;
                    std::printf("  inferred: fieldCount = %u (from first row)\n",
                                inferred);
                }
            }
            // Surface wrong-width rows as warnings (no auto-fix).
            uint32_t fc = doc.value("fieldCount", 0u);
            int badRows = 0;
            for (size_t r = 0; r < records.size(); ++r) {
                if (records[r].is_array() && records[r].size() != fc) {
                    if (++badRows <= 3) {
                        std::printf("  WARN: row %zu has %zu cells, expected %u\n",
                                    r, records[r].size(), fc);
                    }
                }
            }
            if (badRows > 3) {
                std::printf("  WARN: ... and %d more wrong-width rows\n",
                            badRows - 3);
            }
            std::ofstream out(outPath);
            if (!out) {
                std::fprintf(stderr,
                    "migrate-jsondbc: cannot write %s\n", outPath.c_str());
                return 1;
            }
            out << doc.dump(2) << "\n";
            out.close();
            std::printf("Migrated %s -> %s\n", path.c_str(), outPath.c_str());
            std::printf("  fixes applied: %d\n", fixes);
            if (badRows > 0) {
                std::printf("  warnings     : %d wrong-width rows (NOT auto-fixed)\n",
                            badRows);
            }
            return 0;
        }
    }

    if (dataPath.empty()) {
        dataPath = "Data";
        LOG_INFO("No --data path specified, using default: ", dataPath);
    }

    wowee::editor::EditorApp app;
    if (!app.initialize(dataPath)) {
        LOG_ERROR("Failed to initialize editor");
        return 1;
    }

    if (!adtMap.empty()) {
        app.loadADT(adtMap, adtX, adtY);
    }

    app.run();
    app.shutdown();

    return 0;
}
