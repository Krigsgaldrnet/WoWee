#include "editor_brush.hpp"
#include <algorithm>
#include <cmath>

namespace wowee {
namespace editor {

float EditorBrush::getInfluence(float distance) const {
    if (distance >= settings_.radius) return 0.0f;

    float t = distance / settings_.radius;
    float innerRadius = 1.0f - settings_.falloff;
    if (t <= innerRadius) return 1.0f;

    float falloffT = (t - innerRadius) / settings_.falloff;
    return 1.0f - (falloffT * falloffT);
}

} // namespace editor
} // namespace wowee
