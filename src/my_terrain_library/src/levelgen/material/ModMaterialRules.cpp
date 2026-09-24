#include "levelgen/material/MaterialRules.h"

#include "world/level/block/Blocks.h"

#include <mutex>

// The engine's own dimensions' material rules, built in code and registered
// into the MATERIAL_RULE registry (vanilla dimensions read theirs from the
// datapack JSON). They are the 26.3 form of the surface rules the old engine
// built in SurfaceRuleData::hush/aether/twilight, rule for rule; the mods'
// pre-26.3 surface rules map 1:1 onto material rules (stone_depth, water,
// y_above, noise_threshold, vertical_gradient, biome, not, steep, block,
// sequence, condition keep their names and fields).

namespace minecraft {
namespace levelgen {
namespace material {

namespace {

using namespace MaterialRules;

// SurfaceRules.ON_FLOOR / UNDER_FLOOR / ON_CEILING (the vanilla
// material_condition entries on_floor / under_floor / on_ceiling).
MaterialConditionPtr onFloor() {
    return stoneDepthCheck(0, false, CaveSurface::FLOOR);
}
MaterialConditionPtr underFloor() {
    return stoneDepthCheck(0, true, CaveSurface::FLOOR);
}
MaterialConditionPtr onCeiling() {
    return stoneDepthCheck(0, false, CaveSurface::CEILING);
}

// The vanilla bedrock_floor rule: a 5-block gradient from the bottom.
MaterialRulePtr bedrockFloor() {
    return ifTrue(verticalGradient("minecraft:bedrock_floor", VerticalAnchor::bottom(), VerticalAnchor::aboveBottom(5)),
                  state("minecraft:bedrock"));
}

// The Hush - engine-only dimension (DimensionId::Hush); shape follows the
// overworld (bedrock floor, surface above the preliminary surface only):
//   ON_FLOOR:    resonant_barrens -> calcite noise in [-0.0125, 0.0125]
//                                    ? polished_hushstone : hushstone
//                sunken_choir      -> under water (more than one block below
//                                    the water line): surface noise >= 0
//                                    ? sculk : sculk_loam; steep -> hushstone;
//                                    sculk_loam
//                hollow_deep       -> steep -> hushstone; calcite-window
//                                    outcrops -> polished_hushstone;
//                                    sculk_loam
//                aurora_steppe     -> steep -> hushstone; surface noise in
//                                    [-0.15, 0.15] -> sculk_loam; hush_moss
//                steep             -> hushstone
//                otherwise         -> sculk_loam
//   UNDER_FLOOR: resonant_barrens, hollow_deep -> hushstone, else sculk_loam
// Everything deeper stays the generator's default block (hushstone).
MaterialRulePtr hush() {
    MaterialRulePtr hushstone = state("minecraft:hushstone");
    MaterialRulePtr polishedHushstone = state("minecraft:polished_hushstone");
    MaterialRulePtr sculkLoam = state("minecraft:sculk_loam");
    MaterialRulePtr hushMoss = state("minecraft:hush_moss");
    MaterialRulePtr sculk = state("minecraft:sculk");

    MaterialConditionPtr barrens = isBiome({"minecraft:resonant_barrens"});
    MaterialConditionPtr sunkenChoir = isBiome({"minecraft:sunken_choir"});
    MaterialConditionPtr hollowDeep = isBiome({"minecraft:hollow_deep"});
    MaterialConditionPtr auroraSteppe = isBiome({"minecraft:aurora_steppe"});
    MaterialConditionPtr isSteep = steep();
    // Same noise and window as the stony_peaks calcite outcrops.
    MaterialConditionPtr polishedOutcrop = noiseCondition2d("minecraft:calcite", -0.0125, 0.0125);
    // not(not_underwater): a real lake bed.
    MaterialConditionPtr underwater = not_(waterBlockCheck(-1, 0));
    // The swamp surface noise split at 0: half the bed is sculk.
    MaterialConditionPtr sculkBed = noiseCondition2d("minecraft:surface", 0.0);
    MaterialConditionPtr bareSoil = noiseCondition2d("minecraft:surface", -0.15, 0.15);

    MaterialRulePtr barrensFloor = sequence({ifTrue(polishedOutcrop, polishedHushstone), hushstone});
    MaterialRulePtr choirFloor = sequence({
        ifTrue(underwater, sequence({ifTrue(sculkBed, sculk), sculkLoam})),
        ifTrue(isSteep, hushstone),
        sculkLoam,
    });
    MaterialRulePtr deepFloor = sequence({
        ifTrue(isSteep, hushstone),
        ifTrue(polishedOutcrop, polishedHushstone),
        sculkLoam,
    });
    MaterialRulePtr steppeFloor = sequence({
        ifTrue(isSteep, hushstone),
        ifTrue(bareSoil, sculkLoam),
        hushMoss,
    });
    MaterialRulePtr floor = sequence({
        ifTrue(barrens, barrensFloor),
        ifTrue(sunkenChoir, choirFloor),
        ifTrue(hollowDeep, deepFloor),
        ifTrue(auroraSteppe, steppeFloor),
        ifTrue(isSteep, hushstone),
        sculkLoam,
    });
    MaterialRulePtr belowFloor = sequence({
        ifTrue(barrens, hushstone),
        ifTrue(hollowDeep, hushstone),
        sculkLoam,
    });
    MaterialRulePtr surface = sequence({
        ifTrue(onFloor(), floor),
        ifTrue(underFloor(), belowFloor),
    });

    return sequence({
        bedrockFloor(),
        ifTrue(abovePreliminarySurface(), surface),
    });
}

// The Aether - data/aether/worldgen/noise_settings/skylands.json
// "surface_rule" (AetherNoiseBuilders.aetherSurfaceRules()):
//   ON_FLOOR    -> not under water ? aether_grass_block : aether_dirt
//   UNDER_FLOOR -> aether_dirt
// No bedrock, no preliminary-surface gate. The engine registers the Aether
// blocks as minecraft:<slug>; the mod's double_drops / snowy properties are
// not carried.
MaterialRulePtr aether() {
    MaterialRulePtr grass = state("minecraft:aether_grass_block");
    MaterialRulePtr dirt = state("minecraft:aether_dirt");
    MaterialConditionPtr notUnderwater = waterBlockCheck(-1, 0);
    return sequence({
        ifTrue(onFloor(), sequence({ifTrue(notUnderwater, grass), dirt})),
        ifTrue(underFloor(), dirt),
    });
}

// The Twilight Forest - data/twilightforest/worldgen/noise_settings/
// twilight_noise_gen.json "surface_rule" (TFSurfaceRules):
//   bedrock_floor gradient bottom..aboveBottom(5)                -> bedrock
//   highlands: ON_FLOOR ->
//       surface noise >= 2.25/8.25                               -> coarse_dirt
//       surface noise >= -2.25/8.25: not under water -> podzol, else dirt
//   thornlands | final_plateau:
//       ON_FLOOR                                                 -> weathered_deadrock
//       waterStart(-6, -1) && UNDER_FLOOR                        -> cracked_deadrock
//       otherwise                                                -> deadrock
//   snowy_forest:
//       ON_FLOOR: not under water -> snow_block, else dirt
//       waterStart(-6, -1) && UNDER_FLOOR                        -> dirt
//   glacier:
//       ON_FLOOR -> gravel; waterStart(-6, -1) && UNDER_FLOOR -> gravel
//   default:
//       ON_FLOOR:
//         lake | stream: ON_CEILING -> sandstone; not under water -> grass; sand
//         swamp | fire_swamp: not under water -> grass; dirt
//         not under water && yStart(-4, 1)                       -> grass_block
//         !yStart(-4, 1) && under water                          -> dirt
//       waterStart(-6, -1) && yStart(-4, 1) && UNDER_FLOOR       -> dirt
// No preliminary-surface gate: the deadrock "otherwise" fills the whole
// thornlands / plateau column down to the bedrock band. The deadrock family is
// registered as minecraft:<slug>; the calcite / andesite / tuff stand-ins only
// apply to a block registry without it.
MaterialRulePtr twilight() {
    auto deadrock = [](const char* slug, const char* standIn) -> MaterialRulePtr {
        if (world::level::block::Blocks::getDefaultState(slug) != nullptr) return state(slug);
        return state(standIn);
    };
    MaterialRulePtr weatheredDeadrock = deadrock("minecraft:weathered_deadrock", "minecraft:calcite");
    MaterialRulePtr crackedDeadrock = deadrock("minecraft:cracked_deadrock", "minecraft:andesite");
    MaterialRulePtr deadrockBody = deadrock("minecraft:deadrock", "minecraft:tuff");
    MaterialRulePtr coarseDirt = state("minecraft:coarse_dirt");
    MaterialRulePtr podzol = state("minecraft:podzol");
    MaterialRulePtr dirt = state("minecraft:dirt");
    MaterialRulePtr snowBlock = state("minecraft:snow_block");
    MaterialRulePtr gravel = state("minecraft:gravel");
    MaterialRulePtr sandstone = state("minecraft:sandstone");
    MaterialRulePtr grassBlock = state("minecraft:grass_block");
    MaterialRulePtr sand = state("minecraft:sand");

    // water: offset -1, surface_depth_multiplier 0, add_stone_depth false.
    MaterialConditionPtr notUnderwater = waterBlockCheck(-1, 0);
    // water: offset -6, surface_depth_multiplier -1, add_stone_depth true.
    MaterialConditionPtr shallowWater = waterStartCheck(-6, -1);
    // y_above: absolute -4, surface_depth_multiplier 1, add_stone_depth true.
    MaterialConditionPtr aboveMinusFour = yStartCheck(VerticalAnchor::absolute(-4), 1);

    MaterialRulePtr highlands = ifTrue(
        isBiome({"twilightforest:highlands"}),
        ifTrue(onFloor(), sequence({
                              ifTrue(noiseCondition2d("minecraft:surface", 0.2727272727272727), coarseDirt),
                              ifTrue(noiseCondition2d("minecraft:surface", -0.2727272727272727),
                                     sequence({ifTrue(notUnderwater, podzol), dirt})),
                          })));

    MaterialRulePtr deadrockLands = ifTrue(
        isBiome({"twilightforest:thornlands", "twilightforest:final_plateau"}),
        sequence({
            ifTrue(onFloor(), weatheredDeadrock),
            ifTrue(shallowWater, ifTrue(underFloor(), crackedDeadrock)),
            deadrockBody,
        }));

    MaterialRulePtr snowyForest = ifTrue(
        isBiome({"twilightforest:snowy_forest"}),
        sequence({
            ifTrue(onFloor(), sequence({ifTrue(notUnderwater, snowBlock), dirt})),
            ifTrue(shallowWater, ifTrue(underFloor(), dirt)),
        }));

    MaterialRulePtr glacier = ifTrue(
        isBiome({"twilightforest:glacier"}),
        sequence({
            ifTrue(onFloor(), gravel),
            ifTrue(shallowWater, ifTrue(underFloor(), gravel)),
        }));

    MaterialRulePtr defaultFloor = sequence({
        ifTrue(isBiome({"twilightforest:lake", "twilightforest:stream"}),
               sequence({ifTrue(onCeiling(), sandstone), ifTrue(notUnderwater, grassBlock), sand})),
        ifTrue(isBiome({"twilightforest:swamp", "twilightforest:fire_swamp"}),
               sequence({ifTrue(notUnderwater, grassBlock), dirt})),
        ifTrue(notUnderwater, ifTrue(aboveMinusFour, grassBlock)),
        ifTrue(not_(aboveMinusFour), ifTrue(not_(notUnderwater), dirt)),
    });

    MaterialRulePtr defaultRules = sequence({
        ifTrue(onFloor(), defaultFloor),
        ifTrue(shallowWater, ifTrue(aboveMinusFour, ifTrue(underFloor(), dirt))),
    });

    return sequence({
        bedrockFloor(),
        highlands,
        deadrockLands,
        snowyForest,
        glacier,
        defaultRules,
    });
}

} // namespace

void registerModMaterialRules() {
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        MaterialRuleRegistry& registry = MaterialRuleRegistry::get();
        registry.registerRuleFactory("obeycraft:hush", hush);
        registry.registerRuleFactory("aether:aether", aether);
        registry.registerRuleFactory("twilight_forest:twilight_forest", twilight);
    });
}

} // namespace material
} // namespace levelgen
} // namespace minecraft
