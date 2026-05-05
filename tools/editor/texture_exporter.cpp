#include "texture_exporter.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include <filesystem>
#include <algorithm>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace wowee {
namespace editor {

std::vector<std::string> TextureExporter::collectUsedTextures(const pipeline::ADTTerrain& terrain) {
    std::unordered_set<std::string> unique;
    for (const auto& tex : terrain.textures)
        unique.insert(tex);
    std::vector<std::string> result(unique.begin(), unique.end());
    std::sort(result.begin(), result.end());
    return result;
}

int TextureExporter::exportTexturesAsPng(pipeline::AssetManager* am,
                                          const std::vector<std::string>& texturePaths,
                                          const std::string& outputDir) {
    namespace fs = std::filesystem;
    int exported = 0;

    for (const auto& texPath : texturePaths) {
        auto blpImage = am->loadTexture(texPath);
        if (!blpImage.isValid()) {
            LOG_WARNING("Texture not found or invalid: ", texPath);
            continue;
        }

        // Build output path: replace backslashes, change .blp to .png
        std::string outPath = texPath;
        std::replace(outPath.begin(), outPath.end(), '\\', '/');
        // Lowercase
        std::transform(outPath.begin(), outPath.end(), outPath.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        // Change extension
        auto dotPos = outPath.rfind('.');
        if (dotPos != std::string::npos)
            outPath = outPath.substr(0, dotPos) + ".png";

        std::string fullPath = outputDir + "/" + outPath;
        fs::create_directories(fs::path(fullPath).parent_path());

        // Write RGBA data as PNG
        if (stbi_write_png(fullPath.c_str(), blpImage.width, blpImage.height, 4,
                           blpImage.data.data(), blpImage.width * 4)) {
            exported++;
        } else {
            LOG_WARNING("Failed to write PNG: ", fullPath);
        }
    }

    LOG_INFO("Exported ", exported, "/", texturePaths.size(), " textures as PNG to ", outputDir);
    return exported;
}

} // namespace editor
} // namespace wowee
