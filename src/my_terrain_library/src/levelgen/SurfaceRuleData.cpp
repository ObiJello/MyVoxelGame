#include "levelgen/SurfaceRuleData.h"
#include "levelgen/SurfaceRules.h"
#include <limits>
#include <vector>

namespace minecraft {
namespace levelgen {

// Static member definitions
RuleSource* SurfaceRuleData::AIR = nullptr;
RuleSource* SurfaceRuleData::BEDROCK = nullptr;
RuleSource* SurfaceRuleData::WHITE_TERRACOTTA = nullptr;
RuleSource* SurfaceRuleData::ORANGE_TERRACOTTA = nullptr;
RuleSource* SurfaceRuleData::TERRACOTTA = nullptr;
RuleSource* SurfaceRuleData::RED_SAND = nullptr;
RuleSource* SurfaceRuleData::RED_SANDSTONE = nullptr;
RuleSource* SurfaceRuleData::STONE = nullptr;
RuleSource* SurfaceRuleData::DEEPSLATE = nullptr;
RuleSource* SurfaceRuleData::DIRT = nullptr;
RuleSource* SurfaceRuleData::PODZOL = nullptr;
RuleSource* SurfaceRuleData::COARSE_DIRT = nullptr;
RuleSource* SurfaceRuleData::MYCELIUM = nullptr;
RuleSource* SurfaceRuleData::GRASS_BLOCK = nullptr;
RuleSource* SurfaceRuleData::CALCITE = nullptr;
RuleSource* SurfaceRuleData::GRAVEL = nullptr;
RuleSource* SurfaceRuleData::SAND = nullptr;
RuleSource* SurfaceRuleData::SANDSTONE = nullptr;
RuleSource* SurfaceRuleData::PACKED_ICE = nullptr;
RuleSource* SurfaceRuleData::SNOW_BLOCK = nullptr;
RuleSource* SurfaceRuleData::MUD = nullptr;
RuleSource* SurfaceRuleData::POWDER_SNOW = nullptr;
RuleSource* SurfaceRuleData::ICE = nullptr;
RuleSource* SurfaceRuleData::WATER = nullptr;
RuleSource* SurfaceRuleData::LAVA = nullptr;
RuleSource* SurfaceRuleData::NETHERRACK = nullptr;
RuleSource* SurfaceRuleData::SOUL_SAND = nullptr;
RuleSource* SurfaceRuleData::SOUL_SOIL = nullptr;
RuleSource* SurfaceRuleData::BASALT = nullptr;
RuleSource* SurfaceRuleData::BLACKSTONE = nullptr;
RuleSource* SurfaceRuleData::WARPED_WART_BLOCK = nullptr;
RuleSource* SurfaceRuleData::WARPED_NYLIUM = nullptr;
RuleSource* SurfaceRuleData::NETHER_WART_BLOCK = nullptr;
RuleSource* SurfaceRuleData::CRIMSON_NYLIUM = nullptr;
RuleSource* SurfaceRuleData::ENDSTONE = nullptr;
bool SurfaceRuleData::s_initialized = false;

// Reference: SurfaceRuleData.java lines 48-50
RuleSource* SurfaceRuleData::makeStateRule(const std::string& blockName) {
    return SurfaceRules::state(blockName);
}

// Reference: SurfaceRuleData.java lines 124-126
ConditionSource* SurfaceRuleData::surfaceNoiseAbove(double threshold) {
    return SurfaceRules::noiseCondition("minecraft:surface", threshold / 8.25, std::numeric_limits<double>::max());
}

// Reference: SurfaceRuleData.java static block (lines 128-164)
void SurfaceRuleData::initialize() {
    if (s_initialized) return;
    s_initialized = true;

    // Initialize the SurfaceRules statics first
    SurfaceRules::initializeStatics();

    // Initialize block state rules
    AIR = makeStateRule("minecraft:air");
    BEDROCK = makeStateRule("minecraft:bedrock");
    WHITE_TERRACOTTA = makeStateRule("minecraft:white_terracotta");
    ORANGE_TERRACOTTA = makeStateRule("minecraft:orange_terracotta");
    TERRACOTTA = makeStateRule("minecraft:terracotta");
    RED_SAND = makeStateRule("minecraft:red_sand");
    RED_SANDSTONE = makeStateRule("minecraft:red_sandstone");
    STONE = makeStateRule("minecraft:stone");
    DEEPSLATE = makeStateRule("minecraft:deepslate");
    DIRT = makeStateRule("minecraft:dirt");
    PODZOL = makeStateRule("minecraft:podzol");
    COARSE_DIRT = makeStateRule("minecraft:coarse_dirt");
    MYCELIUM = makeStateRule("minecraft:mycelium");
    GRASS_BLOCK = makeStateRule("minecraft:grass_block");
    CALCITE = makeStateRule("minecraft:calcite");
    GRAVEL = makeStateRule("minecraft:gravel");
    SAND = makeStateRule("minecraft:sand");
    SANDSTONE = makeStateRule("minecraft:sandstone");
    PACKED_ICE = makeStateRule("minecraft:packed_ice");
    SNOW_BLOCK = makeStateRule("minecraft:snow_block");
    MUD = makeStateRule("minecraft:mud");
    POWDER_SNOW = makeStateRule("minecraft:powder_snow");
    ICE = makeStateRule("minecraft:ice");
    WATER = makeStateRule("minecraft:water");
    LAVA = makeStateRule("minecraft:lava");
    NETHERRACK = makeStateRule("minecraft:netherrack");
    SOUL_SAND = makeStateRule("minecraft:soul_sand");
    SOUL_SOIL = makeStateRule("minecraft:soul_soil");
    BASALT = makeStateRule("minecraft:basalt");
    BLACKSTONE = makeStateRule("minecraft:blackstone");
    WARPED_WART_BLOCK = makeStateRule("minecraft:warped_wart_block");
    WARPED_NYLIUM = makeStateRule("minecraft:warped_nylium");
    NETHER_WART_BLOCK = makeStateRule("minecraft:nether_wart_block");
    CRIMSON_NYLIUM = makeStateRule("minecraft:crimson_nylium");
    ENDSTONE = makeStateRule("minecraft:end_stone");
}

// Reference: SurfaceRuleData.java lines 52-54
RuleSource* SurfaceRuleData::overworld() {
    return overworldLike(true, false, true);
}

// Reference: SurfaceRuleData.java lines 56-97
RuleSource* SurfaceRuleData::overworldLike(bool doPreliminarySurfaceCheck, bool bedrockRoof, bool bedrockFloor) {
    initialize();

    // Reference: lines 57-69 - Y level conditions
    ConditionSource* woodedBadlandsTop = SurfaceRules::yBlockCheck(VerticalAnchor::absolute(97), 2);
    ConditionSource* badlandsTop = SurfaceRules::yBlockCheck(VerticalAnchor::absolute(256), 0);
    ConditionSource* badlandsHeightCondition = SurfaceRules::yStartCheck(VerticalAnchor::absolute(63), -1);
    ConditionSource* badlandsMid = SurfaceRules::yStartCheck(VerticalAnchor::absolute(74), 1);
    ConditionSource* mangroveSwampPuddleLevel = SurfaceRules::yBlockCheck(VerticalAnchor::absolute(60), 0);
    ConditionSource* swampPuddleLevel = SurfaceRules::yBlockCheck(VerticalAnchor::absolute(62), 0);
    ConditionSource* aboveOverworldSeaLevel = SurfaceRules::yBlockCheck(VerticalAnchor::absolute(63), 0);
    ConditionSource* notUnderwater = SurfaceRules::waterBlockCheck(-1, 0);
    ConditionSource* aboveWater = SurfaceRules::waterBlockCheck(0, 0);
    ConditionSource* notUnderDeepWater = SurfaceRules::waterStartCheck(-6, -1);
    ConditionSource* hole = SurfaceRules::hole();
    ConditionSource* frozenOcean = SurfaceRules::isBiome({"minecraft:frozen_ocean", "minecraft:deep_frozen_ocean"});
    ConditionSource* steep = SurfaceRules::steep();

    // Reference: line 70 - grass or dirt based on water
    RuleSource* grassOrDirtIfUnderwater = SurfaceRules::sequence({
        SurfaceRules::ifTrue(aboveWater, GRASS_BLOCK),
        DIRT
    });

    // Reference: line 71 - sand or sandstone based on ceiling
    RuleSource* sandOrSandstoneIfCeiling = SurfaceRules::sequence({
        SurfaceRules::ifTrue(SurfaceRules::ON_CEILING, SANDSTONE),
        SAND
    });

    // Reference: line 72 - gravel or stone based on ceiling
    RuleSource* gravelOrStoneIfCeiling = SurfaceRules::sequence({
        SurfaceRules::ifTrue(SurfaceRules::ON_CEILING, STONE),
        GRAVEL
    });

    // Reference: lines 73-74 - biome groups
    ConditionSource* biomesWithSandAndSandstone = SurfaceRules::isBiome({
        "minecraft:warm_ocean", "minecraft:beach", "minecraft:snowy_beach"
    });
    ConditionSource* biomesWithSandAndVeryDeepSandstone = SurfaceRules::isBiome({"minecraft:desert"});

    // Reference: line 75 - common surface and under rules
    RuleSource* commonSurfaceAndUnderRules = SurfaceRules::sequence({
        // Stony peaks - calcite or stone
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:stony_peaks"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(
                    SurfaceRules::noiseCondition("minecraft:calcite", -0.0125, 0.0125),
                    CALCITE
                ),
                STONE
            })
        ),
        // Stony shore - gravel or stone
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:stony_shore"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(
                    SurfaceRules::noiseCondition("minecraft:gravel", -0.05, 0.05),
                    gravelOrStoneIfCeiling
                ),
                STONE
            })
        ),
        // Windswept hills - stone on high noise
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:windswept_hills"}),
            SurfaceRules::ifTrue(surfaceNoiseAbove(1.0), STONE)
        ),
        // Sand biomes
        SurfaceRules::ifTrue(biomesWithSandAndSandstone, sandOrSandstoneIfCeiling),
        SurfaceRules::ifTrue(biomesWithSandAndVeryDeepSandstone, sandOrSandstoneIfCeiling),
        // Dripstone caves - stone
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:dripstone_caves"}),
            STONE
        )
    });

    // Reference: lines 76-77 - powder snow rules
    RuleSource* powderSnowUnderRule = SurfaceRules::ifTrue(
        SurfaceRules::noiseCondition("minecraft:powder_snow", 0.45, 0.58),
        SurfaceRules::ifTrue(aboveWater, POWDER_SNOW)
    );

    RuleSource* powderSnowSurfaceRule = SurfaceRules::ifTrue(
        SurfaceRules::noiseCondition("minecraft:powder_snow", 0.35, 0.6),
        SurfaceRules::ifTrue(aboveWater, POWDER_SNOW)
    );

    // Reference: line 78 - biome under surface rules
    RuleSource* biomeUnderSurfaceRule = SurfaceRules::sequence({
        // Frozen peaks
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:frozen_peaks"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(steep, PACKED_ICE),
                SurfaceRules::ifTrue(
                    SurfaceRules::noiseCondition("minecraft:packed_ice", -0.5, 0.2),
                    PACKED_ICE
                ),
                SurfaceRules::ifTrue(
                    SurfaceRules::noiseCondition("minecraft:ice", -0.0625, 0.025),
                    ICE
                ),
                SurfaceRules::ifTrue(aboveWater, SNOW_BLOCK)
            })
        ),
        // Snowy slopes
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:snowy_slopes"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(steep, STONE),
                powderSnowUnderRule,
                SurfaceRules::ifTrue(aboveWater, SNOW_BLOCK)
            })
        ),
        // Jagged peaks
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:jagged_peaks"}),
            STONE
        ),
        // Grove
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:grove"}),
            SurfaceRules::sequence({
                powderSnowUnderRule,
                DIRT
            })
        ),
        commonSurfaceAndUnderRules,
        // Windswept savanna
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:windswept_savanna"}),
            SurfaceRules::ifTrue(surfaceNoiseAbove(1.75), STONE)
        ),
        // Windswept gravelly hills
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:windswept_gravelly_hills"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(surfaceNoiseAbove(2.0), gravelOrStoneIfCeiling),
                SurfaceRules::ifTrue(surfaceNoiseAbove(1.0), STONE),
                SurfaceRules::ifTrue(surfaceNoiseAbove(-1.0), DIRT),
                gravelOrStoneIfCeiling
            })
        ),
        // Mangrove swamp
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:mangrove_swamp"}),
            MUD
        ),
        DIRT
    });

    // Reference: line 79 - biome surface rules
    RuleSource* biomeSurfaceRule = SurfaceRules::sequence({
        // Frozen peaks
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:frozen_peaks"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(steep, PACKED_ICE),
                SurfaceRules::ifTrue(
                    SurfaceRules::noiseCondition("minecraft:packed_ice", 0.0, 0.2),
                    PACKED_ICE
                ),
                SurfaceRules::ifTrue(
                    SurfaceRules::noiseCondition("minecraft:ice", 0.0, 0.025),
                    ICE
                ),
                SurfaceRules::ifTrue(aboveWater, SNOW_BLOCK)
            })
        ),
        // Snowy slopes
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:snowy_slopes"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(steep, STONE),
                powderSnowSurfaceRule,
                SurfaceRules::ifTrue(aboveWater, SNOW_BLOCK)
            })
        ),
        // Jagged peaks
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:jagged_peaks"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(steep, STONE),
                SurfaceRules::ifTrue(aboveWater, SNOW_BLOCK)
            })
        ),
        // Grove
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:grove"}),
            SurfaceRules::sequence({
                powderSnowSurfaceRule,
                SurfaceRules::ifTrue(aboveWater, SNOW_BLOCK)
            })
        ),
        commonSurfaceAndUnderRules,
        // Windswept savanna
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:windswept_savanna"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(surfaceNoiseAbove(1.75), STONE),
                SurfaceRules::ifTrue(surfaceNoiseAbove(-0.5), COARSE_DIRT)
            })
        ),
        // Windswept gravelly hills
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:windswept_gravelly_hills"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(surfaceNoiseAbove(2.0), gravelOrStoneIfCeiling),
                SurfaceRules::ifTrue(surfaceNoiseAbove(1.0), STONE),
                SurfaceRules::ifTrue(surfaceNoiseAbove(-1.0), grassOrDirtIfUnderwater),
                gravelOrStoneIfCeiling
            })
        ),
        // Old growth taiga
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:old_growth_pine_taiga", "minecraft:old_growth_spruce_taiga"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(surfaceNoiseAbove(1.75), COARSE_DIRT),
                SurfaceRules::ifTrue(surfaceNoiseAbove(-0.95), PODZOL)
            })
        ),
        // Ice spikes
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:ice_spikes"}),
            SurfaceRules::ifTrue(aboveWater, SNOW_BLOCK)
        ),
        // Mangrove swamp
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:mangrove_swamp"}),
            MUD
        ),
        // Mushroom fields
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:mushroom_fields"}),
            MYCELIUM
        ),
        grassOrDirtIfUnderwater
    });

    // Reference: lines 80-82 - clay band conditions for badlands
    ConditionSource* clayBand1 = SurfaceRules::noiseCondition("minecraft:surface", -0.909, -0.5454);
    ConditionSource* clayBand2 = SurfaceRules::noiseCondition("minecraft:surface", -0.1818, 0.1818);
    ConditionSource* clayBand3 = SurfaceRules::noiseCondition("minecraft:surface", 0.5454, 0.909);

    // Reference: line 83 - main rule close to surface (complex badlands + biome rules)
    RuleSource* mainRuleCloseToSurface = SurfaceRules::sequence({
        // ON_FLOOR rules
        SurfaceRules::ifTrue(SurfaceRules::ON_FLOOR, SurfaceRules::sequence({
            // Wooded badlands top
            SurfaceRules::ifTrue(
                SurfaceRules::isBiome({"minecraft:wooded_badlands"}),
                SurfaceRules::ifTrue(woodedBadlandsTop, SurfaceRules::sequence({
                    SurfaceRules::ifTrue(clayBand1, COARSE_DIRT),
                    SurfaceRules::ifTrue(clayBand2, COARSE_DIRT),
                    SurfaceRules::ifTrue(clayBand3, COARSE_DIRT),
                    grassOrDirtIfUnderwater
                }))
            ),
            // Swamp water puddles
            SurfaceRules::ifTrue(
                SurfaceRules::isBiome({"minecraft:swamp"}),
                SurfaceRules::ifTrue(swampPuddleLevel,
                    SurfaceRules::ifTrue(SurfaceRules::not_(aboveOverworldSeaLevel),
                        SurfaceRules::ifTrue(
                            SurfaceRules::noiseCondition("minecraft:surface_swamp", 0.0, std::numeric_limits<double>::max()),
                            WATER
                        )
                    )
                )
            ),
            // Mangrove swamp water puddles
            SurfaceRules::ifTrue(
                SurfaceRules::isBiome({"minecraft:mangrove_swamp"}),
                SurfaceRules::ifTrue(mangroveSwampPuddleLevel,
                    SurfaceRules::ifTrue(SurfaceRules::not_(aboveOverworldSeaLevel),
                        SurfaceRules::ifTrue(
                            SurfaceRules::noiseCondition("minecraft:surface_swamp", 0.0, std::numeric_limits<double>::max()),
                            WATER
                        )
                    )
                )
            )
        })),
        // Badlands biomes
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:badlands", "minecraft:eroded_badlands", "minecraft:wooded_badlands"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(SurfaceRules::ON_FLOOR, SurfaceRules::sequence({
                    SurfaceRules::ifTrue(badlandsTop, ORANGE_TERRACOTTA),
                    SurfaceRules::ifTrue(badlandsMid, SurfaceRules::sequence({
                        SurfaceRules::ifTrue(clayBand1, TERRACOTTA),
                        SurfaceRules::ifTrue(clayBand2, TERRACOTTA),
                        SurfaceRules::ifTrue(clayBand3, TERRACOTTA),
                        SurfaceRules::bandlands()
                    })),
                    SurfaceRules::ifTrue(notUnderwater, SurfaceRules::sequence({
                        SurfaceRules::ifTrue(SurfaceRules::ON_CEILING, RED_SANDSTONE),
                        RED_SAND
                    })),
                    SurfaceRules::ifTrue(SurfaceRules::not_(hole), ORANGE_TERRACOTTA),
                    SurfaceRules::ifTrue(notUnderDeepWater, WHITE_TERRACOTTA),
                    gravelOrStoneIfCeiling
                })),
                SurfaceRules::ifTrue(badlandsHeightCondition, SurfaceRules::sequence({
                    SurfaceRules::ifTrue(aboveOverworldSeaLevel,
                        SurfaceRules::ifTrue(SurfaceRules::not_(badlandsMid), ORANGE_TERRACOTTA)
                    ),
                    SurfaceRules::bandlands()
                })),
                SurfaceRules::ifTrue(SurfaceRules::UNDER_FLOOR,
                    SurfaceRules::ifTrue(notUnderDeepWater, WHITE_TERRACOTTA)
                )
            })
        ),
        // ON_FLOOR with water check
        SurfaceRules::ifTrue(SurfaceRules::ON_FLOOR,
            SurfaceRules::ifTrue(notUnderwater, SurfaceRules::sequence({
                SurfaceRules::ifTrue(frozenOcean,
                    SurfaceRules::ifTrue(hole, SurfaceRules::sequence({
                        SurfaceRules::ifTrue(aboveWater, AIR),
                        SurfaceRules::ifTrue(SurfaceRules::temperature(), ICE),
                        WATER
                    }))
                ),
                biomeSurfaceRule
            }))
        ),
        // Not under deep water rules
        SurfaceRules::ifTrue(notUnderDeepWater, SurfaceRules::sequence({
            SurfaceRules::ifTrue(SurfaceRules::ON_FLOOR,
                SurfaceRules::ifTrue(frozenOcean,
                    SurfaceRules::ifTrue(hole, WATER)
                )
            ),
            SurfaceRules::ifTrue(SurfaceRules::UNDER_FLOOR, biomeUnderSurfaceRule),
            SurfaceRules::ifTrue(biomesWithSandAndSandstone,
                SurfaceRules::ifTrue(SurfaceRules::DEEP_UNDER_FLOOR, SANDSTONE)
            ),
            SurfaceRules::ifTrue(biomesWithSandAndVeryDeepSandstone,
                SurfaceRules::ifTrue(SurfaceRules::VERY_DEEP_UNDER_FLOOR, SANDSTONE)
            )
        })),
        // ON_FLOOR fallback rules
        SurfaceRules::ifTrue(SurfaceRules::ON_FLOOR, SurfaceRules::sequence({
            SurfaceRules::ifTrue(
                SurfaceRules::isBiome({"minecraft:frozen_peaks", "minecraft:jagged_peaks"}),
                STONE
            ),
            SurfaceRules::ifTrue(
                SurfaceRules::isBiome({"minecraft:warm_ocean", "minecraft:lukewarm_ocean", "minecraft:deep_lukewarm_ocean"}),
                sandOrSandstoneIfCeiling
            ),
            gravelOrStoneIfCeiling
        }))
    });

    // Reference: lines 84-96 - build final rule sequence
    std::vector<RuleSource*> builder;

    // Reference: lines 85-87 - bedrock roof
    if (bedrockRoof) {
        builder.push_back(SurfaceRules::ifTrue(
            SurfaceRules::not_(SurfaceRules::verticalGradient(
                "bedrock_roof",
                VerticalAnchor::belowTop(5),
                VerticalAnchor::top()
            )),
            BEDROCK
        ));
    }

    // Reference: lines 89-91 - bedrock floor
    if (bedrockFloor) {
        builder.push_back(SurfaceRules::ifTrue(
            SurfaceRules::verticalGradient(
                "bedrock_floor",
                VerticalAnchor::bottom(),
                VerticalAnchor::aboveBottom(5)
            ),
            BEDROCK
        ));
    }

    // Reference: lines 93-94 - main surface rules
    RuleSource* ruleAbovePreliminarySurface = SurfaceRules::ifTrue(
        SurfaceRules::abovePreliminarySurface(),
        mainRuleCloseToSurface
    );
    builder.push_back(doPreliminarySurfaceCheck ? ruleAbovePreliminarySurface : mainRuleCloseToSurface);

    // Reference: line 95 - deepslate transition
    builder.push_back(SurfaceRules::ifTrue(
        SurfaceRules::verticalGradient(
            "deepslate",
            VerticalAnchor::absolute(0),
            VerticalAnchor::absolute(8)
        ),
        DEEPSLATE
    ));

    // Reference: line 96 - return sequence
    return SurfaceRules::sequence(builder);
}

