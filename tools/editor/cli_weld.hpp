#pragma once

#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

// Vertex weld pass shared by --info-mesh-stats / --info-wob-stats /
// --bake-wom-collision. Positions are quantized onto a 1/eps grid;
// every vertex sharing a cell with a previously-seen vertex is
// remapped to that vertex's index. Returns canon[v] giving the
// canonical (lowest-index) representative of v's equivalence class
// and writes the count of distinct cells to `uniqueOut`.
//
// Implementation uses std::map<tuple<int64,int64,int64>, uint32_t>
// for exact equality on the quantized key — a hash-based key would
// risk false-positive collisions that incorrectly merge distinct
// corners (e.g. a unit cube's 8 corners all hashing to 2 buckets).
std::vector<uint32_t> buildWeldMap(
    const std::vector<glm::vec3>& positions,
    float eps,
    std::size_t& uniqueOut);

} // namespace cli
} // namespace editor
} // namespace wowee
