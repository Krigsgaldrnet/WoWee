#pragma once

#include "pipeline/wowee_model.hpp"
#include <glm/glm.hpp>

namespace wowee {
namespace editor {
namespace cli {

// Append a flat-shaded axis-aligned box to a WoweeModel. The box
// is centered at (cx, cy, cz) with half-extents (hx, hy, hz). Each
// of the 6 faces emits its own 4 vertices with the face's outward
// normal, so adjacent faces don't share normals — exactly what
// flat shading needs. UVs are 0..1 across each face.
//
// Used pervasively by --gen-mesh-* primitives that build meshes
// from axis-aligned box primitives (firepit stones, dock pilings,
// canopy posts, woodpile logs, tent walls before the door cutout
// added one-off triangles, etc.). Hoisted out of cli_gen_mesh.cpp
// where 36 identical lambdas duplicated this implementation.
void addFlatBox(wowee::pipeline::WoweeModel& wom,
                float cx, float cy, float cz,
                float hx, float hy, float hz);

// Overload taking lower/upper corner positions (lo, hi). Some
// callers (--gen-mesh-archway, --gen-mesh-fence) compute corners
// directly rather than center+halfsize.
void addFlatBox(wowee::pipeline::WoweeModel& wom,
                glm::vec3 lo, glm::vec3 hi);

} // namespace cli
} // namespace editor
} // namespace wowee
