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

TEST_CASE("WOB rejects missing file", "[wob]") {
    REQUIRE_FALSE(WoweeBuildingLoader::exists("nonexistent_path"));
}
