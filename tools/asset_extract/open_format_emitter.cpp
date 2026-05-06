#include "open_format_emitter.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_loader.hpp"

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

void emitOpenFormats(const std::string& rootDir,
                     bool emitPng, bool emitJsonDbc,
                     OpenFormatStats& stats) {
    if (!fs::exists(rootDir)) return;
    if (!emitPng && !emitJsonDbc) return;

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
        }
    }
}

} // namespace tools
} // namespace wowee
