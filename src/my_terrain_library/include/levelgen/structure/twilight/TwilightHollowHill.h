#pragma once

#include "data/worldgen/features/TwilightSpikes.h"
#include "levelgen/structure/StructureStartData.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Twilight Forest 4.9 — the speleothem machinery of the cave landmarks
// (hollow hills now; hydra lair, yeti cave and troll cave share it):
//   world/components/structures/StructureSpeleothemConfig.java
//   world/components/speleothem/{SpeleothemVarietyConfig,Stalactite,
//       StalactiteReloadListener}.java
//   util/iterators/{RectangleLatticeIterator,ZippedIterator}.java
//
// The pieces themselves (HollowHillComponent) and the builder/terraformer
// declared in TwilightStructures.h live in TwilightHollowHill.cpp.

namespace minecraft {
namespace levelgen {
class WorldgenRandom;
namespace structure {
namespace twilight_pieces {

/**
 * RectangleLatticeIterator.TriangularLatticeConfig — two rectangular
 * lattices, the second offset by (xOffset, zOffset), approximating a
 * triangular (hexagonal-packing) grid.
 */
struct TriangularLatticeConfig {
    float spacing = 3.5f;
    float xOffset = 0.0f;
    float zOffset = 0.0f;
    float xSpacing = 0.0f;
    float zSpacing = 0.0f;

    /** TriangularLatticeConfig(spacing): offsets cos/sin(PI / 6) * spacing. */
    static TriangularLatticeConfig fromSpacing(float spacing);
    /** TriangularLatticeConfig(spacing, xOffset, zOffset): xSpacing = 2 xOffset. */
    static TriangularLatticeConfig fromOffsets(float spacing, float xOffset, float zOffset);

    /**
     * boundedGrid(bounds, yLevel) iterated to the end: the ZippedIterator of
     * the unshifted and the shifted RectangleLatticeIterator, in its exact
     * alternating order. Returns (x, z) pairs (y is always yLevel).
     */
    std::vector<std::pair<int32_t, int32_t>> boundedGrid(const BoundingBox& bounds) const;
};

/**
 * StructureSpeleothemConfig + its SpeleothemVarietyConfig (the
 * twilight/stalactites/<type>.json of the config's "type"), compiled once:
 * the stalactite list interpolated between base and ore stalactites by
 * ore_chance exactly as compileStalactites does (including the
 * string-length quantization factor), the stalagmite list as-is.
 */
class StructureSpeleothemConfig {
public:
    using Stalactite = data::worldgen::features::twilight::Stalactite;

    /**
     * The config registered as `id` ("twilightforest:small_hollow_hill" ->
     * data/twilightforest/twilight/structure_speleothem_settings/
     * small_hollow_hill.json). Cached, thread-safe. Throws
     * std::runtime_error when the settings file is missing (the mod's
     * registry lookup would fail the same way).
     */
    static std::shared_ptr<const StructureSpeleothemConfig> get(const std::string& id);

    const TriangularLatticeConfig& lattice() const { return m_lattice; }

    /** variety != null && rand.nextFloat() < stalactite_chance. */
    bool shouldDoAStalactite(WorldgenRandom& random) const;
    /** variety != null && rand.nextFloat() < stalagmite_chance. */
    bool shouldDoAStalagmite(WorldgenRandom& random) const;

    /** The compiled stalactite getter (one nextInt(total) draw, or none for the stone default). */
    const Stalactite& getStalactite(WorldgenRandom& random) const;
    /** The compiled stalagmite getter. */
    const Stalactite& getStalagmite(WorldgenRandom& random) const;
    /** getSpeleothem(hanging, rand). */
    const Stalactite& getSpeleothem(bool hanging, WorldgenRandom& random) const {
        return hanging ? getStalactite(random) : getStalagmite(random);
    }

    /** One compiled WeightedList<Stalactite> (empty = BlockSpikeFeature::defaultRandom). */
    struct WeightedStalactites {
        std::vector<std::pair<Stalactite, int32_t>> entries;
        int32_t totalWeight = 0;
        const Stalactite& pick(WorldgenRandom& random) const;
    };

private:
    TriangularLatticeConfig m_lattice;
    std::string m_type;
    bool m_hasVariety = false;
    float m_stalactiteChance = 0.0f;
    float m_stalagmiteChance = 0.0f;
    WeightedStalactites m_stalactites;
    WeightedStalactites m_stalagmites;

    friend struct SpeleothemConfigLoader;
};

} // namespace twilight_pieces
} // namespace structure
} // namespace levelgen
} // namespace minecraft
