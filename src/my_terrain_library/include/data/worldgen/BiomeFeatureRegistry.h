#pragma once

#include "levelgen/GenerationStep.h"
#include "levelgen/placement/PlacedFeature.h"
#include "data/worldgen/placement/OrePlacements.h"
#include "data/worldgen/placement/VegetationPlacements.h"
#include "data/worldgen/placement/TreePlacements.h"
#include "data/worldgen/placement/CavePlacements.h"
#include <unordered_map>
#include <vector>
#include <string>

// Reference: This is a custom registry to map biome keys to their features
// In Java, this is handled by BiomeGenerationSettings per biome

namespace minecraft {
namespace data {
namespace worldgen {

/**
 * BiomeFeatureRegistry - Global registry mapping biome keys to their PlacedFeatures
 *
 * This provides a centralized way to look up features for any biome without
 * modifying the Biome class structure. The registry is populated at bootstrap time.
 */
class BiomeFeatureRegistry {
private:
    // Features per biome per step: biome_key -> [step -> features]
    static std::unordered_map<std::string, std::vector<std::vector<const levelgen::placement::PlacedFeature*>>> s_biomeFeatures;
    static bool s_initialized;

public:
    /**
     * Bootstrap the registry - must be called before using features
     */
    static void bootstrap();

    /**
     * Check if initialized
     */
    static bool isInitialized() { return s_initialized; }

    /**
     * Get features for a biome at a specific generation step
     */
    static const std::vector<const levelgen::placement::PlacedFeature*>& getFeaturesForStep(
        const std::string& biomeKey,
        int step
    );

    /**
     * Get all features for a biome (as vector of vectors, indexed by step)
     */
    static const std::vector<std::vector<const levelgen::placement::PlacedFeature*>>& getFeaturesForBiome(
        const std::string& biomeKey
    );

    /**
     * Check if a biome has a specific feature
     */
    static bool hasFeature(const std::string& biomeKey, const levelgen::placement::PlacedFeature* feature);

    /**
     * Get all registered biome keys in the order they were bootstrapped
     * Reference: BiomeSource.possibleBiomes() in Java
     *
     * This order is CRITICAL for FeatureSorter - features get global indices
     * based on the order they're first seen when processing biomes.
     */
    static const std::vector<std::string>& getAllBiomeKeys();

private:
    // Ordered list of biome keys (order matters for FeatureSorter)
    static std::vector<std::string> s_biomeKeyOrder;
    /**
     * Add underground variety (decorating stones) to a biome
     * Reference: BiomeDefaultFeatures.java addDefaultUndergroundVariety() lines 28-39
     * Adds: ORE_DIRT, ORE_GRAVEL, ORE_GRANITE, ORE_DIORITE, ORE_ANDESITE, ORE_TUFF (UNDERGROUND_ORES)
     *       GLOW_LICHEN (VEGETAL_DECORATION)
     */
    static void addDefaultUndergroundVariety(const std::string& biomeKey);

    /**
     * Add default ores to a biome
     * Reference: BiomeDefaultFeatures.java addDefaultOres() lines 41-58
     * Adds actual ores: coal, iron, gold, redstone, diamond, lapis, copper, underwater_magma
     * @param largeCopperBlobs If true, use large copper blobs (for dripstone caves)
     */
    static void addDefaultOres(const std::string& biomeKey, bool largeCopperBlobs = false);

    /**
     * Add emerald ore (for mountain biomes)
     */
    static void addEmeraldOre(const std::string& biomeKey);

    /**
     * Add soft disks (sand, clay, gravel) for underwater areas
     * Reference: BiomeDefaultFeatures.java addDefaultSoftDisks()
     */
    static void addDefaultSoftDisks(const std::string& biomeKey);

    /**
     * Add default grass (for most biomes)
     * Reference: BiomeDefaultFeatures.java addDefaultGrass()
     */
    static void addDefaultGrass(const std::string& biomeKey);

    /**
     * Add plains grass (more grass than default)
     * Reference: BiomeDefaultFeatures.java addPlainGrass()
     */
    static void addPlainGrass(const std::string& biomeKey);

