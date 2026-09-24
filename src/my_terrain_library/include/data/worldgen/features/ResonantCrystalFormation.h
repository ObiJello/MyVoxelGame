#pragma once

#include "levelgen/feature/Feature.h"
#include <array>
#include <cstdint>
#include <vector>

// The Hush — engine-only dimension. No Java reference: a crystal outcrop
// feature in the spirit of vanilla's LargeDripstoneFeature (a few tapered
// columns from one anchor) and the amethyst geode's budding clusters.
// Defined in src/data/worldgen/features/ResonantCrystalFormation.cpp; the
// per-biome presets live in HushFeatures::bootstrap().

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

/**
 * The size of one kind of shard, drawn per shard:
 *   length  uniform float [minLength, maxLength] blocks along the shard's axis
 *   width   uniform int   [minWidth, maxWidth], 1..5 blocks across the base
 *           (1 = a single column, 2 = 2x2, 3 = 3x3, 4 = rounded 4x4,
 *           5 = rounded 5x5); every width tapers to a one-block point
 *   lean    uniform float [minLeanDegrees, maxLeanDegrees] from vertical
 */
struct CrystalShardSpec {
    float minLength = 1.0f;
    float maxLength = 1.0f;
    int32_t minWidth = 1;
    int32_t maxWidth = 1;
    float minLeanDegrees = 0.0f;
    float maxLeanDegrees = 0.0f;
};

/**
 * ResonantCrystalFormationConfiguration — one preset of the formation.
 *
 * A formation is a main shard (or, at landmarkChance, a landmark shard with
 * one extra satellite) at the origin plus minSatellites..maxSatellites
 * smaller shards around it, fanned out evenly with jitter, each leaning away
 * from the centre. Floor formations stand on a mound (a raised core disc of
 * moundCore and a dressed ring out to moundRadius); hanging formations grow
 * down from a ceiling and have no mound.
 */
class ResonantCrystalFormationConfiguration : public levelgen::FeatureConfiguration {
public:
    /** Grow down from a ceiling (stalactite) instead of up from the ground. */
    bool hanging = false;

    CrystalShardSpec main;
    float landmarkChance = 0.0f;
    CrystalShardSpec landmark;
    CrystalShardSpec satellite;
    int32_t minSatellites = 0;
    int32_t maxSatellites = 0;
    /** Satellite base distance from the origin, added to half the main shard's width. */
    int32_t minSatelliteSpread = 1;
    int32_t maxSatelliteSpread = 1;

    /** Floor only: raised core disc radius and dressed ring radius (0 = none). */
    int32_t moundCore = 0;
    int32_t moundRadius = 0;

    int32_t minGroundBuds = 0;     // clusters on the ground (ceiling) around the formation
    int32_t maxGroundBuds = 0;
    int32_t minSideBuds = 0;       // clusters budding on the shards' faces
    int32_t maxSideBuds = 0;
    int32_t minFallenShards = 0;   // floor only: 2-3 block shards lying on the ground
    int32_t maxFallenShards = 0;

    // ---- blocks (resolved by HushFeatures::bootstrap) ----
    BlockState* crystal = nullptr;
    /** resonant_cluster per FACING, indexed by the Direction ordinal (DOWN, UP, NORTH, SOUTH, WEST, EAST). */
    std::array<BlockState*, 6> clusters{};
    BlockState* moundAccent = nullptr;   // calcite
    BlockState* moundStone = nullptr;    // polished hushstone
    BlockState* moundRough = nullptr;    // hushstone
    BlockState* air = nullptr;
    /** Blocks the formation may stand on (hang from): the origin's anchor must be one of these. */
    std::vector<BlockState*> anchors;

    bool isComplete() const;
};

/**
 * ResonantCrystalFormationFeature — see the configuration above. The shape
 * is built in "grow space" (+y away from the anchor), so the floor and
 * ceiling variants share every step; world y = origin.y + y for a floor
 * formation and origin.y - y for a hanging one.
 *
 * Correctness rules:
 *   - the origin must be free and its anchor (the block under it, or over
 *     it when hanging) one of config.anchors; the four neighbouring columns
 *     must find footing within one block of the origin's level
 *   - crystal is only written into air or replaceable plants (hush grass),
 *     never water; the mound only re-dresses anchor blocks, and only where
 *     the cell above is free
 *   - low overhangs are filled down to their footing (<= 3 blocks), so no
 *     part of a shard floats over a gap at its base
 *   - every cluster has a solid block behind its facing
 *   - no crystal lands more than MAX_REACH blocks from the origin in x or z
 *     (clusters MAX_REACH + 1), inside the one-chunk feature write margin;
 *     every write also checks ensureCanWrite and the build height
 *   - all randomness comes from the placement's WorldgenRandom, and the
 *     trigonometry from Mth's sine table
 */
class ResonantCrystalFormationFeature
    : public levelgen::Feature<ResonantCrystalFormationConfiguration> {
public:
    static constexpr int32_t MAX_REACH = 13;

    bool place(levelgen::FeaturePlaceContext<ResonantCrystalFormationConfiguration>& context) override;
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
