// Tests for Wowee open format round-trips (WOM, WOB, WHM/WOT)
#include <catch_amalgamated.hpp>
#include "pipeline/wowee_building.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/wowee_terrain_loader.hpp"
#include "pipeline/adt_loader.hpp"
#include <filesystem>
#include <fstream>
#include <cstring>

using namespace wowee::pipeline;

static const std::string TEST_DIR = "test_output_formats";

static void ensureTestDir() {
    std::filesystem::create_directories(TEST_DIR);
}

static void cleanupTestDir() {
    std::filesystem::remove_all(TEST_DIR);
}

struct CleanupListener : Catch::EventListenerBase {
    using EventListenerBase::EventListenerBase;
    void testRunEnded(const Catch::TestRunStats&) override { cleanupTestDir(); }
};
CATCH_REGISTER_LISTENER(CleanupListener)

// ============== WOB Tests ==============

TEST_CASE("WOB save and load round-trip", "[wob]") {
    ensureTestDir();
    WoweeBuilding bld;
    bld.name = "TestHouse";
    bld.boundRadius = 20.0f;

    WoweeBuilding::Group grp;
    grp.name = "Room1";
    grp.isOutdoor = false;
    grp.boundMin = glm::vec3(-5, -5, 0);
    grp.boundMax = glm::vec3(5, 5, 4);
    grp.vertices.push_back({{0,0,0}, {0,0,1}, {0,0}, {1,1,1,1}});
    grp.vertices.push_back({{1,0,0}, {0,0,1}, {1,0}, {1,1,1,1}});
    grp.vertices.push_back({{0,1,0}, {0,0,1}, {0,1}, {1,1,1,1}});
    grp.indices = {0, 1, 2};
    grp.texturePaths.push_back("textures/wall.png");
    WoweeBuilding::Material mat;
    mat.texturePath = "textures/wall.png";
    mat.flags = 0x10;
    mat.shader = 3;
    mat.blendMode = 1;
    grp.materials.push_back(mat);
    bld.groups.push_back(grp);

    WoweeBuilding::Portal portal;
    portal.groupA = 0;
    portal.groupB = 1;
    portal.vertices = {{0,0,0}, {1,0,0}, {1,0,3}, {0,0,3}};
    bld.portals.push_back(portal);

    WoweeBuilding::DoodadPlacement dp;
    dp.modelPath = "models/chair.wom";
    dp.position = {2, 3, 0};
    dp.rotation = {0, 45, 0};
    dp.scale = 1.5f;
    bld.doodads.push_back(dp);

    REQUIRE(bld.isValid());

    std::string base = TEST_DIR + "/test_building";
    REQUIRE(WoweeBuildingLoader::save(bld, base));
    REQUIRE(WoweeBuildingLoader::exists(base));

    auto loaded = WoweeBuildingLoader::load(base);
    REQUIRE(loaded.isValid());
    REQUIRE(loaded.name == "TestHouse");
    REQUIRE(loaded.groups.size() == 1);
    REQUIRE(loaded.groups[0].name == "Room1");
    REQUIRE(loaded.groups[0].vertices.size() == 3);
    REQUIRE(loaded.groups[0].indices.size() == 3);
    REQUIRE(loaded.groups[0].isOutdoor == false);
    REQUIRE(loaded.groups[0].materials.size() == 1);
    REQUIRE(loaded.groups[0].materials[0].texturePath == "textures/wall.png");
    REQUIRE(loaded.groups[0].materials[0].flags == 0x10);
    REQUIRE(loaded.groups[0].materials[0].shader == 3);
    REQUIRE(loaded.groups[0].materials[0].blendMode == 1);
    REQUIRE(loaded.portals.size() == 1);
    REQUIRE(loaded.portals[0].groupA == 0);
    REQUIRE(loaded.portals[0].vertices.size() == 4);
    REQUIRE(loaded.doodads.size() == 1);
    REQUIRE(loaded.doodads[0].modelPath == "models/chair.wom");
    REQUIRE(loaded.doodads[0].rotation.y == Catch::Approx(45.0f));
    REQUIRE(loaded.doodads[0].scale == Catch::Approx(1.5f));

    std::filesystem::remove(base + ".wob");
}

