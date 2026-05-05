#pragma once

#include <string>
#include <vector>
#include <array>

namespace wowee {
namespace editor {

enum class Biome {
    Grassland,
    Forest,
    Jungle,
    Desert,
    Barrens,
    Snow,
    Swamp,
    Rocky,
    Beach,
    Volcanic,
    COUNT
};

struct BiomeTextures {
    const char* name;
    const char* base;       // Primary ground texture
    const char* secondary;  // Secondary layer (dirt/path)
    const char* accent;     // Accent (rocks/roots)
    const char* detail;     // Detail overlay
};

inline const BiomeTextures& getBiomeTextures(Biome biome) {
    static const std::array<BiomeTextures, static_cast<size_t>(Biome::COUNT)> biomes = {{
        { // Grassland
            "Grassland",
            "Tileset\\Elwynn\\ElwynnGrassBase.blp",
            "Tileset\\Elwynn\\ElwynnDirtBase.blp",
            "Tileset\\Elwynn\\ElwynnCobblestoneBase.blp",
            "Tileset\\Elwynn\\ElwynnGrassHighlight.blp"
        },
        { // Forest
            "Forest",
            "Tileset\\Ashenvale\\AshenvaleGrass.blp",
            "Tileset\\Ashenvale\\AshenvaleDirt.blp",
            "Tileset\\Ashenvale\\AshenvaleRoots.blp",
            "Tileset\\Ashenvale\\AshenvaleMossBase.blp"
        },
        { // Jungle
            "Jungle",
            "Tileset\\Stranglethorn\\StranglethornGrass.blp",
            "Tileset\\Stranglethorn\\StranglethornDirt03.blp",
            "Tileset\\Stranglethorn\\StranglethornMossRoot01.blp",
            "Tileset\\Stranglethorn\\StranglethornPlants01.blp"
        },
        { // Desert
            "Desert",
            "Tileset\\Tanaris\\TanarisSandBase01.blp",
            "Tileset\\Tanaris\\TanarisCrackedGround.blp",
            "Tileset\\Tanaris\\TanarisRockBase01.blp",
            "Tileset\\Tanaris\\TanarisSandBase02.blp"
        },
        { // Barrens
            "Barrens",
            "Tileset\\Barrens\\BarrensBaseDirt.blp",
            "Tileset\\Barrens\\BarrensBaseGrassGold.blp",
            "Tileset\\Barrens\\BarrensBaseRock.blp",
            "Tileset\\Barrens\\BarrensBaseDirtLighter.blp"
        },
        { // Snow
            "Snow",
            "Tileset\\Expansion02\\Dragonblight\\DragonblightFreshSmoothSnowA.blp",
            "Tileset\\Winterspring Grove\\WinterspringDirt.blp",
            "Tileset\\Winterspring Grove\\WinterspringRock.blp",
            "Tileset\\Winterspring Grove\\WinterspringRockSnow.blp"
        },
        { // Swamp
            "Swamp",
            "Tileset\\Wetlands\\WetlandsGrassDark01.blp",
            "Tileset\\Wetlands\\WetlandsDirt01.blp",
            "Tileset\\Wetlands\\WetlandsDirtMoss01.blp",
            "Tileset\\Wetlands\\WetlandsBaseRock.blp"
        },
        { // Rocky
            "Rocky",
            "Tileset\\Barrens\\BarrensRock01.blp",
            "Tileset\\Barrens\\BarrensBaseDirt.blp",
            "Tileset\\Desolace\\DesolaceRock01.blp",
            "Tileset\\Desolace\\DesolaceDirt.blp"
        },
        { // Beach
            "Beach",
            "Tileset\\Ashenvale\\AshenvaleSand.blp",
            "Tileset\\Feralas\\FeralasSand.blp",
            "Tileset\\Ashenvale\\AshenvaleShore.blp",
            "Tileset\\Feralas\\FeralasGrass.blp"
        },
        { // Volcanic
            "Volcanic",
            "Tileset\\Desolace\\DesolaceDirt.blp",
            "Tileset\\Desolace\\DesolaceCracks.blp",
            "Tileset\\Desolace\\DesolaceRock01.blp",
            "Tileset\\Tanaris\\TanarisRockBaseBurn.blp"
        }
    }};
    return biomes[static_cast<size_t>(biome)];
}

inline const char* getBiomeName(Biome b) {
    return getBiomeTextures(b).name;
}

} // namespace editor
} // namespace wowee
