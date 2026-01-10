#pragma once

#include <string>
#include <cstdint>
#include <unordered_map>

// Forward declaration
namespace minecraft { namespace world { namespace biome { class Biome; } } }

namespace minecraft {
namespace world {
namespace biome {

/**
 * BiomeKey - Resource key for a biome
 * Reference: ResourceKey<Biome> in Java
 *
 * This is a simple string-based key identifying biomes.
 */
using BiomeKey = std::string;

/**
 * BiomeHolder - Reference to a biome
 */
using BiomeHolder = const Biome*;

/**
 * BiomeKeys - Static biome key constants
 * Reference: net/minecraft/world/level/biome/Biomes.java
 */
struct BiomeKeys {
    // Oceans
    static constexpr const char* OCEAN = "minecraft:ocean";
    static constexpr const char* DEEP_OCEAN = "minecraft:deep_ocean";
    static constexpr const char* FROZEN_OCEAN = "minecraft:frozen_ocean";
    static constexpr const char* DEEP_FROZEN_OCEAN = "minecraft:deep_frozen_ocean";
    static constexpr const char* COLD_OCEAN = "minecraft:cold_ocean";
    static constexpr const char* DEEP_COLD_OCEAN = "minecraft:deep_cold_ocean";
    static constexpr const char* LUKEWARM_OCEAN = "minecraft:lukewarm_ocean";
    static constexpr const char* DEEP_LUKEWARM_OCEAN = "minecraft:deep_lukewarm_ocean";
    static constexpr const char* WARM_OCEAN = "minecraft:warm_ocean";

    // Plains variants
    static constexpr const char* PLAINS = "minecraft:plains";
    static constexpr const char* SUNFLOWER_PLAINS = "minecraft:sunflower_plains";
    static constexpr const char* SNOWY_PLAINS = "minecraft:snowy_plains";

    // Forests
    static constexpr const char* FOREST = "minecraft:forest";
    static constexpr const char* FLOWER_FOREST = "minecraft:flower_forest";
    static constexpr const char* BIRCH_FOREST = "minecraft:birch_forest";
    static constexpr const char* OLD_GROWTH_BIRCH_FOREST = "minecraft:old_growth_birch_forest";
    static constexpr const char* DARK_FOREST = "minecraft:dark_forest";
    static constexpr const char* PALE_GARDEN = "minecraft:pale_garden";

    // Taiga variants
    static constexpr const char* TAIGA = "minecraft:taiga";
    static constexpr const char* SNOWY_TAIGA = "minecraft:snowy_taiga";
    static constexpr const char* OLD_GROWTH_PINE_TAIGA = "minecraft:old_growth_pine_taiga";
    static constexpr const char* OLD_GROWTH_SPRUCE_TAIGA = "minecraft:old_growth_spruce_taiga";

    // Jungle variants
    static constexpr const char* JUNGLE = "minecraft:jungle";
    static constexpr const char* SPARSE_JUNGLE = "minecraft:sparse_jungle";
    static constexpr const char* BAMBOO_JUNGLE = "minecraft:bamboo_jungle";

    // Deserts and badlands
    static constexpr const char* DESERT = "minecraft:desert";
    static constexpr const char* BADLANDS = "minecraft:badlands";
    static constexpr const char* ERODED_BADLANDS = "minecraft:eroded_badlands";
    static constexpr const char* WOODED_BADLANDS = "minecraft:wooded_badlands";

    // Savanna variants
    static constexpr const char* SAVANNA = "minecraft:savanna";
    static constexpr const char* SAVANNA_PLATEAU = "minecraft:savanna_plateau";
    static constexpr const char* WINDSWEPT_SAVANNA = "minecraft:windswept_savanna";

    // Mountains and hills
    static constexpr const char* WINDSWEPT_HILLS = "minecraft:windswept_hills";
    static constexpr const char* WINDSWEPT_GRAVELLY_HILLS = "minecraft:windswept_gravelly_hills";
    static constexpr const char* WINDSWEPT_FOREST = "minecraft:windswept_forest";
    static constexpr const char* MEADOW = "minecraft:meadow";
    static constexpr const char* CHERRY_GROVE = "minecraft:cherry_grove";
    static constexpr const char* GROVE = "minecraft:grove";
    static constexpr const char* SNOWY_SLOPES = "minecraft:snowy_slopes";
    static constexpr const char* FROZEN_PEAKS = "minecraft:frozen_peaks";
    static constexpr const char* JAGGED_PEAKS = "minecraft:jagged_peaks";
    static constexpr const char* STONY_PEAKS = "minecraft:stony_peaks";

    // Swamps
    static constexpr const char* SWAMP = "minecraft:swamp";
    static constexpr const char* MANGROVE_SWAMP = "minecraft:mangrove_swamp";

    // Beaches and shores
    static constexpr const char* BEACH = "minecraft:beach";
    static constexpr const char* SNOWY_BEACH = "minecraft:snowy_beach";
    static constexpr const char* STONY_SHORE = "minecraft:stony_shore";

    // Rivers
    static constexpr const char* RIVER = "minecraft:river";
    static constexpr const char* FROZEN_RIVER = "minecraft:frozen_river";

    // Ice
    static constexpr const char* ICE_SPIKES = "minecraft:ice_spikes";

    // Mushroom
    static constexpr const char* MUSHROOM_FIELDS = "minecraft:mushroom_fields";

    // Cave biomes
    static constexpr const char* DRIPSTONE_CAVES = "minecraft:dripstone_caves";
    static constexpr const char* LUSH_CAVES = "minecraft:lush_caves";
    static constexpr const char* DEEP_DARK = "minecraft:deep_dark";

    // Nether (for completeness)
    static constexpr const char* NETHER_WASTES = "minecraft:nether_wastes";
    static constexpr const char* SOUL_SAND_VALLEY = "minecraft:soul_sand_valley";
    static constexpr const char* CRIMSON_FOREST = "minecraft:crimson_forest";
    static constexpr const char* WARPED_FOREST = "minecraft:warped_forest";
    static constexpr const char* BASALT_DELTAS = "minecraft:basalt_deltas";

    // End (for completeness)
    static constexpr const char* THE_END = "minecraft:the_end";
    static constexpr const char* END_HIGHLANDS = "minecraft:end_highlands";
    static constexpr const char* END_MIDLANDS = "minecraft:end_midlands";
    static constexpr const char* SMALL_END_ISLANDS = "minecraft:small_end_islands";
    static constexpr const char* END_BARRENS = "minecraft:end_barrens";

    // The void
    static constexpr const char* THE_VOID = "minecraft:the_void";
};

/**
 * Biomes - Registry for biome instances
 * Provides lookup from BiomeKey to Biome*
 */
class Biomes {
private:
    static std::unordered_map<BiomeKey, Biome*>& registry() {
        static std::unordered_map<BiomeKey, Biome*> s_registry;
        return s_registry;
    }

public:
    /**
     * Register a biome in the registry
     */
    static void registerBiome(const BiomeKey& key, Biome* biome) {
        registry()[key] = biome;
    }

    /**
     * Get a biome by key
     * Auto-creates a minimal biome if not found
     * Implementation in Biomes.cpp
     */
    static BiomeHolder get(const BiomeKey& key);

    /**
     * Check if a biome is registered
     */
    static bool has(const BiomeKey& key) {
        return registry().find(key) != registry().end();
    }
};

} // namespace biome
} // namespace world
} // namespace minecraft