TEST_CASE("WOB toWMOModel conversion", "[wob]") {
    WoweeBuilding bld;
    bld.name = "ConvertTest";
    WoweeBuilding::Group grp;
    grp.name = "Group0";
    grp.vertices.push_back({{0,0,0}, {0,0,1}, {0,0}, {1,1,1,1}});
    grp.vertices.push_back({{1,0,0}, {0,0,1}, {1,0}, {1,1,1,1}});
    grp.vertices.push_back({{0,1,0}, {0,0,1}, {0,1}, {1,1,1,1}});
    grp.indices = {0, 1, 2};
    bld.groups.push_back(grp);

    WMOModel wmoOut;
    REQUIRE(WoweeBuildingLoader::toWMOModel(bld, wmoOut));
    REQUIRE(wmoOut.isValid());
    REQUIRE(wmoOut.nGroups == 1);
    REQUIRE(wmoOut.groups[0].vertices.size() == 3);
    REQUIRE(wmoOut.groups[0].indices.size() == 3);
}

// ============== WHM/WOT Tests ==============

TEST_CASE("WHM heightmap save and load round-trip", "[whm]") {
    ensureTestDir();
    ADTTerrain terrain{};
    terrain.loaded = true;
    terrain.version = 18;
    terrain.coord = {32, 48};

    for (int ci = 0; ci < 256; ci++) {
        auto& chunk = terrain.chunks[ci];
        chunk.heightMap.loaded = true;
        chunk.indexX = ci % 16;
        chunk.indexY = ci / 16;
        chunk.position[2] = 100.0f;
        for (int v = 0; v < 145; v++)
            chunk.heightMap.heights[v] = static_cast<float>(v) * 0.1f;
    }

    terrain.textures.push_back("Tileset\\Elwynn\\ElwynnGrass01.blp");
    terrain.chunks[0].layers.push_back({0, 0, 0, 0});

    // Use the editor exporter via manual WHM write
    std::string hmPath = TEST_DIR + "/test_terrain.whm";
    {
        std::ofstream f(hmPath, std::ios::binary);
        uint32_t magic = 0x314D4857, chunks = 256, verts = 145;
        f.write(reinterpret_cast<const char*>(&magic), 4);
        f.write(reinterpret_cast<const char*>(&chunks), 4);
        f.write(reinterpret_cast<const char*>(&verts), 4);
        for (int ci = 0; ci < 256; ci++) {
            float base = 100.0f;
            f.write(reinterpret_cast<const char*>(&base), 4);
            f.write(reinterpret_cast<const char*>(terrain.chunks[ci].heightMap.heights.data()), 145 * 4);
            uint32_t alphaSize = 0;
            f.write(reinterpret_cast<const char*>(&alphaSize), 4);
        }
    }

    ADTTerrain loaded{};
    REQUIRE(WoweeTerrainLoader::loadHeightmap(hmPath, loaded));
    REQUIRE(loaded.isLoaded());

    REQUIRE(loaded.chunks[0].heightMap.heights[0] == Catch::Approx(0.0f));
    REQUIRE(loaded.chunks[0].heightMap.heights[10] == Catch::Approx(1.0f));
    REQUIRE(loaded.chunks[0].heightMap.heights[100] == Catch::Approx(10.0f));

    std::filesystem::remove(hmPath);
}

TEST_CASE("WHM rejects invalid magic", "[whm]") {
    ensureTestDir();
    std::string path = TEST_DIR + "/bad.whm";
    {
        std::ofstream f(path, std::ios::binary);
        uint32_t bad = 0xDEADBEEF;
        f.write(reinterpret_cast<const char*>(&bad), 4);
    }

    ADTTerrain terrain{};
    REQUIRE_FALSE(WoweeTerrainLoader::loadHeightmap(path, terrain));
    std::filesystem::remove(path);
}

