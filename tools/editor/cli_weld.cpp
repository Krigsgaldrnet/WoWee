#include "cli_weld.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace wowee {
namespace editor {
namespace cli {

std::vector<uint32_t> buildWeldMap(
        const std::vector<glm::vec3>& positions,
        float eps,
        std::size_t& uniqueOut) {
    const float invEps = 1.0f / std::max(eps, 1e-9f);
    using QKey = std::tuple<int64_t, int64_t, int64_t>;
    std::map<QKey, uint32_t> bucket;
    std::vector<uint32_t> canon(positions.size());
    for (std::size_t v = 0; v < positions.size(); ++v) {
        const auto& p = positions[v];
        QKey k{static_cast<int64_t>(std::lround(p.x * invEps)),
               static_cast<int64_t>(std::lround(p.y * invEps)),
               static_cast<int64_t>(std::lround(p.z * invEps))};
        auto it = bucket.find(k);
        if (it == bucket.end()) {
            bucket.emplace(k, static_cast<uint32_t>(v));
            canon[v] = static_cast<uint32_t>(v);
        } else {
            canon[v] = it->second;
        }
    }
    uniqueOut = bucket.size();
    return canon;
}

} // namespace cli
} // namespace editor
} // namespace wowee
