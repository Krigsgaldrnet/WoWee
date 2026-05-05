#pragma once

#include "pipeline/adt_loader.hpp"
#include <string>

namespace wowee {
namespace editor {

// Wowee Open Terrain Format (.wot)
// JSON + binary heightmap — no Blizzard formats, fully open
class WoweeTerrain {
public:
    // Export terrain to open format: .wot (JSON metadata) + .whm (binary heightmap)
    static bool exportOpen(const pipeline::ADTTerrain& terrain,
                           const std::string& basePath, int tileX, int tileY);

    // Export normal map as PNG (129x129 RGB)
    static bool exportNormalMap(const pipeline::ADTTerrain& terrain,
                                const std::string& path);

    // Import terrain from open format back to ADTTerrain
    static bool importOpen(const std::string& basePath, pipeline::ADTTerrain& terrain);
};

} // namespace editor
} // namespace wowee