TEST_CASE("WOT metadata round-trip with placements", "[wot]") {
    ensureTestDir();
    std::string wotPath = TEST_DIR + "/test_terrain.wot";

    // Write a WOT JSON with textures, layers, water, doodads, and WMOs
    {
        std::ofstream f(wotPath);
        f << R"({
  "format": "wot-1.0",
  "tileX": 32, "tileY": 48,
  "textures": ["Tileset\\Elwynn\\ElwynnGrass01.blp", "Tileset\\Elwynn\\ElwynnDirt01.blp"],
  "chunkLayers": [
    {"layers": [0, 1], "holes": 5}
  ],
  "water": [
    {"chunk": 0, "type": 5, "height": 50.0},
    null
  ],
  "doodadNames": ["World\\Doodad\\Tree01.m2", "World\\Doodad\\Rock03.m2"],
  "doodads": [
    {"nameId": 0, "uniqueId": 100, "pos": [1000.0, 2000.0, 50.0], "rot": [0.0, 45.0, 0.0], "scale": 1024, "flags": 0},
    {"nameId": 1, "uniqueId": 101, "pos": [1100.0, 2100.0, 55.0], "rot": [10.0, 0.0, 5.0], "scale": 512, "flags": 2}
  ],
  "wmoNames": ["World\\WMO\\House01.wmo"],
  "wmos": [
    {"nameId": 0, "uniqueId": 200, "pos": [1200.0, 2200.0, 60.0], "rot": [0.0, 90.0, 0.0], "flags": 1, "doodadSet": 0}
  ]
})";
    }

    ADTTerrain terrain{};
    terrain.loaded = true;
    for (int i = 0; i < 256; i++) {
        terrain.chunks[i].heightMap.loaded = true;
        terrain.chunks[i].position[2] = 100.0f;
    }

    REQUIRE(WoweeTerrainLoader::loadMetadata(wotPath, terrain));

    // Tile coordinates
    REQUIRE(terrain.coord.x == 32);
    REQUIRE(terrain.coord.y == 48);

    // Textures
    REQUIRE(terrain.textures.size() == 2);
    REQUIRE(terrain.textures[0] == "Tileset\\Elwynn\\ElwynnGrass01.blp");

    // Chunk layers
    REQUIRE(terrain.chunks[0].layers.size() == 2);
    REQUIRE(terrain.chunks[0].layers[0].textureId == 0);
    REQUIRE(terrain.chunks[0].layers[1].textureId == 1);
    REQUIRE(terrain.chunks[0].holes == 5);

    // Water
    REQUIRE(terrain.waterData[0].hasWater());
    REQUIRE(terrain.waterData[0].layers[0].liquidType == 5);
    REQUIRE(terrain.waterData[0].layers[0].maxHeight == Catch::Approx(50.0f));

    // Doodad names and placements
    REQUIRE(terrain.doodadNames.size() == 2);
    REQUIRE(terrain.doodadNames[0] == "World\\Doodad\\Tree01.m2");
    REQUIRE(terrain.doodadPlacements.size() == 2);
    REQUIRE(terrain.doodadPlacements[0].nameId == 0);
    REQUIRE(terrain.doodadPlacements[0].uniqueId == 100);
    REQUIRE(terrain.doodadPlacements[0].position[0] == Catch::Approx(1000.0f));
    REQUIRE(terrain.doodadPlacements[0].rotation[1] == Catch::Approx(45.0f));
    REQUIRE(terrain.doodadPlacements[0].scale == 1024);
    REQUIRE(terrain.doodadPlacements[1].nameId == 1);
    REQUIRE(terrain.doodadPlacements[1].flags == 2);

    // WMO names and placements
    REQUIRE(terrain.wmoNames.size() == 1);
    REQUIRE(terrain.wmoNames[0] == "World\\WMO\\House01.wmo");
    REQUIRE(terrain.wmoPlacements.size() == 1);
    REQUIRE(terrain.wmoPlacements[0].nameId == 0);
    REQUIRE(terrain.wmoPlacements[0].uniqueId == 200);
    REQUIRE(terrain.wmoPlacements[0].position[0] == Catch::Approx(1200.0f));
    REQUIRE(terrain.wmoPlacements[0].rotation[1] == Catch::Approx(90.0f));
    REQUIRE(terrain.wmoPlacements[0].flags == 1);
    REQUIRE(terrain.wmoPlacements[0].doodadSet == 0);

    std::filesystem::remove(wotPath);
}

TEST_CASE("WOB rejects missing file", "[wob]") {
    REQUIRE_FALSE(WoweeBuildingLoader::exists("nonexistent_path"));
}
