#pragma once

#include "levelgen/feature/Feature.h"
#include "levelgen/feature/TreeFeature.h"
#include "world/level/block/Blocks.h"
#include <memory>
#include <string>
#include <vector>

// The Hush — engine-only dimension (DimensionId::Hush). There is no
// HushFeatures.java; this file follows the NetherFeatures/VegetationFeatures
// port patterns so the Hush configured features live in the same registry
// shape as vanilla's.

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

/**
 * HushFeatures - Registry of The Hush configured features
 *
 *   ORE_ECHO              echo ore blobs in hushstone (size 7)
 *   WHISPERWOOD           straight-trunk blob tree, whisperwood log +
 *                         lantern leaves (5 + [0,2] + [0,1] tall, radius 2)
 *   PATCH_RESONANCE_BLOOM random patch of resonance blooms on sculk loam,
 *                         hush moss, grass block or dirt
 *   HUSH_MOSS_PATCH       vegetation patch turning #hush_moss_replaceable
 *                         floors into hush moss with scattered blooms
 *
 *   -- Crystal Caverns / surface expansion (each resolves its own blocks;
 *      a missing block logs and leaves that feature null) --
 *   ORE_RESONITE          resonite ore blobs in hushstone (size 6)
 *   RESONANT_CLUSTERS     simple_random_selector over six resonant_cluster
 *                         placements, one per facing: floor (scan down),
 *                         ceiling (scan up) and the four walls
 *   RESONANT_CRYSTAL_CLUMP tiny random patch (2-3 blocks) of resonant
 *                         crystal on cavern floors
 *   RESONANT_CLUSTER_SURFACE small floor-only patch of up-facing clusters
 *                         (resonant barrens surface)
 *   PATCH_HUSH_GRASS      random patch of hush grass on sculk loam / hush moss
 *   WHISPERWOOD_LARGE     taller whisperwood (8 + [0,3] + [0,1], radius 3,
 *                         foliage height 4)
 *   WHISPERWOOD_FOREST_TREES random_selector: WHISPERWOOD_LARGE 1 : 3 WHISPERWOOD
 *
 *   -- Sunken Choir / Hollow Deep / Aurora Steppe --
 *   SUNKEN_RUIN           a broken hushstone-brick fragment on a flooded
 *                         floor: a pillar, a wall stub or an arch, with
 *                         fallen blocks around it (Hush-only feature class)
 *   KELP                  vanilla AquaticFeatures.KELP (the kelp column)
 *   SEA_PICKLE            vanilla AquaticFeatures.SEA_PICKLE (one pickle)
 *   ROPE_BRIDGE           3-wide whisperwood suspension bridge across a
 *                         chasm found from a rim: catenary deck in half-
 *                         steps, log towers with lanterns on brick footings,
 *                         fence hand-ropes, chain hangers, cross-ties
 *                         (Hush-only feature class)
 *   RESONANT_STALACTITE   hanging ResonantCrystalFormationFeature: a 4-8
 *                         long tapered 2x2 shard and 1-3 leaning satellites
 *                         growing down from a ceiling, cluster tips, side
 *                         buds, clusters on the ceiling around it
 *   HUSHSTONE_BOULDER     vanilla ForestRockFeature in polished hushstone
 *
 *   -- Resonant crystal formations (ResonantCrystalFormationFeature: tapered
 *      shards fanned out from one anchor on a calcite / polished-hushstone
 *      mound, cluster tips, budding clusters, fallen shards) --
 *   CRYSTAL_FORMATION_SMALL  4-7 long main shard + 2-3 thin satellites
 *                         (meadows accent, barrens scatter)
 *   CRYSTAL_FORMATION_FIELD  6-10 long 2x2/3x3 main (20 % an 11-15 long
 *                         landmark) + 3-6 satellites (barrens)
 *   CRYSTAL_FORMATION_TALL   10-15 long 3x3/4x4 main (35 % a 15-19 long
 *                         rounded 5x5) + 3-5 satellites (steppe, deep rims)
 */
class HushFeatures {
private:
    // Feature instances (the vanilla feature classes, reused)
    static levelgen::OreFeature s_oreFeature;
    static std::shared_ptr<levelgen::feature::TreeFeature> s_treeFeature;
    static levelgen::RandomPatchFeature s_randomPatchFeature;
    static levelgen::SimpleBlockFeature s_simpleBlockFeature;
    static levelgen::BlockColumnFeature s_blockColumnFeature;
    static levelgen::VegetationPatchFeature s_vegetationPatchFeature;
    static levelgen::SimpleRandomSelectorFeature s_simpleRandomSelectorFeature;
    static levelgen::RandomSelectorFeature s_randomSelectorFeature;

    static bool s_initialized;

public:
    static levelgen::ConfiguredFeature* ORE_ECHO;
    static levelgen::ConfiguredFeature* WHISPERWOOD;
    static levelgen::ConfiguredFeature* PATCH_RESONANCE_BLOOM;
    static levelgen::ConfiguredFeature* HUSH_MOSS_VEGETATION;   // inner feature of HUSH_MOSS_PATCH
    static levelgen::ConfiguredFeature* HUSH_MOSS_PATCH;

    // Crystal Caverns / surface expansion
    static levelgen::ConfiguredFeature* ORE_RESONITE;
    static levelgen::ConfiguredFeature* RESONANT_CLUSTERS;
    static levelgen::ConfiguredFeature* RESONANT_CRYSTAL_CLUMP;
    static levelgen::ConfiguredFeature* RESONANT_CLUSTER_SURFACE;
    static levelgen::ConfiguredFeature* PATCH_HUSH_GRASS;
    static levelgen::ConfiguredFeature* WHISPERWOOD_LARGE;
    static levelgen::ConfiguredFeature* WHISPERWOOD_FOREST_TREES;

    // Sunken Choir / Hollow Deep / Aurora Steppe
    static levelgen::ConfiguredFeature* SUNKEN_RUIN;
    static levelgen::ConfiguredFeature* KELP;
    static levelgen::ConfiguredFeature* SEA_PICKLE;
    static levelgen::ConfiguredFeature* ROPE_BRIDGE;
    static levelgen::ConfiguredFeature* RESONANT_STALACTITE;
    static levelgen::ConfiguredFeature* HUSHSTONE_BOULDER;

    // Resonant crystal formations
    static levelgen::ConfiguredFeature* CRYSTAL_FORMATION_SMALL;
    static levelgen::ConfiguredFeature* CRYSTAL_FORMATION_FIELD;
    static levelgen::ConfiguredFeature* CRYSTAL_FORMATION_TALL;

    /**
     * Bootstrap/initialize all Hush configured features. A missing core
     * Hush block leaves every feature null (logged, not thrown); a missing
     * expansion block (resonite ore, resonant cluster, hush grass) leaves
     * only the features that use it null.
     */
    static void bootstrap();

    static bool isInitialized() { return s_initialized; }
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
