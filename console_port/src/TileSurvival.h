#pragma once
namespace console {
// Material registry order from Material::staticCtor.
enum class SurvivalMaterial {
    air, grass, dirt, wood, stone, metal, heavyMetal, water, lava, leaves, plant,
    replaceable_plant, sponge, cloth, fire, sand, decoration, clothDecoration, glass,
    buildable_glass, explosive, coral, ice, topSnow, snow, cactus, clay, vegetable, egg,
    portal, cake, web, piston
};
// One registered tile from Tile::staticCtor (ported/TileSurvival.cpp is generated
// by tools/extract_survival_tiles.py). destroyTime is Tile::destroySpeed; -1 is
// setIndestructible. `reconstructed` marks a material whose tile class source is
// not part of the supplied archive subset.
struct SurvivalTile {
    int id;
    float destroyTime;
    SurvivalMaterial material;
    bool reconstructed;
};
const SurvivalTile* consoleSurvivalTile(int id);
}
