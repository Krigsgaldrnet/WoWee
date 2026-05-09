#include "cli_gen_mesh.hpp"

#include "pipeline/wowee_model.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace wowee {
namespace editor {
namespace cli {

namespace {

int handleRock(int& i, int argc, char** argv) {
    // Procedural boulder. Starts as an octahedron, subdivides
    // each face N times to get a rounded base, then displaces
    // each vertex along its outward direction by a smooth
    // sin/cos noise term controlled by `seed` and `roughness`.
    // Result is a unique-shaped rock per seed — perfect for
    // scattering across a zone via random-populate-zone.
    //
    // The 16th procedural primitive in the WOM library.
    std::string womBase = argv[++i];
    float radius = 1.0f;
    float roughness = 0.25f;  // 0..1, fraction of radius
    int subdiv = 2;           // 0=8 tris, 1=32, 2=128, 3=512
    uint32_t seed = 1;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { radius = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { roughness = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { subdiv = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { seed = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    if (radius <= 0 || roughness < 0 || roughness > 1 ||
        subdiv < 0 || subdiv > 4) {
        std::fprintf(stderr,
            "gen-mesh-rock: radius>0, roughness 0..1, subdiv 0..4\n");
        return 1;
    }
    if (womBase.size() >= 4 &&
        womBase.substr(womBase.size() - 4) == ".wom") {
        womBase = womBase.substr(0, womBase.size() - 4);
    }
    // Build sphere via octahedron subdivision. Vertices are
    // accumulated in unit-length form first, then displaced.
    std::vector<glm::vec3> sv;  // sphere verts (unit)
    std::vector<glm::uvec3> st; // sphere tris (vertex indices)
    sv = {
        { 1, 0, 0}, {-1, 0, 0},
        { 0, 1, 0}, { 0,-1, 0},
        { 0, 0, 1}, { 0, 0,-1},
    };
    st = {
        {0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
        {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5},
    };
    // Edge-midpoint cache so shared edges don't duplicate verts.
    for (int s = 0; s < subdiv; ++s) {
        std::map<std::pair<uint32_t,uint32_t>, uint32_t> midCache;
        auto midpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
            auto key = std::make_pair(std::min(a,b), std::max(a,b));
            auto it = midCache.find(key);
            if (it != midCache.end()) return it->second;
            glm::vec3 m = glm::normalize((sv[a] + sv[b]) * 0.5f);
            uint32_t idx = static_cast<uint32_t>(sv.size());
            sv.push_back(m);
            midCache[key] = idx;
            return idx;
        };
        std::vector<glm::uvec3> next;
        next.reserve(st.size() * 4);
        for (auto& tri : st) {
            uint32_t a = tri.x, b = tri.y, c = tri.z;
            uint32_t ab = midpoint(a, b);
            uint32_t bc = midpoint(b, c);
            uint32_t ca = midpoint(c, a);
            next.push_back({a,  ab, ca});
            next.push_back({b,  bc, ab});
            next.push_back({c,  ca, bc});
            next.push_back({ab, bc, ca});
        }
        st.swap(next);
    }
    // Smooth pseudo-noise displacement. Three orthogonal sin
    // products give a coherent bumpy surface; phase shift uses
    // the seed so each value yields a distinct silhouette.
    float sf = static_cast<float>(seed);
    auto displace = [&](glm::vec3 p) -> float {
        float n = std::sin(p.x * 3.1f + sf * 0.91f) *
                  std::sin(p.y * 4.7f + sf * 1.37f) *
                  std::sin(p.z * 5.3f + sf * 0.43f);
        float n2 = std::sin(p.x * 7.1f + sf * 0.11f) *
                   std::sin(p.y * 8.3f + sf * 2.13f) *
                   std::sin(p.z * 9.7f + sf * 1.91f);
        return 1.0f + roughness * (0.7f * n + 0.3f * n2);
    };
    wowee::pipeline::WoweeModel wom;
    wom.name = std::filesystem::path(womBase).stem().string();
    wom.version = 3;
    std::vector<glm::vec3> finalPos(sv.size());
    for (size_t v = 0; v < sv.size(); ++v) {
        finalPos[v] = sv[v] * (radius * displace(sv[v]));
    }
    // Per-vertex normals from triangle face normals (averaged).
    std::vector<glm::vec3> normals(sv.size(), glm::vec3(0));
    for (auto& tri : st) {
        glm::vec3 a = finalPos[tri.x];
        glm::vec3 b = finalPos[tri.y];
        glm::vec3 c = finalPos[tri.z];
        glm::vec3 fn = glm::normalize(glm::cross(b - a, c - a));
        normals[tri.x] += fn;
        normals[tri.y] += fn;
        normals[tri.z] += fn;
    }
    for (auto& n : normals) n = glm::length(n) > 1e-6f
        ? glm::normalize(n) : glm::vec3(0, 1, 0);
    for (size_t v = 0; v < sv.size(); ++v) {
        wowee::pipeline::WoweeModel::Vertex vtx;
        vtx.position = finalPos[v];
        vtx.normal = normals[v];
        // Spherical UV unwrap. Visible seam at u=0/1 is
        // acceptable for rocks — usually hidden by terrain.
        glm::vec3 d = glm::normalize(sv[v]);
        vtx.texCoord = {
            0.5f + std::atan2(d.z, d.x) / (2.0f * 3.14159265f),
            0.5f - std::asin(d.y) / 3.14159265f,
        };
        wom.vertices.push_back(vtx);
    }
    for (auto& tri : st) {
        wom.indices.push_back(tri.x);
        wom.indices.push_back(tri.y);
        wom.indices.push_back(tri.z);
    }
    float bound = radius * (1.0f + roughness);
    wom.boundMin = glm::vec3(-bound);
    wom.boundMax = glm::vec3( bound);
    wowee::pipeline::WoweeModel::Batch batch;
    batch.indexStart = 0;
    batch.indexCount = static_cast<uint32_t>(wom.indices.size());
    batch.textureIndex = 0;
    wom.batches.push_back(batch);
    if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
        std::fprintf(stderr,
            "gen-mesh-rock: failed to save %s.wom\n", womBase.c_str());
        return 1;
    }
    std::printf("Wrote %s.wom\n", womBase.c_str());
    std::printf("  radius    : %.3f\n", radius);
    std::printf("  roughness : %.3f\n", roughness);
    std::printf("  subdiv    : %d\n", subdiv);
    std::printf("  seed      : %u\n", seed);
    std::printf("  vertices  : %zu\n", wom.vertices.size());
    std::printf("  triangles : %zu\n", wom.indices.size() / 3);
    return 0;
}

int handlePillar(int& i, int argc, char** argv) {
    // Procedural classical column. Central shaft is a
    // cylinder with N concave flutes (radius modulated by
    // cos²(theta*flutes/2)), capped above and below by
    // wider disc caps that act as a simple capital and
    // base. The 17th procedural mesh primitive — useful
    // for ruins, temples, dungeons, plaza decoration.
    std::string womBase = argv[++i];
    float radius = 0.4f;
    float height = 4.0f;
    int flutes = 12;
    float capScale = 1.25f;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { radius = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { height = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { flutes = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { capScale = std::stof(argv[++i]); } catch (...) {}
    }
    if (radius <= 0 || height <= 0 ||
        flutes < 4 || flutes > 64 ||
        capScale < 1.0f || capScale > 4.0f) {
        std::fprintf(stderr,
            "gen-mesh-pillar: radius>0, height>0, flutes 4..64, capScale 1..4\n");
        return 1;
    }
    if (womBase.size() >= 4 &&
        womBase.substr(womBase.size() - 4) == ".wom") {
        womBase = womBase.substr(0, womBase.size() - 4);
    }
    const float pi = 3.14159265358979f;
    // We use 8 segments per flute so the cosine-modulated
    // groove resolves smoothly. Vertical: 2 rings (top/bot
    // of shaft) + cap/base discs.
    const int radSegs = flutes * 8;
    const float fluteDepth = radius * 0.12f;
    float capR = radius * capScale;
    float capThick = radius * 0.25f;
    wowee::pipeline::WoweeModel wom;
    wom.name = std::filesystem::path(womBase).stem().string();
    wom.version = 3;
    auto addV = [&](glm::vec3 p, glm::vec3 n, glm::vec2 uv) -> uint32_t {
        wowee::pipeline::WoweeModel::Vertex vtx;
        vtx.position = p; vtx.normal = n; vtx.texCoord = uv;
        wom.vertices.push_back(vtx);
        return static_cast<uint32_t>(wom.vertices.size() - 1);
    };
    // Shaft side ring at given y. radius modulated by flute count.
    auto buildShaftRing = [&](float y) -> uint32_t {
        uint32_t start = static_cast<uint32_t>(wom.vertices.size());
        for (int sg = 0; sg <= radSegs; ++sg) {
            float u = static_cast<float>(sg) / radSegs;
            float ang = u * 2.0f * pi;
            float c = std::cos(ang * flutes * 0.5f);
            float r = radius - fluteDepth * (c * c);
            glm::vec3 p(r * std::cos(ang), y, r * std::sin(ang));
            glm::vec3 n(std::cos(ang), 0, std::sin(ang));
            addV(p, glm::normalize(n), glm::vec2(u, y / height));
        }
        return start;
    };
    // Cap/base disc ring (constant radius capR) at given y.
    auto buildCapRing = [&](float y, float r) -> uint32_t {
        uint32_t start = static_cast<uint32_t>(wom.vertices.size());
        for (int sg = 0; sg <= radSegs; ++sg) {
            float u = static_cast<float>(sg) / radSegs;
            float ang = u * 2.0f * pi;
            glm::vec3 p(r * std::cos(ang), y, r * std::sin(ang));
            glm::vec3 n(std::cos(ang), 0, std::sin(ang));
            addV(p, glm::normalize(n), glm::vec2(u, y / height));
        }
        return start;
    };
    // Layout (Y goes up):
    //   capThick: base disc bottom
    //   capThick: base disc top
    //   ...shaft from capThick to height-capThick...
    //   height-capThick: cap disc bottom
    //   height: cap disc top
    float shaftY0 = capThick;
    float shaftY1 = height - capThick;
    uint32_t baseBot = buildCapRing(0.0f, capR);
    uint32_t baseTop = buildCapRing(shaftY0, capR);
    uint32_t shaftBot = buildShaftRing(shaftY0);
    uint32_t shaftTop = buildShaftRing(shaftY1);
    uint32_t capBot = buildCapRing(shaftY1, capR);
    uint32_t capTop = buildCapRing(height, capR);
    // Quad connector helper.
    auto connect = [&](uint32_t a0, uint32_t a1) {
        for (int sg = 0; sg < radSegs; ++sg) {
            uint32_t i00 = a0 + sg;
            uint32_t i01 = a0 + sg + 1;
            uint32_t i10 = a1 + sg;
            uint32_t i11 = a1 + sg + 1;
            wom.indices.insert(wom.indices.end(),
                               { i00, i10, i01, i01, i10, i11 });
        }
    };
    connect(baseBot, baseTop);   // base side
    connect(shaftBot, shaftTop); // shaft
    connect(capBot, capTop);     // cap side
    // Bottom cap (downward fan), top cap (upward fan).
    uint32_t bottomCenter = addV({0, 0, 0}, {0, -1, 0}, {0.5f, 0.5f});
    uint32_t topCenter = addV({0, height, 0}, {0, 1, 0}, {0.5f, 0.5f});
    for (int sg = 0; sg < radSegs; ++sg) {
        wom.indices.insert(wom.indices.end(),
            { bottomCenter, baseBot + sg + 1, baseBot + sg });
        wom.indices.insert(wom.indices.end(),
            { topCenter, capTop + sg, capTop + sg + 1 });
    }
    // Annular surfaces where caps meet shaft (top of base disc
    // out to shaft, etc.). Just connect the two rings — they
    // sit at the same Y so this looks like a flat ring.
    connect(baseTop, shaftBot);
    connect(shaftTop, capBot);
    wowee::pipeline::WoweeModel::Batch batch;
    batch.indexStart = 0;
    batch.indexCount = static_cast<uint32_t>(wom.indices.size());
    batch.textureIndex = 0;
    wom.batches.push_back(batch);
    wom.boundMin = glm::vec3(-capR, 0,      -capR);
    wom.boundMax = glm::vec3( capR, height,  capR);
    if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
        std::fprintf(stderr,
            "gen-mesh-pillar: failed to save %s.wom\n", womBase.c_str());
        return 1;
    }
    std::printf("Wrote %s.wom\n", womBase.c_str());
    std::printf("  radius    : %.3f\n", radius);
    std::printf("  height    : %.3f\n", height);
    std::printf("  flutes    : %d\n", flutes);
    std::printf("  cap scale : %.2fx (capR=%.3f)\n", capScale, capR);
    std::printf("  vertices  : %zu\n", wom.vertices.size());
    std::printf("  triangles : %zu\n", wom.indices.size() / 3);
    return 0;
}

int handleBridge(int& i, int argc, char** argv) {
    // Procedural plank bridge. Deck is N axis-aligned planks
    // running across the bridge's width with small gaps
    // between, plus two side rails (top + bottom rails on
    // posts). Bridge length runs along +X, width is on Z.
    // The 18th procedural mesh primitive — useful for
    // river crossings, dungeon catwalks, scenic overlooks.
    std::string womBase = argv[++i];
    float length = 6.0f;     // along X
    float width = 2.0f;      // along Z
    int planks = 6;          // plank count across the length
    float railHeight = 1.0f; // rail height above deck (0 = no rails)
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { length = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { width = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { planks = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { railHeight = std::stof(argv[++i]); } catch (...) {}
    }
    if (length <= 0 || width <= 0 ||
        planks < 1 || planks > 64 ||
        railHeight < 0 || railHeight > 4.0f) {
        std::fprintf(stderr,
            "gen-mesh-bridge: length>0, width>0, planks 1..64, rail 0..4\n");
        return 1;
    }
    if (womBase.size() >= 4 &&
        womBase.substr(womBase.size() - 4) == ".wom") {
        womBase = womBase.substr(0, womBase.size() - 4);
    }
    wowee::pipeline::WoweeModel wom;
    wom.name = std::filesystem::path(womBase).stem().string();
    wom.version = 3;
    // Box helper — builds 24-vert / 12-tri box centered on
    // (cx, cy, cz) with half-extents (hx, hy, hz). Each face
    // gets unique vertices so flat-shading works. Indices are
    // pushed into wom.indices directly.
    auto addBox = [&](float cx, float cy, float cz,
                      float hx, float hy, float hz) {
        glm::vec3 c(cx, cy, cz);
        struct Face {
            glm::vec3 n;
            glm::vec3 du, dv;  // unit-length axes spanning the face
        };
        Face faces[6] = {
            {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},   // top    (+Y)
            {{0,-1, 0}, {1, 0, 0}, {0, 0,-1}},   // bottom (-Y)
            {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}},   // right  (+X)
            {{-1,0, 0}, {0, 0,-1}, {0, 1, 0}},   // left   (-X)
            {{0, 0, 1}, {-1,0, 0}, {0, 1, 0}},   // front  (+Z)
            {{0, 0,-1}, {1, 0, 0}, {0, 1, 0}},   // back   (-Z)
        };
        glm::vec3 ext(hx, hy, hz);
        for (const Face& f : faces) {
            glm::vec3 center = c + glm::vec3(f.n.x*hx, f.n.y*hy, f.n.z*hz);
            glm::vec3 du = glm::vec3(f.du.x*ext.x, f.du.y*ext.y, f.du.z*ext.z);
            glm::vec3 dv = glm::vec3(f.dv.x*ext.x, f.dv.y*ext.y, f.dv.z*ext.z);
            uint32_t base = static_cast<uint32_t>(wom.vertices.size());
            auto push = [&](glm::vec3 p, float u, float v) {
                wowee::pipeline::WoweeModel::Vertex vtx;
                vtx.position = p;
                vtx.normal = f.n;
                vtx.texCoord = {u, v};
                wom.vertices.push_back(vtx);
            };
            push(center - du - dv, 0, 0);
            push(center + du - dv, 1, 0);
            push(center + du + dv, 1, 1);
            push(center - du + dv, 0, 1);
            wom.indices.insert(wom.indices.end(),
                { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
    };
    // Deck: planks along X, gap = 5% of plank pitch.
    float plankThickness = 0.08f;
    float plankPitch = length / planks;
    float plankWidth = plankPitch * 0.95f;
    for (int p = 0; p < planks; ++p) {
        float cx = -length * 0.5f + plankPitch * (p + 0.5f);
        addBox(cx, plankThickness * 0.5f, 0,
               plankWidth * 0.5f, plankThickness * 0.5f, width * 0.5f);
    }
    // Rails: 2 sides × (top rail + 3 posts) when railHeight > 0
    if (railHeight > 0.0f) {
        float postR = 0.06f;
        float topRailR = 0.08f;
        int postCount = 3;
        float rzOffset = width * 0.5f - postR;
        for (int side = 0; side < 2; ++side) {
            float zSign = (side == 0) ? 1.0f : -1.0f;
            float z = zSign * rzOffset;
            // Top rail: long thin box spanning length
            addBox(0, plankThickness + railHeight, z,
                   length * 0.5f, topRailR, topRailR);
            // Posts evenly spaced
            for (int p = 0; p < postCount; ++p) {
                float t = (postCount > 1)
                    ? static_cast<float>(p) / (postCount - 1)
                    : 0.5f;
                float cx = -length * 0.5f + length * t;
                if (p == 0) cx += postR;
                if (p == postCount - 1) cx -= postR;
                addBox(cx, plankThickness + railHeight * 0.5f, z,
                       postR, railHeight * 0.5f, postR);
            }
        }
    }
    wowee::pipeline::WoweeModel::Batch batch;
    batch.indexStart = 0;
    batch.indexCount = static_cast<uint32_t>(wom.indices.size());
    batch.textureIndex = 0;
    wom.batches.push_back(batch);
    float maxY = plankThickness + railHeight;
    wom.boundMin = glm::vec3(-length * 0.5f, 0,        -width * 0.5f);
    wom.boundMax = glm::vec3( length * 0.5f, maxY,      width * 0.5f);
    if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
        std::fprintf(stderr,
            "gen-mesh-bridge: failed to save %s.wom\n", womBase.c_str());
        return 1;
    }
    std::printf("Wrote %s.wom\n", womBase.c_str());
    std::printf("  length    : %.3f\n", length);
    std::printf("  width     : %.3f\n", width);
    std::printf("  planks    : %d\n", planks);
    std::printf("  rail H    : %.3f%s\n", railHeight,
                railHeight > 0 ? "" : " (no rails)");
    std::printf("  vertices  : %zu\n", wom.vertices.size());
    std::printf("  triangles : %zu\n", wom.indices.size() / 3);
    return 0;
}

int handleTower(int& i, int argc, char** argv) {
    // Procedural castle tower. Solid cylindrical shaft with
    // crenellated battlements ringing the top: alternating
    // raised "merlons" and gaps. Each merlon is a thin
    // angular wedge sitting on the top rim. Useful for
    // keeps, watchtowers, perimeter walls.
    //
    // The 19th procedural mesh primitive.
    std::string womBase = argv[++i];
    float radius = 1.5f;
    float height = 8.0f;
    int battlements = 8;     // merlons around the rim
    float battlementH = 0.5f;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { radius = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { height = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { battlements = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { battlementH = std::stof(argv[++i]); } catch (...) {}
    }
    if (radius <= 0 || height <= 0 ||
        battlements < 4 || battlements > 64 ||
        battlementH < 0 || battlementH > 4.0f) {
        std::fprintf(stderr,
            "gen-mesh-tower: radius>0, height>0, battlements 4..64, bH 0..4\n");
        return 1;
    }
    if (womBase.size() >= 4 &&
        womBase.substr(womBase.size() - 4) == ".wom") {
        womBase = womBase.substr(0, womBase.size() - 4);
    }
    wowee::pipeline::WoweeModel wom;
    wom.name = std::filesystem::path(womBase).stem().string();
    wom.version = 3;
    const float pi = 3.14159265358979f;
    const int radSegs = std::max(24, battlements * 4);
    auto addV = [&](glm::vec3 p, glm::vec3 n, glm::vec2 uv) -> uint32_t {
        wowee::pipeline::WoweeModel::Vertex vtx;
        vtx.position = p; vtx.normal = n; vtx.texCoord = uv;
        wom.vertices.push_back(vtx);
        return static_cast<uint32_t>(wom.vertices.size() - 1);
    };
    // Cylinder shaft: side ring at y=0 and y=height.
    uint32_t botRing = static_cast<uint32_t>(wom.vertices.size());
    for (int sg = 0; sg <= radSegs; ++sg) {
        float u = static_cast<float>(sg) / radSegs;
        float ang = u * 2.0f * pi;
        glm::vec3 p(radius * std::cos(ang), 0, radius * std::sin(ang));
        glm::vec3 n(std::cos(ang), 0, std::sin(ang));
        addV(p, n, glm::vec2(u, 0));
    }
    uint32_t topRing = static_cast<uint32_t>(wom.vertices.size());
    for (int sg = 0; sg <= radSegs; ++sg) {
        float u = static_cast<float>(sg) / radSegs;
        float ang = u * 2.0f * pi;
        glm::vec3 p(radius * std::cos(ang), height, radius * std::sin(ang));
        glm::vec3 n(std::cos(ang), 0, std::sin(ang));
        addV(p, n, glm::vec2(u, 1));
    }
    for (int sg = 0; sg < radSegs; ++sg) {
        wom.indices.insert(wom.indices.end(), {
            botRing + sg, topRing + sg, botRing + sg + 1,
            botRing + sg + 1, topRing + sg, topRing + sg + 1
        });
    }
    // Top cap (fan toward upward-facing center).
    uint32_t topCenter = addV({0, height, 0}, {0, 1, 0}, {0.5f, 0.5f});
    for (int sg = 0; sg < radSegs; ++sg) {
        wom.indices.insert(wom.indices.end(),
            { topCenter, topRing + sg, topRing + sg + 1 });
    }
    // Bottom cap (fan toward downward-facing center).
    uint32_t botCenter = addV({0, 0, 0}, {0, -1, 0}, {0.5f, 0.5f});
    for (int sg = 0; sg < radSegs; ++sg) {
        wom.indices.insert(wom.indices.end(),
            { botCenter, botRing + sg + 1, botRing + sg });
    }
    // Battlements: thin curved blocks around the top rim,
    // half the slots filled (alternating merlon/gap).
    // Each merlon is approximated by an extruded arc segment
    // at the wall radius extending outward slightly.
    if (battlementH > 0.0f) {
        int merlonSpan = radSegs / battlements;
        int merlonHalf = std::max(1, merlonSpan / 2);
        float outerR = radius * 1.05f;
        float innerR = radius * 0.95f;
        for (int b = 0; b < battlements; ++b) {
            int startSeg = b * merlonSpan;
            // Build 8-vert box-like segment between angles
            // covering merlonHalf slots (so half the rim is
            // filled, forming the merlon/gap pattern).
            float ang0 = 2.0f * pi * static_cast<float>(startSeg) / radSegs;
            float ang1 = 2.0f * pi * static_cast<float>(startSeg + merlonHalf) / radSegs;
            glm::vec3 outer0(outerR * std::cos(ang0), 0, outerR * std::sin(ang0));
            glm::vec3 outer1(outerR * std::cos(ang1), 0, outerR * std::sin(ang1));
            glm::vec3 inner0(innerR * std::cos(ang0), 0, innerR * std::sin(ang0));
            glm::vec3 inner1(innerR * std::cos(ang1), 0, innerR * std::sin(ang1));
            glm::vec3 yLow(0, height, 0);
            glm::vec3 yHigh(0, height + battlementH, 0);
            glm::vec3 norm = glm::normalize(
                outer0 + outer1 - inner0 - inner1);
            auto V = [&](glm::vec3 p, glm::vec3 n) {
                return addV(p, n, {0, 0});
            };
            // 8 verts: 4 corners × 2 heights
            uint32_t bbl = V(outer0 + yLow,  norm);   // bot outer left
            uint32_t bbr = V(outer1 + yLow,  norm);
            uint32_t btl = V(outer0 + yHigh, norm);   // top outer left
            uint32_t btr = V(outer1 + yHigh, norm);
            uint32_t ibl = V(inner0 + yLow,  -norm);  // bot inner left
            uint32_t ibr = V(inner1 + yLow,  -norm);
            uint32_t itl = V(inner0 + yHigh, -norm);  // top inner left
            uint32_t itr = V(inner1 + yHigh, -norm);
            // outer face
            wom.indices.insert(wom.indices.end(), {bbl, btl, bbr, bbr, btl, btr});
            // inner face
            wom.indices.insert(wom.indices.end(), {ibr, itr, ibl, ibl, itr, itl});
            // top face
            wom.indices.insert(wom.indices.end(), {btl, itl, btr, btr, itl, itr});
            // left and right end caps
            wom.indices.insert(wom.indices.end(), {bbl, ibl, btl, btl, ibl, itl});
            wom.indices.insert(wom.indices.end(), {bbr, btr, ibr, ibr, btr, itr});
        }
    }
    wowee::pipeline::WoweeModel::Batch batch;
    batch.indexStart = 0;
    batch.indexCount = static_cast<uint32_t>(wom.indices.size());
    batch.textureIndex = 0;
    wom.batches.push_back(batch);
    float maxY = height + battlementH;
    float maxR = radius * 1.05f;
    wom.boundMin = glm::vec3(-maxR, 0,    -maxR);
    wom.boundMax = glm::vec3( maxR, maxY,  maxR);
    if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
        std::fprintf(stderr,
            "gen-mesh-tower: failed to save %s.wom\n", womBase.c_str());
        return 1;
    }
    std::printf("Wrote %s.wom\n", womBase.c_str());
    std::printf("  radius      : %.3f\n", radius);
    std::printf("  height      : %.3f\n", height);
    std::printf("  battlements : %d (%.3fm tall)\n",
                battlements, battlementH);
    std::printf("  vertices    : %zu\n", wom.vertices.size());
    std::printf("  triangles   : %zu\n", wom.indices.size() / 3);
    return 0;
}

int handleHouse(int& i, int argc, char** argv) {
    // Simple procedural house: cube body + pyramid roof
    // meeting at a central apex above the body's roofline.
    // The pyramid sits flush on the body so the eaves
    // line up with the wall edges. No door cutout — that
    // can be added later via mesh boolean ops or texture.
    //
    // The 20th procedural mesh primitive.
    std::string womBase = argv[++i];
    float width = 4.0f;       // along X
    float depth = 4.0f;       // along Z
    float height = 3.0f;      // wall height (Y)
    float roofH = 2.0f;       // pyramid above walls
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { width = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { depth = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { height = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { roofH = std::stof(argv[++i]); } catch (...) {}
    }
    if (width <= 0 || depth <= 0 || height <= 0 ||
        roofH < 0 || roofH > 20.0f) {
        std::fprintf(stderr,
            "gen-mesh-house: width/depth/height>0, roof 0..20\n");
        return 1;
    }
    if (womBase.size() >= 4 &&
        womBase.substr(womBase.size() - 4) == ".wom") {
        womBase = womBase.substr(0, womBase.size() - 4);
    }
    wowee::pipeline::WoweeModel wom;
    wom.name = std::filesystem::path(womBase).stem().string();
    wom.version = 3;
    auto addV = [&](glm::vec3 p, glm::vec3 n, glm::vec2 uv) -> uint32_t {
        wowee::pipeline::WoweeModel::Vertex vtx;
        vtx.position = p; vtx.normal = n; vtx.texCoord = uv;
        wom.vertices.push_back(vtx);
        return static_cast<uint32_t>(wom.vertices.size() - 1);
    };
    float hx = width * 0.5f;
    float hz = depth * 0.5f;
    // 4 walls — each a quad with an outward-facing normal so
    // the house reads as solid even with backface culling on.
    struct Wall {
        glm::vec3 a, b, c, d;  // CCW from outside
        glm::vec3 n;
    };
    Wall walls[4] = {
        {{ hx, 0,  hz}, {-hx, 0,  hz}, {-hx, height,  hz}, { hx, height,  hz}, { 0, 0,  1}}, // +Z
        {{-hx, 0, -hz}, { hx, 0, -hz}, { hx, height, -hz}, {-hx, height, -hz}, { 0, 0, -1}}, // -Z
        {{ hx, 0, -hz}, { hx, 0,  hz}, { hx, height,  hz}, { hx, height, -hz}, { 1, 0,  0}}, // +X
        {{-hx, 0,  hz}, {-hx, 0, -hz}, {-hx, height, -hz}, {-hx, height,  hz}, {-1, 0,  0}}, // -X
    };
    for (const Wall& w : walls) {
        uint32_t a = addV(w.a, w.n, {0, 0});
        uint32_t b = addV(w.b, w.n, {1, 0});
        uint32_t c = addV(w.c, w.n, {1, 1});
        uint32_t d = addV(w.d, w.n, {0, 1});
        wom.indices.insert(wom.indices.end(), {a, b, c, a, c, d});
    }
    // Floor (single quad, normal-down so it shows from below;
    // texturable as a foundation slab).
    {
        uint32_t a = addV({-hx, 0, -hz}, {0, -1, 0}, {0, 0});
        uint32_t b = addV({ hx, 0, -hz}, {0, -1, 0}, {1, 0});
        uint32_t c = addV({ hx, 0,  hz}, {0, -1, 0}, {1, 1});
        uint32_t d = addV({-hx, 0,  hz}, {0, -1, 0}, {0, 1});
        wom.indices.insert(wom.indices.end(), {a, c, b, a, d, c});
    }
    // Roof: 4 triangles meeting at central apex.
    float apexY = height + roofH;
    glm::vec3 apex(0, apexY, 0);
    // Eave corners (Y = wall height) — each triangle shares
    // two adjacent corners + the apex. Per-face normal is
    // computed once so flat shading works.
    glm::vec3 eaves[4] = {
        {-hx, height,  hz},
        { hx, height,  hz},
        { hx, height, -hz},
        {-hx, height, -hz},
    };
    for (int s = 0; s < 4; ++s) {
        glm::vec3 e0 = eaves[s];
        glm::vec3 e1 = eaves[(s + 1) % 4];
        glm::vec3 fn = glm::normalize(glm::cross(e1 - e0, apex - e0));
        uint32_t a = addV(e0, fn, {0, 0});
        uint32_t b = addV(e1, fn, {1, 0});
        uint32_t c = addV(apex, fn, {0.5f, 1});
        wom.indices.insert(wom.indices.end(), {a, b, c});
    }
    wowee::pipeline::WoweeModel::Batch batch;
    batch.indexStart = 0;
    batch.indexCount = static_cast<uint32_t>(wom.indices.size());
    batch.textureIndex = 0;
    wom.batches.push_back(batch);
    wom.boundMin = glm::vec3(-hx, 0, -hz);
    wom.boundMax = glm::vec3( hx, apexY,  hz);
    if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
        std::fprintf(stderr,
            "gen-mesh-house: failed to save %s.wom\n", womBase.c_str());
        return 1;
    }
    std::printf("Wrote %s.wom\n", womBase.c_str());
    std::printf("  width      : %.3f\n", width);
    std::printf("  depth      : %.3f\n", depth);
    std::printf("  wall H     : %.3f\n", height);
    std::printf("  roof H     : %.3f (apex %.3f)\n", roofH, apexY);
    std::printf("  vertices   : %zu\n", wom.vertices.size());
    std::printf("  triangles  : %zu\n", wom.indices.size() / 3);
    return 0;
}

int handleFountain(int& i, int argc, char** argv) {
    // Procedural fountain: low cylindrical basin with a
    // narrower spout column rising from its center. Solid
    // basin (not hollow) for simplicity — readable as a
    // fountain because of the spout silhouette. Useful for
    // town squares, plazas, garden centerpieces.
    //
    // The 21st procedural mesh primitive.
    std::string womBase = argv[++i];
    float basinR = 1.5f;
    float basinH = 0.5f;
    float spoutR = 0.2f;
    float spoutH = 1.5f;
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { basinR = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { basinH = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { spoutR = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { spoutH = std::stof(argv[++i]); } catch (...) {}
    }
    if (basinR <= 0 || basinH <= 0 || spoutR <= 0 || spoutH <= 0 ||
        spoutR >= basinR) {
        std::fprintf(stderr,
            "gen-mesh-fountain: all dims > 0; spoutR must be < basinR\n");
        return 1;
    }
    if (womBase.size() >= 4 &&
        womBase.substr(womBase.size() - 4) == ".wom") {
        womBase = womBase.substr(0, womBase.size() - 4);
    }
    wowee::pipeline::WoweeModel wom;
    wom.name = std::filesystem::path(womBase).stem().string();
    wom.version = 3;
    const float pi = 3.14159265358979f;
    const int segs = 24;
    auto addV = [&](glm::vec3 p, glm::vec3 n, glm::vec2 uv) -> uint32_t {
        wowee::pipeline::WoweeModel::Vertex vtx;
        vtx.position = p; vtx.normal = n; vtx.texCoord = uv;
        wom.vertices.push_back(vtx);
        return static_cast<uint32_t>(wom.vertices.size() - 1);
    };
    // Cylinder helper: build side ring + caps from y0 to y1
    // at given radius. Returns when done; indices appended
    // directly. Side ring is 2× (segs+1) verts at y0 then y1.
    auto cylinder = [&](float r, float y0, float y1) {
        uint32_t bot = static_cast<uint32_t>(wom.vertices.size());
        for (int sg = 0; sg <= segs; ++sg) {
            float u = static_cast<float>(sg) / segs;
            float ang = u * 2.0f * pi;
            glm::vec3 p(r * std::cos(ang), y0, r * std::sin(ang));
            glm::vec3 n(std::cos(ang), 0, std::sin(ang));
            addV(p, n, glm::vec2(u, 0));
        }
        uint32_t top = static_cast<uint32_t>(wom.vertices.size());
        for (int sg = 0; sg <= segs; ++sg) {
            float u = static_cast<float>(sg) / segs;
            float ang = u * 2.0f * pi;
            glm::vec3 p(r * std::cos(ang), y1, r * std::sin(ang));
            glm::vec3 n(std::cos(ang), 0, std::sin(ang));
            addV(p, n, glm::vec2(u, 1));
        }
        for (int sg = 0; sg < segs; ++sg) {
            wom.indices.insert(wom.indices.end(), {
                bot + sg, top + sg, bot + sg + 1,
                bot + sg + 1, top + sg, top + sg + 1
            });
        }
        // Top cap (faces +Y)
        uint32_t topC = addV({0, y1, 0}, {0, 1, 0}, {0.5f, 0.5f});
        for (int sg = 0; sg < segs; ++sg) {
            wom.indices.insert(wom.indices.end(),
                {topC, top + sg, top + sg + 1});
        }
        // Bottom cap (faces -Y)
        uint32_t botC = addV({0, y0, 0}, {0, -1, 0}, {0.5f, 0.5f});
        for (int sg = 0; sg < segs; ++sg) {
            wom.indices.insert(wom.indices.end(),
                {botC, bot + sg + 1, bot + sg});
        }
    };
    // Basin: cylinder from y=0 to y=basinH at basinR.
    cylinder(basinR, 0.0f, basinH);
    // Spout: cylinder from y=basinH to y=basinH+spoutH at spoutR.
    cylinder(spoutR, basinH, basinH + spoutH);
    wowee::pipeline::WoweeModel::Batch batch;
    batch.indexStart = 0;
    batch.indexCount = static_cast<uint32_t>(wom.indices.size());
    batch.textureIndex = 0;
    wom.batches.push_back(batch);
    float maxY = basinH + spoutH;
    wom.boundMin = glm::vec3(-basinR, 0,    -basinR);
    wom.boundMax = glm::vec3( basinR, maxY,  basinR);
    if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
        std::fprintf(stderr,
            "gen-mesh-fountain: failed to save %s.wom\n", womBase.c_str());
        return 1;
    }
    std::printf("Wrote %s.wom\n", womBase.c_str());
    std::printf("  basin    : R=%.3f H=%.3f\n", basinR, basinH);
    std::printf("  spout    : R=%.3f H=%.3f\n", spoutR, spoutH);
    std::printf("  total H  : %.3f\n", maxY);
    std::printf("  vertices : %zu\n", wom.vertices.size());
    std::printf("  triangles: %zu\n", wom.indices.size() / 3);
    return 0;
}

int handleStatue(int& i, int argc, char** argv) {
    // Humanoid placeholder: square pedestal block + tall
    // narrow body cylinder + head sphere. The silhouette
    // reads as a statue without needing limbs. Useful for
    // monuments, hero statues, plaza centerpieces, religious
    // shrines.
    //
    // The 22nd procedural mesh primitive.
    std::string womBase = argv[++i];
    float pedSize = 1.0f;     // pedestal width and depth
    float bodyH = 2.5f;       // body cylinder height
    float headR = 0.4f;       // head sphere radius
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { pedSize = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { bodyH = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { headR = std::stof(argv[++i]); } catch (...) {}
    }
    if (pedSize <= 0 || bodyH <= 0 || headR <= 0) {
        std::fprintf(stderr,
            "gen-mesh-statue: all dims must be positive\n");
        return 1;
    }
    if (womBase.size() >= 4 &&
        womBase.substr(womBase.size() - 4) == ".wom") {
        womBase = womBase.substr(0, womBase.size() - 4);
    }
    wowee::pipeline::WoweeModel wom;
    wom.name = std::filesystem::path(womBase).stem().string();
    wom.version = 3;
    const float pi = 3.14159265358979f;
    auto addV = [&](glm::vec3 p, glm::vec3 n, glm::vec2 uv) -> uint32_t {
        wowee::pipeline::WoweeModel::Vertex vtx;
        vtx.position = p; vtx.normal = n; vtx.texCoord = uv;
        wom.vertices.push_back(vtx);
        return static_cast<uint32_t>(wom.vertices.size() - 1);
    };
    // Pedestal: low square block (24 unique verts).
    float pedH = pedSize * 0.4f;
    float hp = pedSize * 0.5f;
    {
        struct Face { glm::vec3 n, du, dv; };
        Face faces[6] = {
            {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},
            {{0,-1, 0}, {1, 0, 0}, {0, 0,-1}},
            {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
            {{-1,0, 0}, {0, 0,-1}, {0, 1, 0}},
            {{0, 0, 1}, {-1,0, 0}, {0, 1, 0}},
            {{0, 0,-1}, {1, 0, 0}, {0, 1, 0}},
        };
        glm::vec3 c(0, pedH * 0.5f, 0);
        glm::vec3 ext(hp, pedH * 0.5f, hp);
        for (const Face& f : faces) {
            glm::vec3 center = c + glm::vec3(f.n.x*ext.x, f.n.y*ext.y, f.n.z*ext.z);
            glm::vec3 du = glm::vec3(f.du.x*ext.x, f.du.y*ext.y, f.du.z*ext.z);
            glm::vec3 dv = glm::vec3(f.dv.x*ext.x, f.dv.y*ext.y, f.dv.z*ext.z);
            uint32_t base = static_cast<uint32_t>(wom.vertices.size());
            addV(center - du - dv, f.n, {0, 0});
            addV(center + du - dv, f.n, {1, 0});
            addV(center + du + dv, f.n, {1, 1});
            addV(center - du + dv, f.n, {0, 1});
            wom.indices.insert(wom.indices.end(),
                {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }
    // Body cylinder from y=pedH to y=pedH+bodyH at radius pedSize*0.2
    float bodyR = pedSize * 0.2f;
    float bodyY0 = pedH;
    float bodyY1 = pedH + bodyH;
    const int segs = 16;
    uint32_t bodyBot = static_cast<uint32_t>(wom.vertices.size());
    for (int sg = 0; sg <= segs; ++sg) {
        float u = static_cast<float>(sg) / segs;
        float ang = u * 2.0f * pi;
        glm::vec3 p(bodyR * std::cos(ang), bodyY0, bodyR * std::sin(ang));
        glm::vec3 n(std::cos(ang), 0, std::sin(ang));
        addV(p, n, {u, 0});
    }
    uint32_t bodyTop = static_cast<uint32_t>(wom.vertices.size());
    for (int sg = 0; sg <= segs; ++sg) {
        float u = static_cast<float>(sg) / segs;
        float ang = u * 2.0f * pi;
        glm::vec3 p(bodyR * std::cos(ang), bodyY1, bodyR * std::sin(ang));
        glm::vec3 n(std::cos(ang), 0, std::sin(ang));
        addV(p, n, {u, 1});
    }
    for (int sg = 0; sg < segs; ++sg) {
        wom.indices.insert(wom.indices.end(), {
            bodyBot + sg, bodyTop + sg, bodyBot + sg + 1,
            bodyBot + sg + 1, bodyTop + sg, bodyTop + sg + 1
        });
    }
    // Head sphere centered above body. UV-sphere with 16
    // longitude × 12 latitude segments.
    float headY = bodyY1 + headR;
    const int headLon = 16;
    const int headLat = 12;
    uint32_t headStart = static_cast<uint32_t>(wom.vertices.size());
    for (int la = 0; la <= headLat; ++la) {
        float v = static_cast<float>(la) / headLat;
        float phi = v * pi;  // 0..pi
        float sphi = std::sin(phi), cphi = std::cos(phi);
        for (int lo = 0; lo <= headLon; ++lo) {
            float u = static_cast<float>(lo) / headLon;
            float theta = u * 2.0f * pi;
            glm::vec3 dir(sphi * std::cos(theta),
                          cphi,
                          sphi * std::sin(theta));
            glm::vec3 p = glm::vec3(0, headY, 0) + dir * headR;
            addV(p, dir, {u, v});
        }
    }
    int rowSize = headLon + 1;
    for (int la = 0; la < headLat; ++la) {
        for (int lo = 0; lo < headLon; ++lo) {
            uint32_t i00 = headStart + la * rowSize + lo;
            uint32_t i01 = headStart + la * rowSize + lo + 1;
            uint32_t i10 = headStart + (la + 1) * rowSize + lo;
            uint32_t i11 = headStart + (la + 1) * rowSize + lo + 1;
            wom.indices.insert(wom.indices.end(),
                {i00, i10, i01, i01, i10, i11});
        }
    }
    wowee::pipeline::WoweeModel::Batch batch;
    batch.indexStart = 0;
    batch.indexCount = static_cast<uint32_t>(wom.indices.size());
    batch.textureIndex = 0;
    wom.batches.push_back(batch);
    float maxY = headY + headR;
    wom.boundMin = glm::vec3(-hp, 0,    -hp);
    wom.boundMax = glm::vec3( hp, maxY,  hp);
    if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
        std::fprintf(stderr,
            "gen-mesh-statue: failed to save %s.wom\n", womBase.c_str());
        return 1;
    }
    std::printf("Wrote %s.wom\n", womBase.c_str());
    std::printf("  pedestal  : %.3f × %.3f × %.3f\n", pedSize, pedH, pedSize);
    std::printf("  body      : R=%.3f H=%.3f\n", bodyR, bodyH);
    std::printf("  head      : R=%.3f\n", headR);
    std::printf("  total H   : %.3f\n", maxY);
    std::printf("  vertices  : %zu\n", wom.vertices.size());
    std::printf("  triangles : %zu\n", wom.indices.size() / 3);
    return 0;
}

int handleAltar(int& i, int argc, char** argv) {
    // Round altar: stack of N stepped cylindrical discs,
    // each one wider and shorter than the next so the
    // silhouette descends like a wedding cake. Top disc is
    // the altar surface (where offerings would go); base
    // discs widen out to anchor the structure visually.
    //
    // The 23rd procedural mesh primitive — pairs naturally
    // with --gen-texture-marble for a temple aesthetic.
    std::string womBase = argv[++i];
    float topR = 0.7f;        // top altar disc radius
    float topH = 0.3f;        // top altar disc height
    int steps = 3;            // base steps below the top
    float stepStride = 0.3f;  // each step grows R by this much, shrinks H
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { topR = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { topH = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { steps = std::stoi(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { stepStride = std::stof(argv[++i]); } catch (...) {}
    }
    if (topR <= 0 || topH <= 0 || steps < 0 || steps > 16 ||
        stepStride <= 0 || stepStride > 5.0f) {
        std::fprintf(stderr,
            "gen-mesh-altar: topR/topH > 0, steps 0..16, stride 0..5\n");
        return 1;
    }
    if (womBase.size() >= 4 &&
        womBase.substr(womBase.size() - 4) == ".wom") {
        womBase = womBase.substr(0, womBase.size() - 4);
    }
    wowee::pipeline::WoweeModel wom;
    wom.name = std::filesystem::path(womBase).stem().string();
    wom.version = 3;
    const float pi = 3.14159265358979f;
    const int segs = 24;
    auto addV = [&](glm::vec3 p, glm::vec3 n, glm::vec2 uv) -> uint32_t {
        wowee::pipeline::WoweeModel::Vertex vtx;
        vtx.position = p; vtx.normal = n; vtx.texCoord = uv;
        wom.vertices.push_back(vtx);
        return static_cast<uint32_t>(wom.vertices.size() - 1);
    };
    // Build a cylindrical disc from y0 to y1 at radius r.
    // Side ring + top cap (faces +Y). Bottom of each disc
    // is hidden by the next disc below, so we skip a bottom
    // cap on all discs except the last (saves ~24 tris/disc).
    auto disc = [&](float r, float y0, float y1, bool capBottom) {
        uint32_t bot = static_cast<uint32_t>(wom.vertices.size());
        for (int sg = 0; sg <= segs; ++sg) {
            float u = static_cast<float>(sg) / segs;
            float ang = u * 2.0f * pi;
            glm::vec3 p(r * std::cos(ang), y0, r * std::sin(ang));
            glm::vec3 n(std::cos(ang), 0, std::sin(ang));
            addV(p, n, {u, 0});
        }
        uint32_t top = static_cast<uint32_t>(wom.vertices.size());
        for (int sg = 0; sg <= segs; ++sg) {
            float u = static_cast<float>(sg) / segs;
            float ang = u * 2.0f * pi;
            glm::vec3 p(r * std::cos(ang), y1, r * std::sin(ang));
            glm::vec3 n(std::cos(ang), 0, std::sin(ang));
            addV(p, n, {u, 1});
        }
        for (int sg = 0; sg < segs; ++sg) {
            wom.indices.insert(wom.indices.end(), {
                bot + sg, top + sg, bot + sg + 1,
                bot + sg + 1, top + sg, top + sg + 1
            });
        }
        // Top cap fan (faces +Y).
        uint32_t tc = addV({0, y1, 0}, {0, 1, 0}, {0.5f, 0.5f});
        for (int sg = 0; sg < segs; ++sg) {
            wom.indices.insert(wom.indices.end(),
                {tc, top + sg, top + sg + 1});
        }
        if (capBottom) {
            uint32_t bc = addV({0, y0, 0}, {0, -1, 0}, {0.5f, 0.5f});
            for (int sg = 0; sg < segs; ++sg) {
                wom.indices.insert(wom.indices.end(),
                    {bc, bot + sg + 1, bot + sg});
            }
        }
    };
    // Build bottom-up so y0 starts at floor and tops stack.
    // Step k (k=0 is bottom-most) has radius = topR + (steps-k)*stride
    // and height = topH * (1 - 0.2 * k). Y position accumulates.
    float curY = 0.0f;
    for (int k = steps - 1; k >= 0; --k) {  // bottom step first
        float r = topR + (k + 1) * stepStride;
        float h = topH * (1.0f - 0.2f * k);
        if (h < topH * 0.4f) h = topH * 0.4f;
        bool isBottom = (k == steps - 1);
        disc(r, curY, curY + h, isBottom);
        curY += h;
    }
    // Top disc (the actual altar surface)
    disc(topR, curY, curY + topH, steps == 0);
    float maxY = curY + topH;
    wowee::pipeline::WoweeModel::Batch batch;
    batch.indexStart = 0;
    batch.indexCount = static_cast<uint32_t>(wom.indices.size());
    batch.textureIndex = 0;
    wom.batches.push_back(batch);
    float maxR = topR + steps * stepStride;
    wom.boundMin = glm::vec3(-maxR, 0,    -maxR);
    wom.boundMax = glm::vec3( maxR, maxY,  maxR);
    if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
        std::fprintf(stderr,
            "gen-mesh-altar: failed to save %s.wom\n", womBase.c_str());
        return 1;
    }
    std::printf("Wrote %s.wom\n", womBase.c_str());
    std::printf("  top      : R=%.3f H=%.3f\n", topR, topH);
    std::printf("  steps    : %d (stride %.3f)\n", steps, stepStride);
    std::printf("  base R   : %.3f\n", maxR);
    std::printf("  total H  : %.3f\n", maxY);
    std::printf("  vertices : %zu\n", wom.vertices.size());
    std::printf("  triangles: %zu\n", wom.indices.size() / 3);
    return 0;
}

int handlePortal(int& i, int argc, char** argv) {
    // Doorway portal: two vertical post boxes plus a
    // horizontal lintel box across the top. Posts run along
    // the Z axis (so width spans Z), opening faces +X. The
    // gap between the posts is the actual doorway. Useful
    // for entrances, gates, magical portals, ruins.
    //
    // The 24th procedural mesh primitive.
    std::string womBase = argv[++i];
    float width = 2.5f;          // outer-to-outer along Z
    float height = 4.0f;         // total Y
    float postThick = 0.4f;      // post width in X and Z
    float lintelH = 0.5f;        // top lintel height (Y)
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { width = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { height = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { postThick = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { lintelH = std::stof(argv[++i]); } catch (...) {}
    }
    if (width <= 0 || height <= 0 || postThick <= 0 ||
        lintelH < 0 || postThick * 2 >= width ||
        lintelH > height) {
        std::fprintf(stderr,
            "gen-mesh-portal: posts must fit inside width; lintel <= height\n");
        return 1;
    }
    if (womBase.size() >= 4 &&
        womBase.substr(womBase.size() - 4) == ".wom") {
        womBase = womBase.substr(0, womBase.size() - 4);
    }
    wowee::pipeline::WoweeModel wom;
    wom.name = std::filesystem::path(womBase).stem().string();
    wom.version = 3;
    // Box helper — same pattern as other multi-box meshes.
    auto addBox = [&](float cx, float cy, float cz,
                      float hx, float hy, float hz) {
        struct Face { glm::vec3 n, du, dv; };
        Face faces[6] = {
            {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},
            {{0,-1, 0}, {1, 0, 0}, {0, 0,-1}},
            {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
            {{-1,0, 0}, {0, 0,-1}, {0, 1, 0}},
            {{0, 0, 1}, {-1,0, 0}, {0, 1, 0}},
            {{0, 0,-1}, {1, 0, 0}, {0, 1, 0}},
        };
        glm::vec3 c(cx, cy, cz);
        glm::vec3 ext(hx, hy, hz);
        for (const Face& f : faces) {
            glm::vec3 center = c + glm::vec3(f.n.x*hx, f.n.y*hy, f.n.z*hz);
            glm::vec3 du = glm::vec3(f.du.x*ext.x, f.du.y*ext.y, f.du.z*ext.z);
            glm::vec3 dv = glm::vec3(f.dv.x*ext.x, f.dv.y*ext.y, f.dv.z*ext.z);
            uint32_t base = static_cast<uint32_t>(wom.vertices.size());
            auto push = [&](glm::vec3 p, float u, float v) {
                wowee::pipeline::WoweeModel::Vertex vtx;
                vtx.position = p;
                vtx.normal = f.n;
                vtx.texCoord = {u, v};
                wom.vertices.push_back(vtx);
            };
            push(center - du - dv, 0, 0);
            push(center + du - dv, 1, 0);
            push(center + du + dv, 1, 1);
            push(center - du + dv, 0, 1);
            wom.indices.insert(wom.indices.end(),
                {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    };
    // Two posts at z = ±(width/2 - postThick/2). Each
    // post extends from y=0 to y=height-lintelH so it
    // tucks under the lintel.
    float postY = (height - lintelH) * 0.5f;
    float postHy = (height - lintelH) * 0.5f;
    float postZ = (width - postThick) * 0.5f;
    float postHt = postThick * 0.5f;
    addBox(0, postY,  postZ, postHt, postHy, postHt);
    addBox(0, postY, -postZ, postHt, postHy, postHt);
    // Lintel: spans full width across the top, same X
    // thickness as posts.
    if (lintelH > 0.0f) {
        float lintelY = height - lintelH * 0.5f;
        addBox(0, lintelY, 0,
               postHt, lintelH * 0.5f, width * 0.5f);
    }
    wowee::pipeline::WoweeModel::Batch batch;
    batch.indexStart = 0;
    batch.indexCount = static_cast<uint32_t>(wom.indices.size());
    batch.textureIndex = 0;
    wom.batches.push_back(batch);
    wom.boundMin = glm::vec3(-postHt, 0,      -width * 0.5f);
    wom.boundMax = glm::vec3( postHt, height,  width * 0.5f);
    if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
        std::fprintf(stderr,
            "gen-mesh-portal: failed to save %s.wom\n", womBase.c_str());
        return 1;
    }
    std::printf("Wrote %s.wom\n", womBase.c_str());
    std::printf("  width      : %.3f\n", width);
    std::printf("  height     : %.3f\n", height);
    std::printf("  post thick : %.3f\n", postThick);
    std::printf("  lintel H   : %.3f%s\n", lintelH,
                lintelH > 0 ? "" : " (no lintel)");
    std::printf("  vertices   : %zu\n", wom.vertices.size());
    std::printf("  triangles  : %zu\n", wom.indices.size() / 3);
    return 0;
}

int handleArchway(int& i, int argc, char** argv) {
    // Semicircular arched doorway. Two cylindrical pillars
    // hold up a curved keystone vault: the vault is a series
    // of N angular wedge segments tracing a half-circle from
    // pillar-top to pillar-top. The opening is the empty
    // semicircular space below.
    //
    // The 25th procedural mesh primitive — the "fancier"
    // sibling of --gen-mesh-portal which uses a flat lintel.
    std::string womBase = argv[++i];
    float width = 3.0f;        // outer-to-outer pillar centers along Z
    float pillarH = 3.0f;      // pillar height (Y)
    float thickness = 0.4f;    // pillar radius and arch radial thickness
    int archSegs = 12;         // segments around the half-circle
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { width = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { pillarH = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { thickness = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { archSegs = std::stoi(argv[++i]); } catch (...) {}
    }
    if (width <= 0 || pillarH <= 0 || thickness <= 0 ||
        archSegs < 4 || archSegs > 64 ||
        thickness * 4 >= width) {
        std::fprintf(stderr,
            "gen-mesh-archway: thickness×4 < width, archSegs 4..64\n");
        return 1;
    }
    if (womBase.size() >= 4 &&
        womBase.substr(womBase.size() - 4) == ".wom") {
        womBase = womBase.substr(0, womBase.size() - 4);
    }
    wowee::pipeline::WoweeModel wom;
    wom.name = std::filesystem::path(womBase).stem().string();
    wom.version = 3;
    const float pi = 3.14159265358979f;
    const int pillarSegs = 16;
    auto addV = [&](glm::vec3 p, glm::vec3 n, glm::vec2 uv) -> uint32_t {
        wowee::pipeline::WoweeModel::Vertex vtx;
        vtx.position = p; vtx.normal = n; vtx.texCoord = uv;
        wom.vertices.push_back(vtx);
        return static_cast<uint32_t>(wom.vertices.size() - 1);
    };
    // Cylindrical pillar at given (cx, cz), from y=0 to y=pillarH.
    auto pillar = [&](float cx, float cz) {
        float r = thickness;
        uint32_t bot = static_cast<uint32_t>(wom.vertices.size());
        for (int sg = 0; sg <= pillarSegs; ++sg) {
            float u = static_cast<float>(sg) / pillarSegs;
            float ang = u * 2.0f * pi;
            glm::vec3 p(cx + r * std::cos(ang), 0,
                        cz + r * std::sin(ang));
            glm::vec3 n(std::cos(ang), 0, std::sin(ang));
            addV(p, n, {u, 0});
        }
        uint32_t top = static_cast<uint32_t>(wom.vertices.size());
        for (int sg = 0; sg <= pillarSegs; ++sg) {
            float u = static_cast<float>(sg) / pillarSegs;
            float ang = u * 2.0f * pi;
            glm::vec3 p(cx + r * std::cos(ang), pillarH,
                        cz + r * std::sin(ang));
            glm::vec3 n(std::cos(ang), 0, std::sin(ang));
            addV(p, n, {u, 1});
        }
        for (int sg = 0; sg < pillarSegs; ++sg) {
            wom.indices.insert(wom.indices.end(), {
                bot + sg, top + sg, bot + sg + 1,
                bot + sg + 1, top + sg, top + sg + 1
            });
        }
        // Caps
        uint32_t bc = addV({cx, 0, cz}, {0, -1, 0}, {0.5f, 0.5f});
        uint32_t tc = addV({cx, pillarH, cz}, {0, 1, 0}, {0.5f, 0.5f});
        for (int sg = 0; sg < pillarSegs; ++sg) {
            wom.indices.insert(wom.indices.end(),
                {bc, bot + sg + 1, bot + sg});
            wom.indices.insert(wom.indices.end(),
                {tc, top + sg, top + sg + 1});
        }
    };
    float pillarZ = (width - 2 * thickness) * 0.5f;
    pillar(0,  pillarZ);
    pillar(0, -pillarZ);
    // Arch vault: trace half-circle from (z = +pillarZ, y = pillarH)
    // up over to (z = -pillarZ, y = pillarH). Center of arch:
    // (z = 0, y = pillarH). Arch radius = pillarZ.
    // Inner arch (radius pillarZ - thickness*0.5) and outer
    // (radius pillarZ + thickness*0.5) — the vault sits between.
    float archCY = pillarH;
    float arcInner = pillarZ - thickness * 0.5f;
    float arcOuter = pillarZ + thickness * 0.5f;
    // Each segment: 4 verts (inner-near, outer-near, inner-far,
    // outer-far) extruded along X by thickness so the vault
    // has front and back faces.
    float archX = thickness * 0.5f;  // half-depth in X
    // Build vertex rings for inner and outer surfaces at
    // each segment boundary, then connect.
    // Top half-circle goes from theta=0 to theta=pi.
    std::vector<glm::vec3> innerRing;
    std::vector<glm::vec3> outerRing;
    for (int s = 0; s <= archSegs; ++s) {
        float t = static_cast<float>(s) / archSegs;
        float theta = t * pi;  // 0..pi
        float zi = arcInner * std::cos(theta);
        float yi = arcInner * std::sin(theta);
        float zo = arcOuter * std::cos(theta);
        float yo = arcOuter * std::sin(theta);
        innerRing.push_back({0, archCY + yi, zi});
        outerRing.push_back({0, archCY + yo, zo});
    }
    // For each segment, add 8 vertices (4 corners × front/back face)
    // and stitch them into 6 quads = 12 tris each.
    for (int s = 0; s < archSegs; ++s) {
        glm::vec3 i0 = innerRing[s];
        glm::vec3 i1 = innerRing[s + 1];
        glm::vec3 o0 = outerRing[s];
        glm::vec3 o1 = outerRing[s + 1];
        // Estimate outward (radial) normal as midpoint of o0+o1
        // direction from center.
        glm::vec3 outDir = glm::normalize(glm::vec3(0,
            (i0.y + i1.y + o0.y + o1.y) * 0.25f - archCY,
            (i0.z + i1.z + o0.z + o1.z) * 0.25f));
        glm::vec3 frontN(1, 0, 0);
        glm::vec3 backN(-1, 0, 0);
        auto V = [&](glm::vec3 p, glm::vec3 n) {
            return addV(p, n, {0, 0});
        };
        // Outer surface (top of arch): faces outward radially
        uint32_t a = V({-archX, o0.y, o0.z}, outDir);
        uint32_t b = V({ archX, o0.y, o0.z}, outDir);
        uint32_t c = V({ archX, o1.y, o1.z}, outDir);
        uint32_t d = V({-archX, o1.y, o1.z}, outDir);
        wom.indices.insert(wom.indices.end(), {a, b, c, a, c, d});
        // Inner surface (underside of arch): faces inward
        uint32_t e = V({-archX, i0.y, i0.z}, -outDir);
        uint32_t f = V({ archX, i0.y, i0.z}, -outDir);
        uint32_t g = V({ archX, i1.y, i1.z}, -outDir);
        uint32_t h = V({-archX, i1.y, i1.z}, -outDir);
        wom.indices.insert(wom.indices.end(), {e, g, f, e, h, g});
        // Front face (+X) of this wedge
        uint32_t fi0 = V({ archX, i0.y, i0.z}, frontN);
        uint32_t fo0 = V({ archX, o0.y, o0.z}, frontN);
        uint32_t fo1 = V({ archX, o1.y, o1.z}, frontN);
        uint32_t fi1 = V({ archX, i1.y, i1.z}, frontN);
        wom.indices.insert(wom.indices.end(),
            {fi0, fo0, fo1, fi0, fo1, fi1});
        // Back face (-X)
        uint32_t bi0 = V({-archX, i0.y, i0.z}, backN);
        uint32_t bo0 = V({-archX, o0.y, o0.z}, backN);
        uint32_t bo1 = V({-archX, o1.y, o1.z}, backN);
        uint32_t bi1 = V({-archX, i1.y, i1.z}, backN);
        wom.indices.insert(wom.indices.end(),
            {bi0, bo1, bo0, bi0, bi1, bo1});
    }
    wowee::pipeline::WoweeModel::Batch batch;
    batch.indexStart = 0;
    batch.indexCount = static_cast<uint32_t>(wom.indices.size());
    batch.textureIndex = 0;
    wom.batches.push_back(batch);
    float maxY = pillarH + arcOuter;
    wom.boundMin = glm::vec3(-thickness, 0,    -width * 0.5f);
    wom.boundMax = glm::vec3( thickness, maxY,  width * 0.5f);
    if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
        std::fprintf(stderr,
            "gen-mesh-archway: failed to save %s.wom\n", womBase.c_str());
        return 1;
    }
    std::printf("Wrote %s.wom\n", womBase.c_str());
    std::printf("  width      : %.3f\n", width);
    std::printf("  pillar H   : %.3f\n", pillarH);
    std::printf("  thickness  : %.3f\n", thickness);
    std::printf("  arch segs  : %d (radius %.3f)\n", archSegs, arcOuter);
    std::printf("  apex Y     : %.3f\n", maxY);
    std::printf("  vertices   : %zu\n", wom.vertices.size());
    std::printf("  triangles  : %zu\n", wom.indices.size() / 3);
    return 0;
}

int handleBarrel(int& i, int argc, char** argv) {
    // Tapered barrel: cylindrical body whose radius bulges
    // smoothly from `topRadius` at the rims to `midRadius`
    // at the middle (the classic stave-cooper barrel
    // silhouette), plus 2 raised hoop bands at 25% and 75%
    // of the height. The 26th procedural mesh primitive.
    std::string womBase = argv[++i];
    float topR = 0.4f;        // radius at top and bottom rim
    float midR = 0.5f;        // radius at the middle bulge
    float height = 1.0f;
    float hoopThick = 0.06f;  // hoop band radial protrusion
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { topR = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { midR = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { height = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { hoopThick = std::stof(argv[++i]); } catch (...) {}
    }
    if (topR <= 0 || midR <= 0 || height <= 0 ||
        hoopThick < 0 || hoopThick > 0.5f) {
        std::fprintf(stderr,
            "gen-mesh-barrel: radii/height > 0, hoopThick 0..0.5\n");
        return 1;
    }
    if (womBase.size() >= 4 &&
        womBase.substr(womBase.size() - 4) == ".wom") {
        womBase = womBase.substr(0, womBase.size() - 4);
    }
    wowee::pipeline::WoweeModel wom;
    wom.name = std::filesystem::path(womBase).stem().string();
    wom.version = 3;
    const float pi = 3.14159265358979f;
    const int segs = 16;        // angular subdivisions
    const int rings = 12;       // vertical slices
    auto addV = [&](glm::vec3 p, glm::vec3 n, glm::vec2 uv) -> uint32_t {
        wowee::pipeline::WoweeModel::Vertex vtx;
        vtx.position = p; vtx.normal = n; vtx.texCoord = uv;
        wom.vertices.push_back(vtx);
        return static_cast<uint32_t>(wom.vertices.size() - 1);
    };
    // Radius profile: smooth cosine bulge from rim to mid.
    // r(t) = topR + (midR - topR) * sin(pi*t) where t in 0..1
    // gives 0 at t=0/1 and 1 at t=0.5 — exact rim fit.
    auto radiusAt = [&](float t) -> float {
        return topR + (midR - topR) * std::sin(pi * t);
    };
    uint32_t firstRing = static_cast<uint32_t>(wom.vertices.size());
    for (int ri = 0; ri <= rings; ++ri) {
        float t = static_cast<float>(ri) / rings;
        float y = t * height;
        float r = radiusAt(t);
        // Hoops: bump radius outward in two narrow bands.
        float hoop1 = std::abs(t - 0.25f);
        float hoop2 = std::abs(t - 0.75f);
        if (hoop1 < 0.04f) r += hoopThick * (1.0f - hoop1 / 0.04f);
        if (hoop2 < 0.04f) r += hoopThick * (1.0f - hoop2 / 0.04f);
        for (int sg = 0; sg <= segs; ++sg) {
            float u = static_cast<float>(sg) / segs;
            float ang = u * 2.0f * pi;
            glm::vec3 p(r * std::cos(ang), y, r * std::sin(ang));
            glm::vec3 n(std::cos(ang), 0, std::sin(ang));
            addV(p, n, {u, t});
        }
    }
    int rowSize = segs + 1;
    for (int ri = 0; ri < rings; ++ri) {
        for (int sg = 0; sg < segs; ++sg) {
            uint32_t i00 = firstRing + ri * rowSize + sg;
            uint32_t i01 = firstRing + ri * rowSize + sg + 1;
            uint32_t i10 = firstRing + (ri + 1) * rowSize + sg;
            uint32_t i11 = firstRing + (ri + 1) * rowSize + sg + 1;
            wom.indices.insert(wom.indices.end(),
                {i00, i10, i01, i01, i10, i11});
        }
    }
    // End caps (top + bottom). topR is also the bottom-most
    // and top-most ring radius since sin(0) = sin(pi) = 0.
    uint32_t botCenter = addV({0, 0, 0}, {0, -1, 0}, {0.5f, 0.5f});
    uint32_t topCenter = addV({0, height, 0}, {0, 1, 0}, {0.5f, 0.5f});
    uint32_t botRing = firstRing;
    uint32_t topRing = firstRing + rings * rowSize;
    for (int sg = 0; sg < segs; ++sg) {
        wom.indices.insert(wom.indices.end(),
            {botCenter, botRing + sg + 1, botRing + sg});
        wom.indices.insert(wom.indices.end(),
            {topCenter, topRing + sg, topRing + sg + 1});
    }
    wowee::pipeline::WoweeModel::Batch batch;
    batch.indexStart = 0;
    batch.indexCount = static_cast<uint32_t>(wom.indices.size());
    batch.textureIndex = 0;
    wom.batches.push_back(batch);
    float maxR = midR + hoopThick;
    wom.boundMin = glm::vec3(-maxR, 0,    -maxR);
    wom.boundMax = glm::vec3( maxR, height, maxR);
    if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
        std::fprintf(stderr,
            "gen-mesh-barrel: failed to save %s.wom\n", womBase.c_str());
        return 1;
    }
    std::printf("Wrote %s.wom\n", womBase.c_str());
    std::printf("  rim R     : %.3f\n", topR);
    std::printf("  bulge R   : %.3f\n", midR);
    std::printf("  height    : %.3f\n", height);
    std::printf("  hoops     : 2 (thickness %.3f)\n", hoopThick);
    std::printf("  vertices  : %zu\n", wom.vertices.size());
    std::printf("  triangles : %zu\n", wom.indices.size() / 3);
    return 0;
}

int handleChest(int& i, int argc, char** argv) {
    // Treasure chest: rectangular body box + smaller lid
    // box on top + 3 thin iron bands wrapping around the
    // body + a small lock plate on the front center face.
    // The 27th procedural mesh primitive — useful for
    // dungeon loot, room decoration, quest objectives.
    std::string womBase = argv[++i];
    float width = 1.4f;       // along X
    float depth = 0.9f;       // along Z
    float bodyH = 0.9f;       // body box height
    float lidH = 0.25f;       // lid height above body
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { width = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { depth = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { bodyH = std::stof(argv[++i]); } catch (...) {}
    }
    if (i + 1 < argc && argv[i + 1][0] != '-') {
        try { lidH = std::stof(argv[++i]); } catch (...) {}
    }
    if (width <= 0 || depth <= 0 || bodyH <= 0 || lidH < 0) {
        std::fprintf(stderr,
            "gen-mesh-chest: width/depth/bodyH > 0, lidH >= 0\n");
        return 1;
    }
    if (womBase.size() >= 4 &&
        womBase.substr(womBase.size() - 4) == ".wom") {
        womBase = womBase.substr(0, womBase.size() - 4);
    }
    wowee::pipeline::WoweeModel wom;
    wom.name = std::filesystem::path(womBase).stem().string();
    wom.version = 3;
    // Box helper — adds 24 unique verts / 12 tris centered
    // on (cx, cy, cz) with half-extents (hx, hy, hz). Each
    // face gets unique normals for flat shading.
    auto addBox = [&](float cx, float cy, float cz,
                      float hx, float hy, float hz) {
        struct Face { glm::vec3 n, du, dv; };
        Face faces[6] = {
            {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},
            {{0,-1, 0}, {1, 0, 0}, {0, 0,-1}},
            {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
            {{-1,0, 0}, {0, 0,-1}, {0, 1, 0}},
            {{0, 0, 1}, {-1,0, 0}, {0, 1, 0}},
            {{0, 0,-1}, {1, 0, 0}, {0, 1, 0}},
        };
        glm::vec3 c(cx, cy, cz);
        glm::vec3 ext(hx, hy, hz);
        for (const Face& f : faces) {
            glm::vec3 center = c + glm::vec3(f.n.x*hx, f.n.y*hy, f.n.z*hz);
            glm::vec3 du = glm::vec3(f.du.x*ext.x, f.du.y*ext.y, f.du.z*ext.z);
            glm::vec3 dv = glm::vec3(f.dv.x*ext.x, f.dv.y*ext.y, f.dv.z*ext.z);
            uint32_t base = static_cast<uint32_t>(wom.vertices.size());
            auto push = [&](glm::vec3 p, float u, float v) {
                wowee::pipeline::WoweeModel::Vertex vtx;
                vtx.position = p; vtx.normal = f.n; vtx.texCoord = {u, v};
                wom.vertices.push_back(vtx);
            };
            push(center - du - dv, 0, 0);
            push(center + du - dv, 1, 0);
            push(center + du + dv, 1, 1);
            push(center - du + dv, 0, 1);
            wom.indices.insert(wom.indices.end(),
                {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    };
    float hx = width * 0.5f;
    float hz = depth * 0.5f;
    // Body: y=0 to y=bodyH
    addBox(0, bodyH * 0.5f, 0, hx, bodyH * 0.5f, hz);
    // Lid: smaller box on top, slightly inset on each side
    float lidInset = std::min(width, depth) * 0.04f;
    float lidHx = hx - lidInset;
    float lidHz = hz - lidInset;
    if (lidH > 0.0f && lidHx > 0 && lidHz > 0) {
        addBox(0, bodyH + lidH * 0.5f, 0,
               lidHx, lidH * 0.5f, lidHz);
    }
    // 3 iron bands wrapping the body — thin slabs
    // protruding ~3% radially on the sides + top.
    // Band positions: 15%, 50%, 85% of body width.
    float bandThickX = width * 0.04f;  // band depth along X
    float bandHy = bodyH * 0.5f + 0.005f;
    float bandHz = hz + 0.012f;
    float bandPositions[3] = {-hx * 0.7f, 0.0f, hx * 0.7f};
    for (float bx : bandPositions) {
        addBox(bx, bandHy, 0,
               bandThickX * 0.5f, bandHy, bandHz);
    }
    // Lock plate: small thin box on the front face, centered.
    // Front face is +Z. Plate sits at z = hz + tiny epsilon.
    float lockW = width * 0.10f;
    float lockH = bodyH * 0.18f;
    float lockY = bodyH * 0.65f;
    float lockEps = 0.008f;
    addBox(0, lockY, hz + lockEps,
           lockW * 0.5f, lockH * 0.5f, lockEps);
    wowee::pipeline::WoweeModel::Batch batch;
    batch.indexStart = 0;
    batch.indexCount = static_cast<uint32_t>(wom.indices.size());
    batch.textureIndex = 0;
    wom.batches.push_back(batch);
    float maxY = bodyH + lidH;
    wom.boundMin = glm::vec3(-hx, 0, -hz - 0.012f);
    wom.boundMax = glm::vec3( hx, maxY, hz + 0.012f);
    if (!wowee::pipeline::WoweeModelLoader::save(wom, womBase)) {
        std::fprintf(stderr,
            "gen-mesh-chest: failed to save %s.wom\n", womBase.c_str());
        return 1;
    }
    std::printf("Wrote %s.wom\n", womBase.c_str());
    std::printf("  width × depth : %.3f × %.3f\n", width, depth);
    std::printf("  body H        : %.3f\n", bodyH);
    std::printf("  lid H         : %.3f\n", lidH);
    std::printf("  components    : body + lid + 3 bands + lock\n");
    std::printf("  vertices      : %zu\n", wom.vertices.size());
    std::printf("  triangles     : %zu\n", wom.indices.size() / 3);
    return 0;
}


}  // namespace

bool handleGenMesh(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--gen-mesh-rock") == 0 && i + 1 < argc) {
        outRc = handleRock(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mesh-pillar") == 0 && i + 1 < argc) {
        outRc = handlePillar(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mesh-bridge") == 0 && i + 1 < argc) {
        outRc = handleBridge(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mesh-tower") == 0 && i + 1 < argc) {
        outRc = handleTower(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mesh-house") == 0 && i + 1 < argc) {
        outRc = handleHouse(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mesh-fountain") == 0 && i + 1 < argc) {
        outRc = handleFountain(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mesh-statue") == 0 && i + 1 < argc) {
        outRc = handleStatue(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mesh-altar") == 0 && i + 1 < argc) {
        outRc = handleAltar(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mesh-portal") == 0 && i + 1 < argc) {
        outRc = handlePortal(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mesh-archway") == 0 && i + 1 < argc) {
        outRc = handleArchway(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mesh-barrel") == 0 && i + 1 < argc) {
        outRc = handleBarrel(i, argc, argv); return true;
    }
    if (std::strcmp(argv[i], "--gen-mesh-chest") == 0 && i + 1 < argc) {
        outRc = handleChest(i, argc, argv); return true;
    }
    return false;
}

}  // namespace cli
}  // namespace editor
}  // namespace wowee
