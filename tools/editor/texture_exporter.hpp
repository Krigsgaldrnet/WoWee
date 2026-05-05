#pragma once

#include "pipeline/adt_loader.hpp"
#include <string>
#include <vector>
#include <unordered_set>

namespace wowee {
namespace pipeline { class AssetManager; }

namespace editor {

class TextureExporter {
public:
    // Collect all texture paths referenced by the terrain
    static std::vector<std::string> collectUsedTextures(const pipeline::ADTTerrain& terrain);

    // Export all used textures as PNG to an output directory
    // Returns count of successfully exported textures
    static int exportTexturesAsPng(pipeline::AssetManager* am,
                                    const std::vector<std::string>& texturePaths,
                                    const std::string& outputDir);
};

} // namespace editor
} // namespace wowee