// Reference: SurfaceRuleData.java lines 99-114
RuleSource* SurfaceRuleData::nether() {
    initialize();

    // Y level conditions
    ConditionSource* aboveNetherLavaLevel = SurfaceRules::yBlockCheck(VerticalAnchor::absolute(31), 0);
    ConditionSource* aboveNetherLavaSurface = SurfaceRules::yBlockCheck(VerticalAnchor::absolute(32), 0);
    ConditionSource* netherBandAroundLavaLevelBottom = SurfaceRules::yStartCheck(VerticalAnchor::absolute(30), 0);
    ConditionSource* netherBandAroundLavaLevelTop = SurfaceRules::not_(SurfaceRules::yStartCheck(VerticalAnchor::absolute(35), 0));
    ConditionSource* closeToCeiling = SurfaceRules::yBlockCheck(VerticalAnchor::belowTop(5), 0);
    ConditionSource* hole = SurfaceRules::hole();

    // Noise conditions
    ConditionSource* soulSandLayer = SurfaceRules::noiseCondition("minecraft:soul_sand_layer", -0.012, std::numeric_limits<double>::max());
    ConditionSource* gravelLayer = SurfaceRules::noiseCondition("minecraft:gravel_layer", -0.012, std::numeric_limits<double>::max());
    ConditionSource* patch = SurfaceRules::noiseCondition("minecraft:patch", -0.012, std::numeric_limits<double>::max());
    ConditionSource* netherrack = SurfaceRules::noiseCondition("minecraft:netherrack", 0.54, std::numeric_limits<double>::max());
    ConditionSource* netherWart = SurfaceRules::noiseCondition("minecraft:nether_wart", 1.17, std::numeric_limits<double>::max());
    ConditionSource* netherStateSelector = SurfaceRules::noiseCondition("minecraft:nether_state_selector", 0.0, std::numeric_limits<double>::max());

    // Gravel patch rule
    RuleSource* gravelPatch = SurfaceRules::ifTrue(patch,
        SurfaceRules::ifTrue(netherBandAroundLavaLevelBottom,
            SurfaceRules::ifTrue(netherBandAroundLavaLevelTop, GRAVEL)
        )
    );

    return SurfaceRules::sequence({
        // Bedrock floor
        SurfaceRules::ifTrue(
            SurfaceRules::verticalGradient("bedrock_floor", VerticalAnchor::bottom(), VerticalAnchor::aboveBottom(5)),
            BEDROCK
        ),
        // Bedrock roof
        SurfaceRules::ifTrue(
            SurfaceRules::not_(SurfaceRules::verticalGradient("bedrock_roof", VerticalAnchor::belowTop(5), VerticalAnchor::top())),
            BEDROCK
        ),
        // Close to ceiling
        SurfaceRules::ifTrue(closeToCeiling, NETHERRACK),
        // Basalt deltas
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:basalt_deltas"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(SurfaceRules::UNDER_CEILING, BASALT),
                SurfaceRules::ifTrue(SurfaceRules::UNDER_FLOOR, SurfaceRules::sequence({
                    gravelPatch,
                    SurfaceRules::ifTrue(netherStateSelector, BASALT),
                    BLACKSTONE
                }))
            })
        ),
        // Soul sand valley
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:soul_sand_valley"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(SurfaceRules::UNDER_CEILING, SurfaceRules::sequence({
                    SurfaceRules::ifTrue(netherStateSelector, SOUL_SAND),
                    SOUL_SOIL
                })),
                SurfaceRules::ifTrue(SurfaceRules::UNDER_FLOOR, SurfaceRules::sequence({
                    gravelPatch,
                    SurfaceRules::ifTrue(netherStateSelector, SOUL_SAND),
                    SOUL_SOIL
                }))
            })
        ),
        // ON_FLOOR rules
        SurfaceRules::ifTrue(SurfaceRules::ON_FLOOR, SurfaceRules::sequence({
            // Lava in holes
            SurfaceRules::ifTrue(
                SurfaceRules::not_(aboveNetherLavaSurface),
                SurfaceRules::ifTrue(hole, LAVA)
            ),
            // Warped forest
            SurfaceRules::ifTrue(
                SurfaceRules::isBiome({"minecraft:warped_forest"}),
                SurfaceRules::ifTrue(SurfaceRules::not_(netherrack),
                    SurfaceRules::ifTrue(aboveNetherLavaLevel, SurfaceRules::sequence({
                        SurfaceRules::ifTrue(netherWart, WARPED_WART_BLOCK),
                        WARPED_NYLIUM
                    }))
                )
            ),
            // Crimson forest
            SurfaceRules::ifTrue(
                SurfaceRules::isBiome({"minecraft:crimson_forest"}),
                SurfaceRules::ifTrue(SurfaceRules::not_(netherrack),
                    SurfaceRules::ifTrue(aboveNetherLavaLevel, SurfaceRules::sequence({
                        SurfaceRules::ifTrue(netherWart, NETHER_WART_BLOCK),
                        CRIMSON_NYLIUM
                    }))
                )
            )
        })),
        // Nether wastes
        SurfaceRules::ifTrue(
            SurfaceRules::isBiome({"minecraft:nether_wastes"}),
            SurfaceRules::sequence({
                SurfaceRules::ifTrue(SurfaceRules::UNDER_FLOOR,
                    SurfaceRules::ifTrue(soulSandLayer, SurfaceRules::sequence({
                        SurfaceRules::ifTrue(SurfaceRules::not_(hole),
                            SurfaceRules::ifTrue(netherBandAroundLavaLevelBottom,
                                SurfaceRules::ifTrue(netherBandAroundLavaLevelTop, SOUL_SAND)
                            )
                        ),
                        NETHERRACK
                    }))
                ),
                SurfaceRules::ifTrue(SurfaceRules::ON_FLOOR,
                    SurfaceRules::ifTrue(aboveNetherLavaLevel,
                        SurfaceRules::ifTrue(netherBandAroundLavaLevelTop,
                            SurfaceRules::ifTrue(gravelLayer, SurfaceRules::sequence({
                                SurfaceRules::ifTrue(aboveNetherLavaSurface, GRAVEL),
                                SurfaceRules::ifTrue(SurfaceRules::not_(hole), GRAVEL)
                            }))
                        )
                    )
                )
            })
        ),
        NETHERRACK
    });
}

// Reference: SurfaceRuleData.java lines 116-118
RuleSource* SurfaceRuleData::end() {
    initialize();
    return ENDSTONE;
}

// Reference: SurfaceRuleData.java lines 120-122
RuleSource* SurfaceRuleData::air() {
    initialize();
    return AIR;
}

} // namespace levelgen
} // namespace minecraft
