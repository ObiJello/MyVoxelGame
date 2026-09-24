#include "world/biome/Biome.h"
#include "world/biome/Biomes.h"

namespace minecraft {
namespace world {
namespace biome {

namespace {

// The Twilight Forest biomes (data/twilightforest/worldgen/biome/*.json,
// BiomeMaker datagen): temperature, downfall, has_precipitation and the
// temperature_modifier the two snow biomes carry ("frozen").
struct TwilightClimate {
    const char* key;
    bool hasPrecipitation;
    float temperature;
    float downfall;
    bool frozen;
};

constexpr TwilightClimate kTwilightClimates[] = {
    {"twilightforest:clearing",              true,  0.8f,  0.4f, false},
    {"twilightforest:dark_forest",           true,  0.7f,  0.8f, false},
    {"twilightforest:dark_forest_center",    true,  0.7f,  0.8f, false},
    {"twilightforest:dense_forest",          true,  0.7f,  0.8f, false},
    {"twilightforest:dense_mushroom_forest", true,  0.8f,  1.0f, false},
    {"twilightforest:enchanted_forest",      false, 0.5f,  0.0f, false},
    {"twilightforest:final_plateau",         true,  1.0f,  0.2f, false},
    {"twilightforest:fire_swamp",            false, 1.0f,  0.4f, false},
    {"twilightforest:firefly_forest",        true,  0.5f,  1.0f, false},
    {"twilightforest:forest",                true,  0.5f,  0.5f, false},
    {"twilightforest:glacier",               true,  0.08f, 0.1f, true},
    {"twilightforest:highlands",             true,  0.4f,  0.7f, false},
    {"twilightforest:highlands_underground", true,  0.35f, 0.0f, false},
    {"twilightforest:lake",                  true,  0.5f,  0.1f, false},
    {"twilightforest:mushroom_forest",       true,  0.8f,  0.8f, false},
    {"twilightforest:oak_savannah",          false, 0.9f,  0.0f, false},
    {"twilightforest:snowy_forest",          true,  0.09f, 0.9f, true},
    {"twilightforest:spooky_forest",         true,  0.5f,  1.0f, false},
    {"twilightforest:stream",                true,  0.5f,  0.1f, false},
    {"twilightforest:swamp",                 true,  0.8f,  0.9f, false},
    {"twilightforest:thornlands",            true,  0.3f,  0.2f, false},
    {"twilightforest:underground",           true,  0.7f,  0.0f, false},
};

const TwilightClimate* twilightClimate(const BiomeKey& key) {
    for (const TwilightClimate& c : kTwilightClimates) {
        if (key == c.key) return &c;
    }
    return nullptr;
}

} // namespace

BiomeHolder Biomes::get(const BiomeKey& key) {
    // Whole find-or-create is under the registry mutex: get() is reached
    // from parallel worldgen phases and inserts on miss.
    std::lock_guard<std::mutex> lock(registryMutex());
    auto& reg = registry();
    auto it = reg.find(key);
    if (it != reg.end()) {
        return it->second;
    }

    // Create biome with proper climate settings based on key
    // Reference: OverworldBiomes.java, NetherBiomes.java, EndBiomes.java
    Biome* newBiome = nullptr;

    // ==================== FROZEN OCEAN BIOMES (with FROZEN temperature modifier) ====================
    if (key == BiomeKeys::FROZEN_OCEAN) {
        // Reference: OverworldBiomes.java frozenOcean() line 346, 358
        // temperature: 0.0, temperatureModifier: FROZEN
        ClimateSettings climate(true, 0.0f, TemperatureModifier::FROZEN, 0.5f);
        newBiome = new Biome(10, key, climate);
    } else if (key == BiomeKeys::DEEP_FROZEN_OCEAN) {
        // Reference: OverworldBiomes.java frozenOcean() line 346, 358
        // temperature: 0.5, temperatureModifier: FROZEN
        ClimateSettings climate(true, 0.5f, TemperatureModifier::FROZEN, 0.5f);
        newBiome = new Biome(50, key, climate);
    }
    // ==================== SNOWY/COLD BIOMES ====================
    else if (key == BiomeKeys::SNOWY_PLAINS || key == BiomeKeys::ICE_SPIKES) {
        // Reference: OverworldBiomes.java plains() line 208 - snowy variant
        ClimateSettings climate(true, 0.0f, TemperatureModifier::NONE, 0.5f);
        newBiome = new Biome(12, key, climate);
    } else if (key == BiomeKeys::SNOWY_TAIGA) {
        // Reference: OverworldBiomes.java taiga() line 431 - snowy variant
        ClimateSettings climate(true, -0.5f, TemperatureModifier::NONE, 0.4f);
        newBiome = new Biome(30, key, climate);
    } else if (key == BiomeKeys::SNOWY_BEACH) {
        // Reference: OverworldBiomes.java beach() line 540 - snowy variant
        ClimateSettings climate(true, 0.05f, TemperatureModifier::NONE, 0.3f);
        newBiome = new Biome(26, key, climate);
    } else if (key == BiomeKeys::FROZEN_RIVER) {
        // Reference: OverworldBiomes.java river() line 519 - frozen variant
        ClimateSettings climate(true, 0.0f, TemperatureModifier::NONE, 0.5f);
        newBiome = new Biome(11, key, climate);
    } else if (key == BiomeKeys::SNOWY_SLOPES) {
        // Reference: OverworldBiomes.java snowySlopes() line 628
        ClimateSettings climate(true, -0.3f, TemperatureModifier::NONE, 0.9f);
        newBiome = new Biome(183, key, climate);
    } else if (key == BiomeKeys::FROZEN_PEAKS || key == BiomeKeys::JAGGED_PEAKS) {
        // Reference: OverworldBiomes.java basePeaks() line 593
        ClimateSettings climate(true, -0.7f, TemperatureModifier::NONE, 0.9f);
        newBiome = new Biome(184, key, climate);
    } else if (key == BiomeKeys::GROVE) {
        // Reference: OverworldBiomes.java grove() line 644
        ClimateSettings climate(true, -0.2f, TemperatureModifier::NONE, 0.8f);
        newBiome = new Biome(185, key, climate);
    }
    // ==================== OCEAN BIOMES ====================
    else if (key == BiomeKeys::OCEAN || key == BiomeKeys::DEEP_OCEAN) {
        // Reference: OverworldBiomes.java baseOcean() line 283
        ClimateSettings climate(true, 0.5f, TemperatureModifier::NONE, 0.5f);
        newBiome = new Biome(0, key, climate);
    } else if (key == BiomeKeys::COLD_OCEAN || key == BiomeKeys::DEEP_COLD_OCEAN) {
        // Reference: OverworldBiomes.java coldOcean() - uses baseOcean
        ClimateSettings climate(true, 0.5f, TemperatureModifier::NONE, 0.5f);
        newBiome = new Biome(46, key, climate);
    } else if (key == BiomeKeys::LUKEWARM_OCEAN || key == BiomeKeys::DEEP_LUKEWARM_OCEAN) {
        // Reference: OverworldBiomes.java lukeWarmOcean() - uses baseOcean
        ClimateSettings climate(true, 0.5f, TemperatureModifier::NONE, 0.5f);
        newBiome = new Biome(45, key, climate);
    } else if (key == BiomeKeys::WARM_OCEAN) {
        // Reference: OverworldBiomes.java warmOcean() - uses baseOcean
        ClimateSettings climate(true, 0.5f, TemperatureModifier::NONE, 0.5f);
        newBiome = new Biome(44, key, climate);
    }
    // ==================== RIVER BIOMES ====================
    else if (key == BiomeKeys::RIVER) {
        // Reference: OverworldBiomes.java river() line 519 - normal variant
        ClimateSettings climate(true, 0.5f, TemperatureModifier::NONE, 0.5f);
        newBiome = new Biome(7, key, climate);
    }
    // ==================== BEACH BIOMES ====================
    else if (key == BiomeKeys::BEACH) {
        // Reference: OverworldBiomes.java beach() line 544 - normal sandy beach
        ClimateSettings climate(true, 0.8f, TemperatureModifier::NONE, 0.4f);
        newBiome = new Biome(16, key, climate);
    } else if (key == BiomeKeys::STONY_SHORE) {
        // Reference: OverworldBiomes.java beach() line 542 - stony variant
        ClimateSettings climate(true, 0.2f, TemperatureModifier::NONE, 0.3f);
        newBiome = new Biome(25, key, climate);
    }
    // ==================== PLAINS BIOMES ====================
    else if (key == BiomeKeys::PLAINS || key == BiomeKeys::SUNFLOWER_PLAINS) {
        // Reference: OverworldBiomes.java plains() line 208 - normal variant
        ClimateSettings climate(true, 0.8f, TemperatureModifier::NONE, 0.4f);
        newBiome = new Biome(1, key, climate);
    }
    // ==================== FOREST BIOMES ====================
    else if (key == BiomeKeys::FOREST) {
        // Reference: OverworldBiomes.java forest() line 407 - normal forest
        ClimateSettings climate(true, 0.7f, TemperatureModifier::NONE, 0.8f);
        newBiome = new Biome(4, key, climate);
    } else if (key == BiomeKeys::FLOWER_FOREST) {
        // Reference: OverworldBiomes.java forest() line 407 - flower variant
        ClimateSettings climate(true, 0.7f, TemperatureModifier::NONE, 0.8f);
        newBiome = new Biome(132, key, climate);
    } else if (key == BiomeKeys::BIRCH_FOREST || key == BiomeKeys::OLD_GROWTH_BIRCH_FOREST) {
        // Reference: OverworldBiomes.java forest() line 407 - birch variant
        ClimateSettings climate(true, 0.6f, TemperatureModifier::NONE, 0.6f);
        newBiome = new Biome(27, key, climate);
    } else if (key == BiomeKeys::DAPPLED_FOREST) {
        // Reference: OverworldBiomes.java dappledForest() (26.3) - baseBiome(0.6, 0.6)
        ClimateSettings climate(true, 0.6f, TemperatureModifier::NONE, 0.6f);
        newBiome = new Biome(27, key, climate);
    } else if (key == BiomeKeys::DARK_FOREST || key == BiomeKeys::PALE_GARDEN) {
        // Reference: OverworldBiomes.java darkForest() line 468
        ClimateSettings climate(true, 0.7f, TemperatureModifier::NONE, 0.8f);
        newBiome = new Biome(29, key, climate);
    }
    // ==================== TAIGA BIOMES ====================
    else if (key == BiomeKeys::TAIGA) {
        // Reference: OverworldBiomes.java taiga() line 431 - normal variant
        ClimateSettings climate(true, 0.25f, TemperatureModifier::NONE, 0.8f);
        newBiome = new Biome(5, key, climate);
    } else if (key == BiomeKeys::OLD_GROWTH_PINE_TAIGA) {
        // Reference: OverworldBiomes.java oldGrowthTaiga() line 76 - pine variant
        ClimateSettings climate(true, 0.3f, TemperatureModifier::NONE, 0.8f);
        newBiome = new Biome(32, key, climate);
    } else if (key == BiomeKeys::OLD_GROWTH_SPRUCE_TAIGA) {
        // Reference: OverworldBiomes.java oldGrowthTaiga() line 76 - spruce variant
        ClimateSettings climate(true, 0.25f, TemperatureModifier::NONE, 0.8f);
        newBiome = new Biome(160, key, climate);
    }
    // ==================== JUNGLE BIOMES ====================
    else if (key == BiomeKeys::JUNGLE || key == BiomeKeys::BAMBOO_JUNGLE) {
        // Reference: OverworldBiomes.java baseJungle() line 130
        ClimateSettings climate(true, 0.95f, TemperatureModifier::NONE, 0.9f);
        newBiome = new Biome(21, key, climate);
    } else if (key == BiomeKeys::SPARSE_JUNGLE) {
        // Reference: OverworldBiomes.java sparseJungle() - uses baseJungle with 0.8 downfall
        ClimateSettings climate(true, 0.95f, TemperatureModifier::NONE, 0.8f);
        newBiome = new Biome(23, key, climate);
    }
    // ==================== SWAMP BIOMES ====================
    else if (key == BiomeKeys::SWAMP || key == BiomeKeys::MANGROVE_SWAMP) {
        // Reference: OverworldBiomes.java swamp() line 484
        ClimateSettings climate(true, 0.8f, TemperatureModifier::NONE, 0.9f);
        newBiome = new Biome(6, key, climate);
    }
    // ==================== MOUNTAIN BIOMES ====================
    else if (key == BiomeKeys::WINDSWEPT_HILLS || key == BiomeKeys::WINDSWEPT_GRAVELLY_HILLS || key == BiomeKeys::WINDSWEPT_FOREST) {
        // Reference: OverworldBiomes.java windsweptHills() line 155
        ClimateSettings climate(true, 0.2f, TemperatureModifier::NONE, 0.3f);
        newBiome = new Biome(3, key, climate);
    } else if (key == BiomeKeys::STONY_PEAKS) {
        // Reference: OverworldBiomes.java stonyPeaks() line 613
        ClimateSettings climate(true, 1.0f, TemperatureModifier::NONE, 0.3f);
        newBiome = new Biome(186, key, climate);
    } else if (key == BiomeKeys::MEADOW || key == BiomeKeys::CHERRY_GROVE) {
        // Reference: OverworldBiomes.java meadowOrCherryGrove() line 576
        ClimateSettings climate(true, 0.5f, TemperatureModifier::NONE, 0.8f);
        newBiome = new Biome(187, key, climate);
    }
    // ==================== HOT/DRY BIOMES (no precipitation) ====================
    else if (key == BiomeKeys::DESERT) {
        // Reference: OverworldBiomes.java desert() line 172
        ClimateSettings climate(false, 2.0f, TemperatureModifier::NONE, 0.0f);
        newBiome = new Biome(2, key, climate);
    } else if (key == BiomeKeys::SAVANNA || key == BiomeKeys::SAVANNA_PLATEAU || key == BiomeKeys::WINDSWEPT_SAVANNA) {
        // Reference: OverworldBiomes.java savanna() line 253
        ClimateSettings climate(false, 2.0f, TemperatureModifier::NONE, 0.0f);
        newBiome = new Biome(35, key, climate);
    } else if (key == BiomeKeys::BADLANDS || key == BiomeKeys::ERODED_BADLANDS || key == BiomeKeys::WOODED_BADLANDS) {
        // Reference: OverworldBiomes.java badlands() line 279
        ClimateSettings climate(false, 2.0f, TemperatureModifier::NONE, 0.0f);
        newBiome = new Biome(37, key, climate);
    }
    // ==================== CAVE BIOMES ====================
    else if (key == BiomeKeys::LUSH_CAVES) {
        // Reference: OverworldBiomes.java lushCaves() line 659
        ClimateSettings climate(true, 0.5f, TemperatureModifier::NONE, 0.5f);
        newBiome = new Biome(188, key, climate);
    } else if (key == BiomeKeys::DRIPSTONE_CAVES || key == BiomeKeys::DEEP_DARK || key == BiomeKeys::SULFUR_CAVES) {
        // sulfurCaves() (26.3) is baseBiome(0.8, 0.4) too
        // Reference: OverworldBiomes.java dripstoneCaves() line 674, deepDark() line 694
        ClimateSettings climate(true, 0.8f, TemperatureModifier::NONE, 0.4f);
        newBiome = new Biome(189, key, climate);
    } else if (key == BiomeKeys::MUSHROOM_FIELDS) {
        // Reference: OverworldBiomes.java mushroomFields() line 220
        ClimateSettings climate(true, 0.9f, TemperatureModifier::NONE, 1.0f);
        newBiome = new Biome(14, key, climate);
    }
    // ==================== SPECIAL BIOMES ====================
    else if (key == BiomeKeys::THE_VOID) {
        // Reference: OverworldBiomes.java theVoid() line 554
        ClimateSettings climate(false, 0.5f, TemperatureModifier::NONE, 0.5f);
        newBiome = new Biome(127, key, climate);
    }
    // ==================== NETHER BIOMES (all 2.0F, no precipitation) ====================
    else if (key == BiomeKeys::NETHER_WASTES || key == BiomeKeys::CRIMSON_FOREST ||
             key == BiomeKeys::WARPED_FOREST || key == BiomeKeys::SOUL_SAND_VALLEY ||
             key == BiomeKeys::BASALT_DELTAS) {
        // Reference: NetherBiomes.java baseBiome() line 33
        ClimateSettings climate(false, 2.0f, TemperatureModifier::NONE, 0.0f);
        newBiome = new Biome(8, key, climate);
    }
    // ==================== END BIOMES (all 0.5F, no precipitation) ====================
    else if (key == BiomeKeys::THE_END || key == BiomeKeys::END_HIGHLANDS ||
             key == BiomeKeys::END_MIDLANDS || key == BiomeKeys::SMALL_END_ISLANDS ||
             key == BiomeKeys::END_BARRENS) {
        // Reference: EndBiomes.java baseEndBiome() line 18
        ClimateSettings climate(false, 0.5f, TemperatureModifier::NONE, 0.5f);
        newBiome = new Biome(9, key, climate);
    }
    // ==================== THE HUSH (engine-only, temperature 0.5, no precipitation) ====================
    else if (key == BiomeKeys::HUSH_MEADOWS || key == BiomeKeys::WHISPERWOOD_FOREST ||
             key == BiomeKeys::RESONANT_BARRENS || key == BiomeKeys::CRYSTAL_CAVERNS ||
             key == BiomeKeys::SUNKEN_CHOIR || key == BiomeKeys::HOLLOW_DEEP) {
        // No vanilla counterpart: mirrors data/minecraft/worldgen/biome/
        // {hush_meadows,whisperwood_forest,resonant_barrens,crystal_caverns,
        // sunken_choir,hollow_deep}.json (temperature 0.5, downfall 0.4,
        // has_precipitation false). The numeric id is the unused legacy
        // slot; nothing keys on it.
        ClimateSettings climate(false, 0.5f, TemperatureModifier::NONE, 0.4f);
        newBiome = new Biome(200, key, climate);
    }
    else if (key == BiomeKeys::AURORA_STEPPE) {
        // data/minecraft/worldgen/biome/aurora_steppe.json: the Hush's cold
        // plateau (temperature 0.0, downfall 0.3, has_precipitation false —
        // nothing ever falls in the Hush, so the cold only shows in water
        // that a freeze pass would reach).
        ClimateSettings climate(false, 0.0f, TemperatureModifier::NONE, 0.3f);
        newBiome = new Biome(200, key, climate);
    }
    // ==================== THE AETHER (temperature 0.8, downfall 0, no precipitation) ====================
    else if (key == BiomeKeys::SKYROOT_MEADOW || key == BiomeKeys::SKYROOT_FOREST ||
             key == BiomeKeys::SKYROOT_GROVE || key == BiomeKeys::SKYROOT_WOODLAND) {
        // data/aether/worldgen/biome/{skyroot_meadow,skyroot_forest,
        // skyroot_grove,skyroot_woodland}.json (AetherBiomeBuilders
        // .makeDefaultBiome): temperature 0.8, downfall 0.0,
        // has_precipitation false, no temperature modifier. The numeric id
        // is an unused legacy slot; nothing keys on it.
        ClimateSettings climate(false, 0.8f, TemperatureModifier::NONE, 0.0f);
        newBiome = new Biome(201, key, climate);
    }
    // ==================== THE TWILIGHT FOREST (22 biomes, see kTwilightClimates) ====================
    else if (const TwilightClimate* tf = twilightClimate(key)) {
        ClimateSettings climate(tf->hasPrecipitation, tf->temperature,
                                tf->frozen ? TemperatureModifier::FROZEN : TemperatureModifier::NONE,
                                tf->downfall);
        // The numeric id is an unused legacy slot; nothing keys on it.
        newBiome = new Biome(202, key, climate);
    }
    // ==================== FALLBACK (should not be reached for known biomes) ====================
    else {
        // For any unknown biome, use a neutral default
        // This prevents temperature-dependent features from misbehaving
        ClimateSettings climate(true, 0.5f, TemperatureModifier::NONE, 0.5f);
        newBiome = new Biome(0, key, climate);
    }

    reg[key] = newBiome;
    return newBiome;
}

} // namespace biome
} // namespace world
} // namespace minecraft
