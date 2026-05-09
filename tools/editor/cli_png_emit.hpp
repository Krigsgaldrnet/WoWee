#pragma once

#include "stb_image_write.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

// Shared PNG-write wrapper used by every --gen-texture-* handler.
// Calls stbi_write_png with the standard RGB/24-bit/W*3-stride
// arguments and reports a stderr message on failure. Returns true
// on success so the caller can do
// `if (!savePngOrError(...)) return 1;`.
//
// 58 sites in cli_gen_texture.cpp open-coded the same 5-line
// "if write fails, fprintf stderr and return 1" block before
// extraction.
inline bool savePngOrError(const std::string& outPath, int W, int H,
                           const std::vector<uint8_t>& pixels,
                           const char* cmdName) {
    if (stbi_write_png(outPath.c_str(), W, H, 3,
                       pixels.data(), W * 3)) {
        return true;
    }
    std::fprintf(stderr, "%s: stbi_write_png failed for %s\n",
                 cmdName, outPath.c_str());
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
