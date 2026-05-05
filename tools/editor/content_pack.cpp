#include "content_pack.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <filesystem>
#include <cstring>

namespace wowee {
namespace editor {

// WCP file format (simple concatenated archive):
// Header: "WCP1" (4 bytes) + fileCount (4) + infoJsonSize (4)
// Info JSON block (infoJsonSize bytes)
// File table: for each file: pathLen(2) + path(pathLen) + dataSize(4)
// File data: concatenated file contents

static constexpr uint32_t WCP_MAGIC = 0x31504357; // "WCP1"

bool ContentPacker::packZone(const std::string& outputDir, const std::string& mapName,
                              const std::string& destPath, const ContentPackInfo& info) {
    namespace fs = std::filesystem;
    std::string srcDir = outputDir + "/" + mapName;

    if (!fs::exists(srcDir)) {
        LOG_ERROR("Source directory not found: ", srcDir);
        return false;
    }

    // Collect all files
    std::vector<std::pair<std::string, std::string>> files; // relative path, full path
    for (auto& entry : fs::recursive_directory_iterator(srcDir)) {
        if (!entry.is_regular_file()) continue;
        std::string rel = fs::relative(entry.path(), srcDir).string();
        files.push_back({rel, entry.path().string()});
    }

    if (files.empty()) {
        LOG_ERROR("No files to pack in: ", srcDir);
        return false;
    }

    // Build info JSON
    std::string infoJson = "{\n";
    infoJson += "  \"format\": \"" + info.format + "\",\n";
    infoJson += "  \"name\": \"" + info.name + "\",\n";
    infoJson += "  \"author\": \"" + info.author + "\",\n";
    infoJson += "  \"description\": \"" + info.description + "\",\n";
    infoJson += "  \"version\": \"" + info.version + "\",\n";
    infoJson += "  \"mapId\": " + std::to_string(info.mapId) + ",\n";
    infoJson += "  \"fileCount\": " + std::to_string(files.size()) + ",\n";
    infoJson += "  \"files\": [\n";
    for (size_t i = 0; i < files.size(); i++) {
        auto fsize = fs::file_size(files[i].second);
        infoJson += "    {\"path\": \"" + files[i].first + "\", \"size\": " + std::to_string(fsize) + "}";
        if (i + 1 < files.size()) infoJson += ",";
        infoJson += "\n";
    }
    infoJson += "  ]\n}\n";

    // Write WCP file
    std::ofstream out(destPath, std::ios::binary);
    if (!out) {
        LOG_ERROR("Failed to create pack file: ", destPath);
        return false;
    }

    // Header
    out.write(reinterpret_cast<const char*>(&WCP_MAGIC), 4);
    uint32_t fileCount = static_cast<uint32_t>(files.size());
    out.write(reinterpret_cast<const char*>(&fileCount), 4);
    uint32_t infoSize = static_cast<uint32_t>(infoJson.size());
    out.write(reinterpret_cast<const char*>(&infoSize), 4);

    // Info JSON
    out.write(infoJson.data(), infoJson.size());

    // File table + data
    for (const auto& [rel, full] : files) {
        uint16_t pathLen = static_cast<uint16_t>(rel.size());
        out.write(reinterpret_cast<const char*>(&pathLen), 2);
        out.write(rel.data(), pathLen);

        std::ifstream fin(full, std::ios::binary | std::ios::ate);
        uint32_t dataSize = static_cast<uint32_t>(fin.tellg());
        fin.seekg(0);
        out.write(reinterpret_cast<const char*>(&dataSize), 4);

        std::vector<char> buf(dataSize);
        fin.read(buf.data(), dataSize);
        out.write(buf.data(), dataSize);
    }

    LOG_INFO("Content pack created: ", destPath, " (", files.size(), " files, ",
             out.tellp(), " bytes)");
    return true;
}

bool ContentPacker::unpackZone(const std::string& wcpPath, const std::string& destDir) {
    std::ifstream in(wcpPath, std::ios::binary);
    if (!in) return false;

    uint32_t magic;
    in.read(reinterpret_cast<char*>(&magic), 4);
    if (magic != WCP_MAGIC) {
        LOG_ERROR("Not a WCP file: ", wcpPath);
        return false;
    }

    uint32_t fileCount, infoSize;
    in.read(reinterpret_cast<char*>(&fileCount), 4);
    in.read(reinterpret_cast<char*>(&infoSize), 4);

    // Skip info JSON
    in.seekg(infoSize, std::ios::cur);

    namespace fs = std::filesystem;
    fs::create_directories(destDir);

    for (uint32_t i = 0; i < fileCount; i++) {
        uint16_t pathLen;
        in.read(reinterpret_cast<char*>(&pathLen), 2);
        std::string path(pathLen, '\0');
        in.read(path.data(), pathLen);

        uint32_t dataSize;
        in.read(reinterpret_cast<char*>(&dataSize), 4);

        std::vector<char> data(dataSize);
        in.read(data.data(), dataSize);

        std::string fullPath = destDir + "/" + path;
        fs::create_directories(fs::path(fullPath).parent_path());
        std::ofstream fout(fullPath, std::ios::binary);
        fout.write(data.data(), dataSize);
    }

    LOG_INFO("Content pack extracted to: ", destDir, " (", fileCount, " files)");
    return true;
}

bool ContentPacker::readInfo(const std::string& wcpPath, ContentPackInfo& info) {
    std::ifstream in(wcpPath, std::ios::binary);
    if (!in) return false;

    uint32_t magic;
    in.read(reinterpret_cast<char*>(&magic), 4);
    if (magic != WCP_MAGIC) return false;

    uint32_t fileCount, infoSize;
    in.read(reinterpret_cast<char*>(&fileCount), 4);
    in.read(reinterpret_cast<char*>(&infoSize), 4);

    std::string json(infoSize, '\0');
    in.read(json.data(), infoSize);

    // Parse basic fields
    auto findStr = [&](const std::string& key) -> std::string {
        auto pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = json.find('"', json.find(':', pos) + 1);
        if (pos == std::string::npos) return "";
        auto end = json.find('"', pos + 1);
        return json.substr(pos + 1, end - pos - 1);
    };

    info.name = findStr("name");
    info.author = findStr("author");
    info.description = findStr("description");
    info.version = findStr("version");
    info.format = findStr("format");

    return true;
}

ContentPacker::ValidationResult ContentPacker::validateZone(const std::string& zoneDir) {
    namespace fs = std::filesystem;
    ValidationResult r;
    if (!fs::exists(zoneDir)) return r;

    for (auto& entry : fs::recursive_directory_iterator(zoneDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::string name = entry.path().filename().string();
        if (ext == ".wot") r.hasWot = true;
        if (ext == ".whm") r.hasWhm = true;
        if (ext == ".wom") r.hasWom = true;
        if (ext == ".png") r.hasPng = true;
        if (name == "zone.json") r.hasZoneJson = true;
        if (name == "creatures.json") r.hasCreatures = true;
        if (name == "quests.json") r.hasQuests = true;
        if (name == "objects.json") r.hasObjects = true;
    }
    return r;
}

int ContentPacker::ValidationResult::openFormatScore() const {
    int score = 0;
    if (hasWot) score++; if (hasWhm) score++; if (hasZoneJson) score++;
    if (hasPng) score++; if (hasWom) score++;
    return score; // max 5 for fully open
}

std::string ContentPacker::ValidationResult::summary() const {
    std::string s;
    s += hasWot ? "WOT " : ""; s += hasWhm ? "WHM " : "";
    s += hasZoneJson ? "zone.json " : ""; s += hasPng ? "PNG " : "";
    s += hasWom ? "WOM " : ""; s += hasCreatures ? "creatures " : "";
    s += hasQuests ? "quests " : ""; s += hasObjects ? "objects " : "";
    return s.empty() ? "(empty)" : s;
}

} // namespace editor
} // namespace wowee
