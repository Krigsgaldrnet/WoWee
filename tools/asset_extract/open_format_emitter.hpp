#pragma once

// Convert proprietary Blizzard formats to wowee open formats as a
// post-extraction pass. Each emit*() reads a single file from disk and
// writes the open-format equivalent SIDE-BY-SIDE — the original file is
// left untouched so private servers (AzerothCore/TrinityCore) that
// expect .blp/.dbc/.m2/.wmo continue to work unchanged.
//
// Naming: foo.blp → foo.png, foo.dbc → foo.json, foo.m2 → foo.wom,
//         foo.wmo → foo.wob.

#include <string>
#include <cstdint>

namespace wowee {
namespace tools {

struct OpenFormatStats {
    uint32_t pngOk = 0, pngFail = 0;
    uint32_t jsonDbcOk = 0, jsonDbcFail = 0;
    uint32_t womOk = 0, womFail = 0;
    uint32_t wobOk = 0, wobFail = 0;
};

// Convert one BLP file on disk to a PNG side-file.
// Returns true on success; false on missing file, invalid BLP, or PNG write error.
bool emitPngFromBlp(const std::string& blpPath, const std::string& pngPath);

// Convert one DBC file on disk to a JSON side-file.
// JSON layout: {format, source, recordCount, fieldCount, records:[[...], ...]}
// — same schema the editor's runtime DBC loader (loadJSON) accepts so
// the output drops into custom_zones/<zone>/data/ directly.
bool emitJsonFromDbc(const std::string& dbcPath, const std::string& jsonPath);

// Convert one M2 file on disk to a WOM side-file. Auto-locates and merges
// the matching <base>00.skin if present (WotLK+ models store geometry there).
bool emitWomFromM2(const std::string& m2Path, const std::string& womBase);

// Convert one WMO file on disk to a WOB side-file. Auto-locates and merges
// matching <base>_NNN.wmo group files if present.
bool emitWobFromWmo(const std::string& wmoPath, const std::string& wobBase);

// Walk an extracted-asset directory and emit open-format side-files for
// every requested format. Counts accumulated into stats.
void emitOpenFormats(const std::string& rootDir,
                     bool emitPng, bool emitJsonDbc,
                     bool emitWom, bool emitWob,
                     OpenFormatStats& stats);

} // namespace tools
} // namespace wowee