    /**
     * Add default flowers (dandelion/poppy)
     * Reference: BiomeDefaultFeatures.java addDefaultFlowers()
     */
    static void addDefaultFlowers(const std::string& biomeKey);

    /**
     * Add plains flowers (tulips, azure bluet, etc.)
     * Reference: BiomeDefaultFeatures.java addPlainFlowers()
     */
    static void addPlainFlowers(const std::string& biomeKey);

    /**
     * Add default mushrooms
     * Reference: BiomeDefaultFeatures.java addDefaultMushrooms()
     */
    static void addDefaultMushrooms(const std::string& biomeKey);

    /**
     * Add plains trees (sparse oak)
     * Reference: BiomeDefaultFeatures.java addPlainsTrees()
     */
    static void addPlainsTrees(const std::string& biomeKey);

    /**
     * Add forest trees (oak and birch mix)
     * Reference: BiomeDefaultFeatures.java addForestTrees()
     */
    static void addForestTrees(const std::string& biomeKey);

    /**
     * Add birch forest trees
     * Reference: BiomeDefaultFeatures.java addBirchTrees()
     */
    static void addBirchTrees(const std::string& biomeKey);

    /**
     * Add taiga trees (spruce)
     * Reference: BiomeDefaultFeatures.java addTaigaTrees()
     */
    static void addTaigaTrees(const std::string& biomeKey);

    /**
     * Add jungle trees
     * Reference: BiomeDefaultFeatures.java addJungleTrees()
     */
    static void addJungleTrees(const std::string& biomeKey);

    /**
     * Add savanna trees (acacia)
     * Reference: BiomeDefaultFeatures.java addSavannaTrees()
     */
    static void addSavannaTrees(const std::string& biomeKey);

    /**
     * Add dark forest trees (dark oak)
     * Reference: BiomeDefaultFeatures.java addDarkForestTrees()
     */
    static void addDarkForestTrees(const std::string& biomeKey);

    /**
     * Add monster rooms (dungeons) - both regular and deep variants
     * Reference: BiomeDefaultFeatures.java addDefaultMonsterRoom()
     */
    static void addDefaultMonsterRoom(const std::string& biomeKey);

    /**
     * Add cherry grove vegetation (cherry trees)
     * Reference: BiomeDefaultFeatures.java addCherryGroveVegetation()
     */
    static void addCherryGroveVegetation(const std::string& biomeKey);

    /**
     * Add leaf litter patches
     * Reference: BiomeDefaultFeatures.java addLeafLitterPatch()
     */
    static void addLeafLitterPatch(const std::string& biomeKey);

