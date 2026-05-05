#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace wowee {
namespace editor {

struct ContentPackInfo {
    std::string name;
    std::string author;
    std::string description;
    std::string version = "1.0";
    uint32_t mapId = 9000;
    std::string format = "wcp-1.0";

    struct FileEntry {
        std::string path;        // path inside pack
        std::string category;    // terrain, object, creature, quest, texture, model
        uint64_t size = 0;
    };
    std::vector<FileEntry> files;
};

class ContentPacker {
public:
    // Pack all zone data from output directory into a .wcp file
    static bool packZone(const std::string& outputDir, const std::string& mapName,
                          const std::string& destPath, const ContentPackInfo& info);

    // Unpack a .wcp file to a directory
    static bool unpackZone(const std::string& wcpPath, const std::string& destDir);

    // Read pack info without extracting
    static bool readInfo(const std::string& wcpPath, ContentPackInfo& info);
};

} // namespace editor
} // namespace wowee
