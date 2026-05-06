#include "open_format_emitter.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/wowee_model.hpp"
#include "pipeline/wowee_building.hpp"
#include "pipeline/wowee_collision.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/adt_loader.hpp"

#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace wowee {
namespace tools {

namespace fs = std::filesystem;

static std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    if (sz <= 0) return {};
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

bool emitPngFromBlp(const std::string& blpPath, const std::string& pngPath) {
    auto bytes = readBytes(blpPath);
    if (bytes.empty()) return false;
    auto img = pipeline::BLPLoader::load(bytes);
    if (!img.isValid()) return false;
    // Same dimension/buffer-size sanity guards as the editor's texture
    // exporter so we never feed stbi_write_png an invalid buffer.
    const size_t expected = static_cast<size_t>(img.width) * img.height * 4;
    if (img.width <= 0 || img.height <= 0 ||
        img.width > 8192 || img.height > 8192 ||
        img.data.size() < expected) {
        return false;
    }
    fs::create_directories(fs::path(pngPath).parent_path());
    return stbi_write_png(pngPath.c_str(), img.width, img.height, 4,
                          img.data.data(), img.width * 4) != 0;
}

bool emitJsonFromDbc(const std::string& dbcPath, const std::string& jsonPath) {
    auto bytes = readBytes(dbcPath);
    if (bytes.empty()) return false;
    pipeline::DBCFile dbc;
    if (!dbc.load(bytes)) return false;

    nlohmann::json j;
    j["format"] = "wowee-dbc-json-1.0";
    // Source field carries the original DBC name (without dirs) so the
    // editor's runtime DBC overlay system can match it to the right slot.
    j["source"] = fs::path(dbcPath).filename().string();
    j["recordCount"] = dbc.getRecordCount();
    j["fieldCount"] = dbc.getFieldCount();

    nlohmann::json records = nlohmann::json::array();
    for (uint32_t i = 0; i < dbc.getRecordCount(); ++i) {
        nlohmann::json row = nlohmann::json::array();
        for (uint32_t f = 0; f < dbc.getFieldCount(); ++f) {
            // Same heuristic the editor's DBCExporter::exportAsJson uses:
            // prefer string if printable + non-empty, else float if it
            // looks like one, else uint32. The runtime loadJSON accepts
            // any of the three branches.
            uint32_t val = dbc.getUInt32(i, f);
            std::string s = dbc.getString(i, f);
            if (!s.empty() && s[0] != '\0' && s.size() < 200) {
                row.push_back(s);
            } else {
                float fv = dbc.getFloat(i, f);
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

    fs::create_directories(fs::path(jsonPath).parent_path());
    std::ofstream out(jsonPath);
    if (!out) return false;
    out << j.dump(2) << "\n";
    return true;
}

bool emitWomFromM2(const std::string& m2Path, const std::string& womBase) {
    auto m2Bytes = readBytes(m2Path);
    if (m2Bytes.empty()) return false;
    // WotLK+ M2s store the actual geometry in <base>00.skin; merge it if
    // it sits next to the .m2 (usual case after extraction).
    std::vector<uint8_t> skinBytes;
    {
        std::string skinPath = m2Path;
        auto dot = skinPath.rfind('.');
        if (dot != std::string::npos)
            skinPath = skinPath.substr(0, dot) + "00.skin";
        skinBytes = readBytes(skinPath);
    }
    auto wom = pipeline::WoweeModelLoader::fromM2Bytes(m2Bytes, skinBytes);
    if (!wom.isValid()) return false;
    return pipeline::WoweeModelLoader::save(wom, womBase);
}

// Inline WHM+WOT writer. Mirrors the structure of WoweeTerrain::exportOpen
// in the editor but stripped to the bytes the runtime needs (no PNG
// previews, no normal map). Keeps the asset extractor independent of
// the editor target.
static bool writeWhmWot(const pipeline::ADTTerrain& terrain,
                         const std::string& outBase, int tileX, int tileY) {
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(outBase).parent_path());

    // .whm — binary heightmap, fixed 256 chunks * 145 floats
    {
        std::ofstream f(outBase + ".whm", std::ios::binary);
        if (!f) return false;
        uint32_t magic = 0x314D4857; // "WHM1"
        uint32_t chunks = 256, verts = 145;
        f.write(reinterpret_cast<const char*>(&magic), 4);
        f.write(reinterpret_cast<const char*>(&chunks), 4);
        f.write(reinterpret_cast<const char*>(&verts), 4);
        for (int ci = 0; ci < 256; ci++) {
            const auto& chunk = terrain.chunks[ci];
            float base = std::isfinite(chunk.position[2]) ? chunk.position[2] : 0.0f;
            f.write(reinterpret_cast<const char*>(&base), 4);
            float clean[145];
            for (int v = 0; v < 145; v++) {
                clean[v] = chunk.heightMap.heights[v];
                if (!std::isfinite(clean[v])) clean[v] = 0.0f;
            }
            f.write(reinterpret_cast<const char*>(clean), 145 * 4);
            uint32_t alphaSize = std::min<uint32_t>(
                static_cast<uint32_t>(chunk.alphaMap.size()), 65536);
            f.write(reinterpret_cast<const char*>(&alphaSize), 4);
            if (alphaSize > 0)
                f.write(reinterpret_cast<const char*>(chunk.alphaMap.data()), alphaSize);
        }
    }

    // .wot — JSON metadata (textures + chunkLayers + water + placements)
    {
        nlohmann::json j;
        j["format"] = "wot-1.0";
        j["editor"] = "asset_extract-1.0.0";
        j["tileX"] = tileX;
        j["tileY"] = tileY;
        j["chunkGrid"] = {16, 16};
        j["vertsPerChunk"] = 145;
        j["heightmapFile"] = fs::path(outBase + ".whm").filename().string();

        nlohmann::json texArr = nlohmann::json::array();
        for (const auto& tex : terrain.textures) texArr.push_back(tex);
        j["textures"] = texArr;

        nlohmann::json chunkArr = nlohmann::json::array();
        for (int ci = 0; ci < 256; ci++) {
            const auto& chunk = terrain.chunks[ci];
            nlohmann::json cl;
            nlohmann::json layerIds = nlohmann::json::array();
            for (const auto& layer : chunk.layers) layerIds.push_back(layer.textureId);
            cl["layers"] = layerIds;
            cl["holes"] = chunk.holes;
            chunkArr.push_back(cl);
        }
        j["chunkLayers"] = chunkArr;

        nlohmann::json waterArr = nlohmann::json::array();
        for (int ci = 0; ci < 256; ci++) {
            const auto& w = terrain.waterData[ci];
            if (w.hasWater()) {
                float h = std::isfinite(w.layers[0].maxHeight) ? w.layers[0].maxHeight : 0.0f;
                waterArr.push_back({{"chunk", ci},
                                     {"type", w.layers[0].liquidType},
                                     {"height", h}});
            } else {
                waterArr.push_back(nullptr);
            }
        }
        j["water"] = waterArr;

        auto san = [](float x) { return std::isfinite(x) ? x : 0.0f; };
        nlohmann::json doodadNames = nlohmann::json::array();
        for (const auto& n : terrain.doodadNames) doodadNames.push_back(n);
        j["doodadNames"] = doodadNames;
        nlohmann::json doodads = nlohmann::json::array();
        for (const auto& dp : terrain.doodadPlacements) {
            doodads.push_back({
                {"nameId", dp.nameId}, {"uniqueId", dp.uniqueId},
                {"pos", {san(dp.position[0]), san(dp.position[1]), san(dp.position[2])}},
                {"rot", {san(dp.rotation[0]), san(dp.rotation[1]), san(dp.rotation[2])}},
                {"scale", dp.scale}, {"flags", dp.flags}
            });
        }
        j["doodads"] = doodads;

        nlohmann::json wmoNames = nlohmann::json::array();
        for (const auto& n : terrain.wmoNames) wmoNames.push_back(n);
        j["wmoNames"] = wmoNames;
        nlohmann::json wmos = nlohmann::json::array();
        for (const auto& wp : terrain.wmoPlacements) {
            wmos.push_back({
                {"nameId", wp.nameId}, {"uniqueId", wp.uniqueId},
                {"pos", {san(wp.position[0]), san(wp.position[1]), san(wp.position[2])}},
                {"rot", {san(wp.rotation[0]), san(wp.rotation[1]), san(wp.rotation[2])}},
                {"flags", wp.flags}, {"doodadSet", wp.doodadSet}
            });
        }
        j["wmos"] = wmos;

        std::ofstream f(outBase + ".wot");
        if (!f) return false;
        f << j.dump(2) << "\n";
    }
    return true;
}

bool emitTerrainFromAdt(const std::string& adtPath, const std::string& outBase) {
    auto bytes = readBytes(adtPath);
    if (bytes.empty()) return false;
    auto terrain = pipeline::ADTLoader::load(bytes);
    if (!terrain.loaded) return false;

    // Parse "<map>_<x>_<y>.adt" tile coords from the filename so the WOT
    // can record them; fall back to (32,32) if the layout is unexpected.
    int tileX = 32, tileY = 32;
    {
        std::string stem = fs::path(adtPath).stem().string();
        auto last = stem.rfind('_');
        auto prev = (last != std::string::npos) ? stem.rfind('_', last - 1) : std::string::npos;
        if (last != std::string::npos && prev != std::string::npos) {
            try {
                tileX = std::stoi(stem.substr(prev + 1, last - prev - 1));
                tileY = std::stoi(stem.substr(last + 1));
            } catch (...) {}
        }
    }
    terrain.coord.x = tileX;
    terrain.coord.y = tileY;

    if (!writeWhmWot(terrain, outBase, tileX, tileY)) return false;

    // Also build a terrain-only WOC (collision mesh) so the runtime can
    // do walkability queries without re-deriving from the heightmap.
    auto col = pipeline::WoweeCollisionBuilder::fromTerrain(terrain);
    pipeline::WoweeCollisionBuilder::save(col, outBase + ".woc");
    return true;
}

bool emitWobFromWmo(const std::string& wmoPath, const std::string& wobBase) {
    auto rootBytes = readBytes(wmoPath);
    if (rootBytes.empty()) return false;
    auto wmo = pipeline::WMOLoader::load(rootBytes);
    if (wmo.nGroups == 0) return false;
    // Merge group files <base>_NNN.wmo for groups that have them.
    std::string base = wmoPath;
    if (base.size() > 4) base = base.substr(0, base.size() - 4);
    for (uint32_t gi = 0; gi < wmo.nGroups; ++gi) {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "_%03u.wmo", gi);
        auto gd = readBytes(base + suffix);
        if (!gd.empty()) pipeline::WMOLoader::loadGroup(gd, wmo, gi);
    }
    auto bld = pipeline::WoweeBuildingLoader::fromWMO(
        wmo, fs::path(wmoPath).stem().string());
    if (!bld.isValid()) return false;
    return pipeline::WoweeBuildingLoader::save(bld, wobBase);
}

void emitOpenFormats(const std::string& rootDir,
                     bool emitPng, bool emitJsonDbc,
                     bool emitWom, bool emitWob,
                     bool emitTerrain,
                     OpenFormatStats& stats) {
    if (!fs::exists(rootDir)) return;
    if (!emitPng && !emitJsonDbc && !emitWom && !emitWob && !emitTerrain) return;

    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    for (auto& entry : fs::recursive_directory_iterator(rootDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = lower(entry.path().extension().string());
        std::string base = entry.path().string();
        if (base.size() > ext.size())
            base = base.substr(0, base.size() - ext.size());

        if (emitPng && ext == ".blp") {
            if (emitPngFromBlp(entry.path().string(), base + ".png")) {
                stats.pngOk++;
            } else {
                stats.pngFail++;
            }
        } else if (emitJsonDbc && ext == ".dbc") {
            if (emitJsonFromDbc(entry.path().string(), base + ".json")) {
                stats.jsonDbcOk++;
            } else {
                stats.jsonDbcFail++;
            }
        } else if (emitWom && ext == ".m2") {
            if (emitWomFromM2(entry.path().string(), base)) stats.womOk++;
            else stats.womFail++;
        } else if (emitWob && ext == ".wmo") {
            // Skip group sub-files (<base>_NNN.wmo) — those get merged
            // into the root WMO during conversion.
            std::string fname = entry.path().filename().string();
            auto under = fname.rfind('_');
            bool isGroup = (under != std::string::npos &&
                            fname.size() - under == 8); // "_NNN.wmo" suffix
            if (!isGroup) {
                if (emitWobFromWmo(entry.path().string(), base)) stats.wobOk++;
                else stats.wobFail++;
            }
        } else if (emitTerrain && ext == ".adt") {
            if (emitTerrainFromAdt(entry.path().string(), base)) {
                stats.whmOk++;
                stats.wocOk++;
            } else {
                stats.whmFail++;
                stats.wocFail++;
            }
        }
    }
}

} // namespace tools
} // namespace wowee