    // =============================================================================
    // Additional helper functions - Reference: BiomeDefaultFeatures.java
    // =============================================================================
    static void addDefaultCarversAndLakes(const std::string& biomeKey);
    static void addDefaultCrystalFormations(const std::string& biomeKey);
    static void addDefaultSprings(const std::string& biomeKey);
    static void addSurfaceFreezing(const std::string& biomeKey);
    static void globalOverworldGeneration(const std::string& biomeKey);
    static void addExtraGold(const std::string& biomeKey);
    static void addExtraEmeralds(const std::string& biomeKey);
    static void addInfestedStone(const std::string& biomeKey);
    static void addSwampClayDisk(const std::string& biomeKey);
    static void addMushroomFieldVegetation(const std::string& biomeKey);
    static void addNearWaterVegetation(const std::string& biomeKey);
    static void addBushes(const std::string& biomeKey);
    static void addPlainVegetation(const std::string& biomeKey);
    static void addDefaultExtraVegetation(const std::string& biomeKey, bool nearWater = false);
    static void addSwampVegetation(const std::string& biomeKey);
    static void addSwampExtraVegetation(const std::string& biomeKey);
    static void addFossilDecoration(const std::string& biomeKey);
    static void addWaterTrees(const std::string& biomeKey);
    static void addColdOceanExtraVegetation(const std::string& biomeKey);
    static void addLukeWarmKelp(const std::string& biomeKey);
    static void addFerns(const std::string& biomeKey);
    static void addTaigaGrass(const std::string& biomeKey);
    static void addGiantTaigaVegetation(const std::string& biomeKey);
    static void addBadlandsGrass(const std::string& biomeKey);
    static void addBadlandsVegetation(const std::string& biomeKey);
    static void addDesertVegetation(const std::string& biomeKey);
    static void addSnowySpruceTrees(const std::string& biomeKey);
    static void addMeadowVegetation(const std::string& biomeKey);
    static void addWindsweptHillsTrees(const std::string& biomeKey);
    static void addSparseJungleTrees(const std::string& biomeKey);
    static void addBambooJungleVegetation(const std::string& biomeKey);
    static void addMangroveSwampVegetation(const std::string& biomeKey);
    static void addPaleGardenVegetation(const std::string& biomeKey);
    static void addLushCavesVegetation(const std::string& biomeKey);
    static void addDripstoneClusterVegetation(const std::string& biomeKey);
    static void addCommonBerryBushes(const std::string& biomeKey);
    static void addRareBerryBushes(const std::string& biomeKey);
    static void addForestFlowers(const std::string& biomeKey);
    static void addTallBirchTrees(const std::string& biomeKey);
    static void addOtherBirchTrees(const std::string& biomeKey);
    static void addBirchForestFlowers(const std::string& biomeKey);
    static void addForestGrass(const std::string& biomeKey);
    static void addShatteredSavannaTrees(const std::string& biomeKey);
    static void addSavannaGrass(const std::string& biomeKey);
    static void addShatteredSavannaGrass(const std::string& biomeKey);
    static void addSavannaExtraGrass(const std::string& biomeKey);
    static void addWarmFlowers(const std::string& biomeKey);
    static void addJungleGrass(const std::string& biomeKey);
    static void addJungleVines(const std::string& biomeKey);
    static void addJungleMelons(const std::string& biomeKey);
    static void addSparseJungleMelons(const std::string& biomeKey);
    static void addLightBambooVegetation(const std::string& biomeKey);
    static void addBambooVegetation(const std::string& biomeKey);
    static void addSeagrassVegetation(const std::string& biomeKey, int amount);
    static void addKelpVegetation(const std::string& biomeKey);
    static void addWarmOceanVegetation(const std::string& biomeKey);
    static void addBadlandsTrees(const std::string& biomeKey);
    static void addBadlandGrass(const std::string& biomeKey);
    static void addBadlandExtraVegetation(const std::string& biomeKey);
    static void addDesertExtraVegetation(const std::string& biomeKey);
    static void addDesertExtraDecoration(const std::string& biomeKey);
    static void addMountainTrees(const std::string& biomeKey);
    static void addMountainForestTrees(const std::string& biomeKey);
    static void addSnowyTrees(const std::string& biomeKey);
    static void addFrozenSprings(const std::string& biomeKey);
    static void addGroveTrees(const std::string& biomeKey);
    static void addIcebergs(const std::string& biomeKey);
    static void addBlueIce(const std::string& biomeKey);
    static void addMossyStoneBlock(const std::string& biomeKey);
    static void addDripstone(const std::string& biomeKey);
    static void addSculk(const std::string& biomeKey);
    static void addLushCavesVegetationFeatures(const std::string& biomeKey);
    static void addLushCavesSpecialOres(const std::string& biomeKey);
    static void setupMushroomFields(const std::string& biomeKey);
    static void setupPlains(const std::string& biomeKey, bool sunflower, bool snowy, bool spikes);
    static void setupSwamp(const std::string& biomeKey);
    static void setupOcean(const std::string& biomeKey, bool deep);
    static void setupColdOcean(const std::string& biomeKey, bool deep);
    static void setupLukewarmOcean(const std::string& biomeKey, bool deep);
    static void setupWarmOcean(const std::string& biomeKey);
    static void setupFrozenOcean(const std::string& biomeKey, bool deep);
    static void setupTaiga(const std::string& biomeKey, bool snowy);
    static void setupForest(const std::string& biomeKey, bool birch, bool tall, bool flower);
    static void setupDarkForest(const std::string& biomeKey, bool isPaleGarden);
    static void setupSavanna(const std::string& biomeKey, bool shattered, bool plateau);
    static void setupBadlands(const std::string& biomeKey, bool wooded);
    static void setupDesert(const std::string& biomeKey);
    static void setupWindsweptHills(const std::string& biomeKey, bool moreTrees);
    static void setupRiver(const std::string& biomeKey, bool frozen);
    static void setupBeach(const std::string& biomeKey, bool snowy, bool stony);
    static void setupMeadowOrCherryGrove(const std::string& biomeKey, bool cherryGrove);
    static void setupPeaks(const std::string& biomeKey, bool stony);
    static void setupSnowySlopes(const std::string& biomeKey);
    static void setupGrove(const std::string& biomeKey);
    static void setupJungle(const std::string& biomeKey, bool bamboo, bool sparse, bool core);
    static void setupOldGrowthTaiga(const std::string& biomeKey, bool spruce);
    static void setupLushCaves(const std::string& biomeKey);
    static void setupDripstoneCaves(const std::string& biomeKey);
    static void setupDeepDark(const std::string& biomeKey);
    static void setupMangroveSwamp(const std::string& biomeKey);
    static void registerForestFeatures(const std::string& biomeKey);
    static void registerPlainsFeatures(const std::string& biomeKey);
    static void registerSwampFeatures(const std::string& biomeKey);
    static void registerJungleFeatures(const std::string& biomeKey);
    static void registerSparseJungleFeatures(const std::string& biomeKey);
    static void registerBambooJungleFeatures(const std::string& biomeKey);
    static void registerTaigaFeatures(const std::string& biomeKey);
    static void registerOldGrowthTaigaFeatures(const std::string& biomeKey);
    static void registerSnowyTaigaFeatures(const std::string& biomeKey);
    static void registerGroveTaigaFeatures(const std::string& biomeKey);
    static void registerBirchForestFeatures(const std::string& biomeKey);
    static void registerOldGrowthBirchFeatures(const std::string& biomeKey);
    static void registerBadlandsFeatures(const std::string& biomeKey);
    static void registerDesertFeatures(const std::string& biomeKey);
    static void registerSavannaFeatures(const std::string& biomeKey);
    static void registerWindsweptSavannaFeatures(const std::string& biomeKey);
    static void registerWindsweptHillsFeatures(const std::string& biomeKey);
    static void registerDarkForestFeatures(const std::string& biomeKey);
    static void registerFlowerForestFeatures(const std::string& biomeKey);
    static void registerMeadowFeatures(const std::string& biomeKey);
    static void registerCherryGroveFeatures(const std::string& biomeKey);
    static void registerSnowyFeatures(const std::string& biomeKey);
    static void registerSnowyBeachFeatures(const std::string& biomeKey);
    static void registerBeachFeatures(const std::string& biomeKey);
    static void registerStonyShoreFeatures(const std::string& biomeKey);
    static void registerOceanFeatures(const std::string& biomeKey);
    static void registerDeepOceanFeatures(const std::string& biomeKey);
    static void registerColdOceanFeatures(const std::string& biomeKey);
    static void registerFrozenOceanFeatures(const std::string& biomeKey);
    static void registerLukeWarmOceanFeatures(const std::string& biomeKey);
    static void registerWarmOceanFeatures(const std::string& biomeKey);
    static void registerMushroomIslandFeatures(const std::string& biomeKey);
    static void registerRiverFeatures(const std::string& biomeKey);
    static void registerMangroveSwampFeatures(const std::string& biomeKey);
    static void registerPaleGardenFeatures(const std::string& biomeKey);
    static void registerLushCavesFeatures(const std::string& biomeKey);
    static void registerDripstoneCavesFeatures(const std::string& biomeKey);
    static void registerDeepDarkFeatures(const std::string& biomeKey);

    /**
     * Helper to add a feature to a biome
     */
    static void addFeature(const std::string& biomeKey, int step, const levelgen::placement::PlacedFeature* feature);
};

} // namespace worldgen
} // namespace data
} // namespace minecraft
