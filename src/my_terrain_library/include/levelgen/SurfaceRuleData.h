#pragma once

#include "levelgen/SurfaceRules.h"
#include "world/level/block/state/BlockState.h"
#include <string>

// Reference: net/minecraft/data/worldgen/SurfaceRuleData.java

namespace minecraft {
namespace levelgen {

/**
 * SurfaceRuleData - Factory for creating surface rule configurations
 * Reference: SurfaceRuleData.java
 */
class SurfaceRuleData {
private:
    // Block state rule sources - Reference: lines 12-46
    static RuleSource* AIR;
    static RuleSource* BEDROCK;
    static RuleSource* WHITE_TERRACOTTA;
    static RuleSource* ORANGE_TERRACOTTA;
    static RuleSource* TERRACOTTA;
    static RuleSource* RED_SAND;
    static RuleSource* RED_SANDSTONE;
    static RuleSource* STONE;
    static RuleSource* DEEPSLATE;
    static RuleSource* DIRT;
    static RuleSource* PODZOL;
    static RuleSource* COARSE_DIRT;
    static RuleSource* MYCELIUM;
    static RuleSource* GRASS_BLOCK;
    static RuleSource* CALCITE;
    static RuleSource* GRAVEL;
    static RuleSource* SAND;
    static RuleSource* SANDSTONE;
    static RuleSource* PACKED_ICE;
    static RuleSource* SNOW_BLOCK;
    static RuleSource* MUD;
    static RuleSource* POWDER_SNOW;
    static RuleSource* ICE;
    static RuleSource* WATER;
    static RuleSource* LAVA;
    static RuleSource* NETHERRACK;
    static RuleSource* SOUL_SAND;
    static RuleSource* SOUL_SOIL;
    static RuleSource* BASALT;
    static RuleSource* BLACKSTONE;
    static RuleSource* WARPED_WART_BLOCK;
    static RuleSource* WARPED_NYLIUM;
    static RuleSource* NETHER_WART_BLOCK;
    static RuleSource* CRIMSON_NYLIUM;
    static RuleSource* ENDSTONE;

    static bool s_initialized;

    /**
     * Create a state rule for a block
     * Reference: SurfaceRuleData.java lines 48-50
     */
    static RuleSource* makeStateRule(const std::string& blockName);

    /**
     * Create noise condition for surface noise threshold
     * Reference: SurfaceRuleData.java lines 124-126
     */
    static ConditionSource* surfaceNoiseAbove(double threshold);

public:
    /**
     * Initialize static block rules
     * Must be called before using any surface rule data
     */
    static void initialize();

    /**
     * Get Overworld surface rules (standard configuration)
     * Reference: SurfaceRuleData.java lines 52-54
     */
    static RuleSource* overworld();

    /**
     * Get Overworld-like surface rules with configuration options
     * Reference: SurfaceRuleData.java lines 56-97
     *
     * @param doPreliminarySurfaceCheck - Whether to check preliminary surface level
     * @param bedrockRoof - Whether to add bedrock ceiling (for Nether-like)
     * @param bedrockFloor - Whether to add bedrock floor
     */
    static RuleSource* overworldLike(bool doPreliminarySurfaceCheck, bool bedrockRoof, bool bedrockFloor);

    /**
     * Get Nether surface rules
     * Reference: SurfaceRuleData.java lines 99-114
     */
    static RuleSource* nether();

    /**
     * Get End surface rules
     * Reference: SurfaceRuleData.java lines 116-118
     */
    static RuleSource* end();

    /**
     * Get air rule (for special cases)
     * Reference: SurfaceRuleData.java lines 120-122
     */
    static RuleSource* air();
};

} // namespace levelgen
} // namespace minecraft
