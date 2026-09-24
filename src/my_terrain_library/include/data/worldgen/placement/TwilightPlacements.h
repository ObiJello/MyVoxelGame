#pragma once

#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifier.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "levelgen/placement/PlacementContext.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/Heightmap.h"
#include "levelgen/WorldgenRandom.h"
#include "random/LegacyRandomSource.h"
#include "random/XoroshiroRandomSource.h"
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// Twilight Forest 4.9 — placed features, built at bootstrap straight from
// data/twilightforest/worldgen/placed_feature/**.json (copied from the mod's
// generated data; TFPlacedFeatures.java generates them). Each JSON's
// "feature" is looked up in the TF configured-feature registry
// (data/worldgen/features/TwilightFeatureRegistry.h) and its "placement"
// list becomes the modifier chain, modifier for modifier. The three TF
// placement modifier types are ported here; the vanilla ones map onto the
// library's PlacementModifiers.

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace levelgen;
using namespace levelgen::placement;

/**
 * AvoidLandmarkModifier — world/components/placements/AvoidLandmarkModifier
 * ("twilightforest:no_structure"). Drops the position when a structure
 * referenced by its chunk is a DecorationClearance whose clearance forbids
 * this feature's occupancy (surface / underground / vegetation) and the
 * position lies inside that start's clearance square (chunk_clearance_radius
 * * 16 + additional_clearance around the start's centre), or — for a
 * clearance radius <= 0 — within additional_clearance of any of its pieces
 * (UtilityPiece.allowFeatures pieces excepted). Draws no randomness.
 */
class NoStructurePlacement : public PlacementModifier {
public:
    NoStructurePlacement(bool occupiesSurface, bool occupiesUnderground, bool occupiesVegetation,
                         int32_t additionalClearance, std::unordered_set<std::string> structuresAllowed)
        : m_occupiesSurface(occupiesSurface), m_occupiesUnderground(occupiesUnderground),
          m_occupiesVegetation(occupiesVegetation), m_additionalClearance(additionalClearance),
          m_structuresAllowed(std::move(structuresAllowed)) {}

    void appendPositions(PlacementContext& context, WorldgenRandom& random, const core::BlockPos& origin,
                         std::vector<core::BlockPos>& out) override;

    std::string getTypeName() const override { return "NoStructurePlacement"; }

private:
    bool structureBlocksPlacement(PlacementContext& context, const core::BlockPos& origin,
                                  const std::string& structureName,
                                  const std::vector<int64_t>& startChunks) const;

    bool m_occupiesSurface;
    bool m_occupiesUnderground;
    bool m_occupiesVegetation;
    int32_t m_additionalClearance;
    std::unordered_set<std::string> m_structuresAllowed;
};

/**
 * ChunkCenterModifier ("twilightforest:chunk_centerer"): the chunk's centre
 * column ((x & ~15) + 8, y, (z & ~15) + 8).
 */
class ChunkCenterPlacement : public PlacementModifier {
public:
    void appendPositions(PlacementContext& context, WorldgenRandom& random, const core::BlockPos& origin,
                         std::vector<core::BlockPos>& out) override;
    std::string getTypeName() const override { return "ChunkCenterPlacement"; }
};

/**
 * ChunkBlanketingModifier ("twilightforest:chunk_blanketing"): every column
 * of the chunk (z-major), each kept when nextFloat() <= integrity, at the
 * heightmap's first free block, filtered by the optional biome lock.
 */
class ChunkBlanketingPlacement : public PlacementModifier {
public:
    ChunkBlanketingPlacement(float integrity, Heightmap::Types heightmap, std::unordered_set<std::string> biomeLock)
        : m_integrity(integrity), m_heightmap(heightmap), m_biomeLock(std::move(biomeLock)) {}

    void appendPositions(PlacementContext& context, WorldgenRandom& random, const core::BlockPos& origin,
                         std::vector<core::BlockPos>& out) override;
    std::string getTypeName() const override { return "ChunkBlanketingPlacement"; }

private:
    float m_integrity;
    Heightmap::Types m_heightmap;
    std::unordered_set<std::string> m_biomeLock;   // empty = no lock
};

/**
 * TrapezoidInt — net/minecraft/util/valueproviders/TrapezoidInt.java (26.1).
 * The symmetric no-plateau case is nextInt(max + 1) - nextInt(max + 1), the
 * old random_patch spread.
 */
class TrapezoidInt : public carver::IntProvider {
public:
    TrapezoidInt(int32_t minInclusive, int32_t maxInclusive, int32_t plateau)
        : m_min(minInclusive), m_max(maxInclusive), m_plateau(plateau) {}

    int32_t sample(WorldgenRandom& random) const override { return sampleImpl(random); }
    int32_t sample(LegacyRandomSource& random) const override { return sampleImpl(random); }
    int32_t sample(XoroshiroRandomSource& random) const override { return sampleImpl(random); }
    int32_t getMinValue() const override { return m_min; }
    int32_t getMaxValue() const override { return m_max; }

private:
    template<typename R>
    int32_t sampleImpl(R& random) const {
        if (m_plateau == 0 && m_max == -m_min) {
            const int32_t a = random.nextInt(m_max + 1);
            const int32_t b = random.nextInt(m_max + 1);
            return a - b;
        }
        const int32_t range = m_max - m_min;
        if (m_plateau == range) {
            return random.nextInt(m_max - m_min + 1) + m_min;   // Mth.randomBetweenInclusive
        }
        const int32_t plateauStart = (range - m_plateau) / 2;
        const int32_t plateauEnd = range - plateauStart;
        const int32_t a = random.nextInt(plateauEnd + 1);
        const int32_t b = random.nextInt(plateauStart + 1);
        return m_min + a + b;
    }

    int32_t m_min;
    int32_t m_max;
    int32_t m_plateau;
};

/**
 * TwilightPlacements — the TF placed-feature registry. bootstrap() runs the
 * TF configured-feature bootstraps (TwilightFeatures, TwilightTreeFeatures,
 * TwilightDecorFeatures) and then loads every placed_feature JSON. A JSON
 * whose configured feature is not registered (a missing block) or whose
 * modifiers cannot be built is skipped with one warning; get() then returns
 * null and BiomeFeatureRegistry::addFeature skips it.
 */
class TwilightPlacements {
public:
    static void bootstrap();
    static bool isInitialized();

    /** The placed feature "twilightforest:<path>", or null. */
    static PlacedFeature* get(const std::string& id);
};

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft
