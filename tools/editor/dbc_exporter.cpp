#include "dbc_exporter.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <filesystem>

namespace wowee {
namespace editor {

bool DBCExporter::exportAsJson(pipeline::AssetManager* am,
                                const std::string& dbcName,
                                const std::string& outputPath) {
    auto dbc = am->loadDBC(dbcName);
    if (!dbc || !dbc->isLoaded()) return false;

    namespace fs = std::filesystem;
    fs::create_directories(fs::path(outputPath).parent_path());

    std::ofstream f(outputPath);
    if (!f) return false;

    f << "{\n";
    f << "  \"format\": \"wowee-dbc-json-1.0\",\n";
    f << "  \"source\": \"" << dbcName << "\",\n";
    f << "  \"recordCount\": " << dbc->getRecordCount() << ",\n";
    f << "  \"fieldCount\": " << dbc->getFieldCount() << ",\n";
    f << "  \"records\": [\n";

    for (uint32_t i = 0; i < dbc->getRecordCount(); i++) {
        f << "    [";
        for (uint32_t j = 0; j < dbc->getFieldCount(); j++) {
            // Try to detect string fields vs numeric
            uint32_t val = dbc->getUInt32(i, j);
            // Check if it looks like a string offset (points into string block)
            std::string strVal = dbc->getString(i, j);
            if (!strVal.empty() && strVal[0] != '\0' && strVal.size() < 200) {
                // Escape quotes in string
                std::string escaped;
                for (char c : strVal) {
                    if (c == '"') escaped += "\\\"";
                    else if (c == '\\') escaped += "\\\\";
                    else if (c == '\n') escaped += "\\n";
                    else escaped += c;
                }
                f << "\"" << escaped << "\"";
            } else {
                // Check if it's a float
                float fval = dbc->getFloat(i, j);
                if (val != 0 && fval != 0.0f && fval > -1e10f && fval < 1e10f &&
                    static_cast<uint32_t>(fval) != val) {
                    f << fval;
                } else {
                    f << val;
                }
            }
            if (j + 1 < dbc->getFieldCount()) f << ", ";
        }
        f << "]" << (i + 1 < dbc->getRecordCount() ? "," : "") << "\n";
    }

    f << "  ]\n}\n";

    LOG_INFO("DBC exported as JSON: ", dbcName, " → ", outputPath,
             " (", dbc->getRecordCount(), " records)");
    return true;
}

int DBCExporter::exportZoneDBCs(pipeline::AssetManager* am,
                                  const std::string& outputDir) {
    // Zone-relevant DBCs for custom content
    const char* zoneDBCs[] = {
        "AreaTable.dbc",
        "Map.dbc",
        "Light.dbc",
        "LightParams.dbc",
        "ZoneMusic.dbc",
        "SoundAmbience.dbc",
        "GroundEffectTexture.dbc",
        "GroundEffectDoodad.dbc",
        "LiquidType.dbc"
    };

    int exported = 0;
    for (const char* name : zoneDBCs) {
        std::string baseName(name);
        auto dot = baseName.rfind('.');
        if (dot != std::string::npos) baseName = baseName.substr(0, dot);

        std::string outPath = outputDir + "/" + baseName + ".json";
        if (exportAsJson(am, name, outPath))
            exported++;
    }

    LOG_INFO("Exported ", exported, " zone-relevant DBCs as JSON to ", outputDir);
    return exported;
}

} // namespace editor
} // namespace wowee
