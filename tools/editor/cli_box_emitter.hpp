#pragma once

#include "pipeline/wowee_model.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>

namespace wowee {
namespace editor {
namespace cli {

// Strip a file-extension suffix from a base path if present. Used
// pervasively by --gen-mesh-* / --bake-* / --info-* handlers that
// accept either `path/foo` or `path/foo.ext` as input — the loader
// expects the bare base, so the trailing ".wom" / ".wob" / ".woc"
// must be removed if the user typed it.
//
// Pattern was open-coded as a 4-line if-block in 64+ sites
// across cli_gen_mesh.cpp; hoisted here for one-line callers.
inline void stripExt(std::string& base, const char* ext) {
    std::size_t extLen = 0;
    while (ext[extLen]) ++extLen;
    if (base.size() >= extLen &&
        base.compare(base.size() - extLen, extLen, ext) == 0) {
        base.resize(base.size() - extLen);
    }
}

// Append a single batch covering ALL of wom.indices to wom.batches.
// Called at the end of every gen-mesh primitive (the procedural
// builders emit just one batch per primitive). The same 4-line
// "construct + populate + push" boilerplate was repeated in 53
// handlers before extraction.
inline void finalizeAsSingleBatch(wowee::pipeline::WoweeModel& wom) {
    wowee::pipeline::WoweeModel::Batch batch;
    batch.indexStart = 0;
    batch.indexCount = static_cast<uint32_t>(wom.indices.size());
    batch.textureIndex = 0;
    wom.batches.push_back(batch);
}

// Append one vertex (position, normal, UV) to a WoweeModel and
// return its newly-assigned index. Inline because the procedural
// mesh primitives call this thousands of times per build and the
// abstraction shouldn't cost a function-call frame each time.
// Pre-extraction this was the same 5-line lambda copy-pasted into
// 21 different handlers.
inline uint32_t addVertex(wowee::pipeline::WoweeModel& wom,
                          glm::vec3 p, glm::vec3 n, glm::vec2 uv) {
    wowee::pipeline::WoweeModel::Vertex vtx;
    vtx.position = p;
    vtx.normal = n;
    vtx.texCoord = uv;
    wom.vertices.push_back(vtx);
    return static_cast<uint32_t>(wom.vertices.size() - 1);
}

// Per-float overload used by handlers that compute pos/normal/uv
// components inline rather than building intermediate glm vectors
// (--gen-mesh-stairs, --gen-mesh-tube, --gen-mesh-capsule,
// --gen-mesh-arch). Same semantics as the vec3/vec2 form.
inline uint32_t addVertex(wowee::pipeline::WoweeModel& wom,
                          float px, float py, float pz,
                          float nx, float ny, float nz,
                          float u,  float v) {
    return addVertex(wom, glm::vec3(px, py, pz), glm::vec3(nx, ny, nz),
                      glm::vec2(u, v));
}

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
