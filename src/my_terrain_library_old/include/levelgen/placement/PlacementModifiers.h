#pragma once

#include "levelgen/placement/PlacementModifier.h"
#include "levelgen/placement/PlacementContext.h"
#include "levelgen/Heightmap.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/ChunkGenerator.h"
#include "world/biome/Biome.h"
#include "synth/PerlinSimplexNoise.h"
#include <cmath>
#include <limits>

// Reference: Various placement modifier files in net/minecraft/world/level/levelgen/placement/

namespace minecraft {
namespace levelgen {
namespace placement {

//=============================================================================
// CaveSurface - Enum for cave surface placement direction
// Reference: CaveSurface.java
//=============================================================================

enum class CaveSurface {
    CEILING,  // Search upward, place on ceiling
    FLOOR     // Search downward, place on floor
};

/**
 * Get the search direction for a cave surface
 * Reference: CaveSurface.java getDirection()
 */
inline int getCaveSurfaceDirection(CaveSurface surface) {
    // CEILING searches UP (direction = UP = 1)
    // FLOOR searches DOWN (direction = DOWN = -1)
    return (surface == CaveSurface::CEILING) ? 1 : -1;
}

/**
 * Get the Y offset for this cave surface type
 * Reference: CaveSurface.java getY()
 */
inline int getCaveSurfaceY(CaveSurface surface) {
    // CEILING: y = 1 (UP)
    // FLOOR: y = -1 (DOWN)
    return (surface == CaveSurface::CEILING) ? 1 : -1;
}

//=============================================================================
// InSquarePlacement - Random XZ within 16x16
// Reference: InSquarePlacement.java
//=============================================================================

class InSquarePlacement : public PlacementModifier {
public:
    /**
     * Get singleton instance
     * Reference: InSquarePlacement.java lines 12-14
     */
    static InSquarePlacement& spread() {
        static InSquarePlacement instance;
        return instance;
    }

    /**
     * Get positions - random XZ offset within chunk
     * Reference: InSquarePlacement.java lines 16-20
     */
    std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) override {
        int32_t x = random.nextInt(16) + origin.getX();
        int32_t z = random.nextInt(16) + origin.getZ();
        return {core::BlockPos(x, origin.getY(), z)};
    }

private:
    InSquarePlacement() = default;
};

//=============================================================================
// CountPlacement - Repeat placement N times
// Reference: CountPlacement.java
//=============================================================================

class CountPlacement : public RepeatingPlacement {
private:
    const carver::IntProvider* m_count;

public:
    /**
     * Create with IntProvider
     * Reference: CountPlacement.java lines 17-19
     */
    static CountPlacement of(const carver::IntProvider* count) {
        return CountPlacement(count);
    }

    /**
     * Create with constant count
     * Reference: CountPlacement.java lines 21-23
     */
    static CountPlacement of(int32_t count) {
        static std::vector<carver::ConstantInt> constants;
        constants.push_back(carver::ConstantInt(count));
        return CountPlacement(&constants.back());
    }

protected:
    /**
     * Get count for this placement
     * Reference: CountPlacement.java lines 25-27
     */
    int32_t count(XoroshiroRandomSource& random, const core::BlockPos& origin) override {
        return m_count->sample(random);
    }

private:
    explicit CountPlacement(const carver::IntProvider* count) : m_count(count) {}
};

//=============================================================================
// HeightmapPlacement - Place at heightmap surface
// Reference: HeightmapPlacement.java
//=============================================================================

class HeightmapPlacement : public PlacementModifier {
private:
    Heightmap::Types m_heightmap;

public:
    /**
     * Create for specific heightmap type
     * Reference: HeightmapPlacement.java lines 18-20
     */
    static HeightmapPlacement onHeightmap(Heightmap::Types heightmap) {
        return HeightmapPlacement(heightmap);
    }

    /**
     * Get positions - set Y to heightmap value
     * Reference: HeightmapPlacement.java lines 22-27
     */
    std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) override {
        int32_t x = origin.getX();
        int32_t z = origin.getZ();
        int32_t height = context.getHeight(m_heightmap, x, z);
        if (height > context.getMinY()) {
            return {core::BlockPos(x, height, z)};
        }
        return {};
    }

private:
    explicit HeightmapPlacement(Heightmap::Types heightmap) : m_heightmap(heightmap) {}
};

//=============================================================================
// HeightRangePlacement - Place at random Y within range
// Reference: HeightRangePlacement.java
//=============================================================================

class HeightRangePlacement : public PlacementModifier {
private:
    const carver::HeightProvider* m_height;

public:
    /**
     * Create with HeightProvider
     * Reference: HeightRangePlacement.java lines 21-23
     */
    static HeightRangePlacement of(const carver::HeightProvider* height) {
        return HeightRangePlacement(height);
    }

    /**
     * Create uniform height range
     * Reference: HeightRangePlacement.java lines 25-27
     */
    static HeightRangePlacement uniform(const VerticalAnchor& minInclusive, const VerticalAnchor& maxInclusive) {
        static std::vector<carver::UniformHeight> heights;
        heights.push_back(carver::UniformHeight(minInclusive, maxInclusive));
        return HeightRangePlacement(&heights.back());
    }

    /**
     * Create triangle height range (trapezoid with plateau=0)
     * Reference: HeightRangePlacement.java lines 29-31
     */
    static HeightRangePlacement triangle(const VerticalAnchor& minInclusive, const VerticalAnchor& maxInclusive) {
        static std::vector<carver::TrapezoidHeight> heights;
        heights.push_back(carver::TrapezoidHeight(minInclusive, maxInclusive, 0));
        return HeightRangePlacement(&heights.back());
    }

    /**
     * Get positions - set Y from height provider
     * Reference: HeightRangePlacement.java lines 33-35
     */
    std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) override {
        int32_t y = m_height->sample(random, context);
        return {core::BlockPos(origin.getX(), y, origin.getZ())};
    }

private:
    explicit HeightRangePlacement(const carver::HeightProvider* height) : m_height(height) {}
};

//=============================================================================
// RarityFilter - 1/chance probability filter
// Reference: RarityFilter.java
//=============================================================================

class RarityFilter : public PlacementFilter {
private:
    int32_t m_chance;

public:
    /**
     * Create rarity filter
     * Reference: RarityFilter.java lines 16-18
     */
    static RarityFilter onAverageOnceEvery(int32_t chance) {
        return RarityFilter(chance);
    }

protected:
    /**
     * Check if placement should occur
     * Reference: RarityFilter.java lines 20-22
     */
    bool shouldPlace(
        PlacementContext& context,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) override {
        return random.nextFloat() < 1.0f / static_cast<float>(m_chance);
    }

private:
    explicit RarityFilter(int32_t chance) : m_chance(chance) {}
};

//=============================================================================
// BiomeFilter - Filter by biome feature list
// Reference: BiomeFilter.java
//=============================================================================

class BiomeFilter : public PlacementFilter {
public:
    /**
     * Get singleton instance
     * Reference: BiomeFilter.java lines 16-18
     */
    static BiomeFilter& biome() {
        static BiomeFilter instance;
        return instance;
    }

protected:
    /**
     * Check if biome has this feature
     * Reference: BiomeFilter.java lines 20-24
     *
     * Algorithm:
     * 1. Get the top feature being placed from context
     * 2. Get the biome at the origin position
     * 3. Check if the biome's generation settings contain this feature
     */
    bool shouldPlace(
        PlacementContext& context,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) override {
        // Reference: BiomeFilter.java line 21
        // Get the top feature being placed
        auto topFeature = context.topFeature();
        if (!topFeature.has_value()) {
            // No feature context - this is an error case in Java
            // Reference: BiomeFilter.java line 21 throws IllegalStateException
            // For safety, allow placement
            return true;
        }

        // Reference: BiomeFilter.java line 22
        // Get biome at position
        ChunkGenerator* generator = context.generator();
        if (!generator) {
            // No generator available - allow placement
            return true;
        }

        // Get biome at origin position
        void* biomePtr = context.getBiome(origin);
        if (!biomePtr) {
            // No biome available - allow placement
            return true;
        }

        // Cast to BiomeHolder
        world::biome::BiomeHolder biome = static_cast<world::biome::BiomeHolder>(biomePtr);

        // Reference: BiomeFilter.java line 23
        // Check if the biome's generation settings contain this feature
        const world::biome::BiomeGenerationSettings& settings = generator->getBiomeGenerationSettings(biome);
        return settings.hasFeature(*topFeature);
    }

private:
    BiomeFilter() = default;
};

//=============================================================================
// RandomOffsetPlacement - Random XZ and Y offset
// Reference: RandomOffsetPlacement.java
//=============================================================================

class RandomOffsetPlacement : public PlacementModifier {
private:
    const carver::IntProvider* m_xzSpread;
    const carver::IntProvider* m_ySpread;

public:
    /**
     * Create with XZ and Y spread
     * Reference: RandomOffsetPlacement.java lines 16-18
     */
    static RandomOffsetPlacement of(const carver::IntProvider* xzSpread, const carver::IntProvider* ySpread) {
        return RandomOffsetPlacement(xzSpread, ySpread);
    }

    /**
     * Create vertical-only offset
     * Reference: RandomOffsetPlacement.java lines 20-22
     */
    static RandomOffsetPlacement vertical(const carver::IntProvider* ySpread) {
        static carver::ConstantInt zero(0);
        return RandomOffsetPlacement(&zero, ySpread);
    }

    /**
     * Create horizontal-only offset
     * Reference: RandomOffsetPlacement.java lines 24-26
     */
    static RandomOffsetPlacement horizontal(const carver::IntProvider* xzSpread) {
        static carver::ConstantInt zero(0);
        return RandomOffsetPlacement(xzSpread, &zero);
    }

    /**
     * Get positions with random offset
     * Reference: RandomOffsetPlacement.java lines 33-38
     */
    std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) override {
        int32_t scatterX = origin.getX() + m_xzSpread->sample(random);
        int32_t scatterY = origin.getY() + m_ySpread->sample(random);
        int32_t scatterZ = origin.getZ() + m_xzSpread->sample(random);
        return {core::BlockPos(scatterX, scatterY, scatterZ)};
    }

private:
    RandomOffsetPlacement(const carver::IntProvider* xzSpread, const carver::IntProvider* ySpread)
        : m_xzSpread(xzSpread), m_ySpread(ySpread) {}
};

//=============================================================================
// SurfaceWaterDepthFilter - Filter by water depth above surface
// Reference: SurfaceWaterDepthFilter.java
//=============================================================================

class SurfaceWaterDepthFilter : public PlacementFilter {
private:
    int32_t m_maxWaterDepth;

public:
    /**
     * Create filter with max water depth
     * Reference: SurfaceWaterDepthFilter.java forMaxDepth
     */
    static SurfaceWaterDepthFilter forMaxDepth(int32_t maxDepth) {
        return SurfaceWaterDepthFilter(maxDepth);
    }

protected:
    /**
     * Check water depth at position
     * Reference: SurfaceWaterDepthFilter.java shouldPlace
     */
    bool shouldPlace(
        PlacementContext& context,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) override {
        int32_t surfaceY = context.getHeight(Heightmap::Types::OCEAN_FLOOR, origin.getX(), origin.getZ());
        int32_t waterY = context.getHeight(Heightmap::Types::WORLD_SURFACE, origin.getX(), origin.getZ());
        return waterY - surfaceY <= m_maxWaterDepth;
    }

private:
    explicit SurfaceWaterDepthFilter(int32_t maxWaterDepth) : m_maxWaterDepth(maxWaterDepth) {}
};

//=============================================================================
// NoiseBasedCountPlacement - Count based on noise value
// Reference: NoiseBasedCountPlacement.java
//=============================================================================

class NoiseBasedCountPlacement : public RepeatingPlacement {
private:
    int32_t m_noiseToCountRatio;
    double m_noiseFactor;
    double m_noiseOffset;

public:
    /**
     * Create noise-based count placement
     * Reference: NoiseBasedCountPlacement.java of()
     */
    static NoiseBasedCountPlacement of(int32_t noiseToCountRatio, double noiseFactor, double noiseOffset) {
        return NoiseBasedCountPlacement(noiseToCountRatio, noiseFactor, noiseOffset);
    }

protected:
    /**
     * Get count based on biome noise
     * Reference: NoiseBasedCountPlacement.java count() lines 26-28
     * Algorithm: ceil((noise + offset) * ratio)
     *
     * Uses Biome.BIOME_INFO_NOISE at (x/noiseFactor, z/noiseFactor)
     */
    int32_t count(XoroshiroRandomSource& random, const core::BlockPos& origin) override {
        double flowerNoise = BiomeInfoNoise::getValue(
            static_cast<double>(origin.getX()) / m_noiseFactor,
            static_cast<double>(origin.getZ()) / m_noiseFactor
        );
        return static_cast<int32_t>(std::ceil((flowerNoise + m_noiseOffset) * static_cast<double>(m_noiseToCountRatio)));
    }

private:
    NoiseBasedCountPlacement(int32_t noiseToCountRatio, double noiseFactor, double noiseOffset)
        : m_noiseToCountRatio(noiseToCountRatio)
        , m_noiseFactor(noiseFactor)
        , m_noiseOffset(noiseOffset) {}
};

//=============================================================================
// NoiseThresholdCountPlacement - Count based on noise threshold
// Reference: NoiseThresholdCountPlacement.java
//=============================================================================

class NoiseThresholdCountPlacement : public RepeatingPlacement {
private:
    double m_noiseLevel;
    int32_t m_belowNoise;
    int32_t m_aboveNoise;

public:
    /**
     * Create noise threshold count placement
     * Reference: NoiseThresholdCountPlacement.java of()
     */
    static NoiseThresholdCountPlacement of(double noiseLevel, int32_t belowNoise, int32_t aboveNoise) {
        return NoiseThresholdCountPlacement(noiseLevel, belowNoise, aboveNoise);
    }

protected:
    /**
     * Get count based on biome noise threshold
     * Reference: NoiseThresholdCountPlacement.java count() lines 26-28
     * Algorithm: if noise < threshold -> belowNoise else aboveNoise
     *
     * Uses Biome.BIOME_INFO_NOISE at (x/200.0, z/200.0)
     */
    int32_t count(XoroshiroRandomSource& random, const core::BlockPos& origin) override {
        double flowerNoise = BiomeInfoNoise::getValue(
            static_cast<double>(origin.getX()) / 200.0,
            static_cast<double>(origin.getZ()) / 200.0
        );
        return flowerNoise < m_noiseLevel ? m_belowNoise : m_aboveNoise;
    }

private:
    NoiseThresholdCountPlacement(double noiseLevel, int32_t belowNoise, int32_t aboveNoise)
        : m_noiseLevel(noiseLevel)
        , m_belowNoise(belowNoise)
        , m_aboveNoise(aboveNoise) {}
};

//=============================================================================
// SurfaceRelativeThresholdFilter - Filter by Y relative to surface
// Reference: SurfaceRelativeThresholdFilter.java
//=============================================================================

class SurfaceRelativeThresholdFilter : public PlacementFilter {
private:
    Heightmap::Types m_heightmap;
    int32_t m_minInclusive;
    int32_t m_maxInclusive;

public:
    /**
     * Create surface relative threshold filter
     * Reference: SurfaceRelativeThresholdFilter.java of()
     */
    static SurfaceRelativeThresholdFilter of(Heightmap::Types heightmap, int32_t minInclusive, int32_t maxInclusive) {
        return SurfaceRelativeThresholdFilter(heightmap, minInclusive, maxInclusive);
    }

protected:
    /**
     * Check if Y is within range relative to surface
     * Reference: SurfaceRelativeThresholdFilter.java shouldPlace()
     * Algorithm: surfaceY + min <= originY <= surfaceY + max
     */
    bool shouldPlace(
        PlacementContext& context,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) override {
        int64_t surfaceY = static_cast<int64_t>(context.getHeight(m_heightmap, origin.getX(), origin.getZ()));
        int64_t minY = surfaceY + static_cast<int64_t>(m_minInclusive);
        int64_t maxY = surfaceY + static_cast<int64_t>(m_maxInclusive);
        return minY <= static_cast<int64_t>(origin.getY()) && static_cast<int64_t>(origin.getY()) <= maxY;
    }

private:
    SurfaceRelativeThresholdFilter(Heightmap::Types heightmap, int32_t minInclusive, int32_t maxInclusive)
        : m_heightmap(heightmap)
        , m_minInclusive(minInclusive)
        , m_maxInclusive(maxInclusive) {}
};

//=============================================================================
// EnvironmentScanPlacement - Scan for target block in direction
// Reference: EnvironmentScanPlacement.java
//=============================================================================

class EnvironmentScanPlacement : public PlacementModifier {
public:
    enum class Direction {
        UP,
        DOWN
    };

private:
    Direction m_directionOfSearch;
    std::function<bool(const ::world::BlockState&)> m_targetCondition;
    std::function<bool(const ::world::BlockState&)> m_allowedSearchCondition;
    int32_t m_maxSteps;

public:
    /**
     * Create environment scan placement
     * Reference: EnvironmentScanPlacement.java of()
     */
    static EnvironmentScanPlacement scanningFor(
        Direction direction,
        std::function<bool(const ::world::BlockState&)> targetCondition,
        std::function<bool(const ::world::BlockState&)> allowedCondition,
        int32_t maxSteps
    ) {
        return EnvironmentScanPlacement(direction, targetCondition, allowedCondition, maxSteps);
    }

    /**
     * Scan for target position
     * Reference: EnvironmentScanPlacement.java getPositions()
     * Algorithm: Scan up/down until finding target or hitting boundary
     */
    std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) override {
        core::BlockPos::MutableBlockPos pos(origin.getX(), origin.getY(), origin.getZ());

        // Check if starting position is allowed
        ::world::BlockState startState = context.getBlockState(pos);
        if (!m_allowedSearchCondition(startState)) {
            return {};
        }

        int32_t yDelta = (m_directionOfSearch == Direction::UP) ? 1 : -1;

        for (int32_t i = 0; i < m_maxSteps; ++i) {
            ::world::BlockState state = context.getBlockState(pos);
            if (m_targetCondition(state)) {
                return {core::BlockPos(pos.getX(), pos.getY(), pos.getZ())};
            }

            pos.setY(pos.getY() + yDelta);

            // Check bounds
            if (pos.getY() < context.getMinY() || pos.getY() >= context.getMinGenY() + context.getGenDepth()) {
                return {};
            }

            ::world::BlockState movedState = context.getBlockState(pos);
            if (!m_allowedSearchCondition(movedState)) {
                // Final check at current position
                if (m_targetCondition(movedState)) {
                    return {core::BlockPos(pos.getX(), pos.getY(), pos.getZ())};
                }
                return {};
            }
        }

        // Final check after max steps
        ::world::BlockState finalState = context.getBlockState(pos);
        if (m_targetCondition(finalState)) {
            return {core::BlockPos(pos.getX(), pos.getY(), pos.getZ())};
        }

        return {};
    }

private:
    EnvironmentScanPlacement(
        Direction direction,
        std::function<bool(const ::world::BlockState&)> targetCondition,
        std::function<bool(const ::world::BlockState&)> allowedCondition,
        int32_t maxSteps
    )
        : m_directionOfSearch(direction)
        , m_targetCondition(targetCondition)
        , m_allowedSearchCondition(allowedCondition)
        , m_maxSteps(maxSteps) {}
};

//=============================================================================
// FixedPlacement - Place at predefined positions
// Reference: FixedPlacement.java
//=============================================================================

class FixedPlacement : public PlacementModifier {
private:
    std::vector<core::BlockPos> m_positions;

public:
    /**
     * Create fixed placement with positions
     * Reference: FixedPlacement.java of()
     */
    static FixedPlacement of(const std::vector<core::BlockPos>& positions) {
        return FixedPlacement(positions);
    }

    /**
     * Return fixed positions in same chunk
     * Reference: FixedPlacement.java getPositions()
     * Algorithm: Filter positions to current chunk
     */
    std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) override {
        int32_t chunkX = origin.getX() >> 4;
        int32_t chunkZ = origin.getZ() >> 4;

        std::vector<core::BlockPos> result;
        for (const auto& pos : m_positions) {
            if ((pos.getX() >> 4) == chunkX && (pos.getZ() >> 4) == chunkZ) {
                result.push_back(pos);
            }
        }
        return result;
    }

private:
    explicit FixedPlacement(const std::vector<core::BlockPos>& positions)
        : m_positions(positions) {}
};

//=============================================================================
// BlockPredicateFilter - Filter by custom block predicate
// Reference: BlockPredicateFilter.java
//=============================================================================

class BlockPredicateFilter : public PlacementFilter {
private:
    std::function<bool(const PlacementContext&, const core::BlockPos&)> m_predicate;

public:
    /**
     * Create block predicate filter
     * Reference: BlockPredicateFilter.java of()
     */
    static BlockPredicateFilter hasSturdyFace(
        std::function<bool(const PlacementContext&, const core::BlockPos&)> predicate
    ) {
        return BlockPredicateFilter(predicate);
    }

protected:
    /**
     * Test predicate at position
     * Reference: BlockPredicateFilter.java shouldPlace()
     */
    bool shouldPlace(
        PlacementContext& context,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) override {
        return m_predicate(context, origin);
    }

private:
    explicit BlockPredicateFilter(
        std::function<bool(const PlacementContext&, const core::BlockPos&)> predicate
    )
        : m_predicate(predicate) {}
};

//=============================================================================
// CountOnEveryLayerPlacement - Place on every valid layer (deprecated)
// Reference: CountOnEveryLayerPlacement.java
//=============================================================================

class CountOnEveryLayerPlacement : public PlacementModifier {
private:
    const carver::IntProvider* m_count;

public:
    /**
     * Create with IntProvider
     * Reference: CountOnEveryLayerPlacement.java lines 23-25
     */
    static CountOnEveryLayerPlacement of(const carver::IntProvider* count) {
        return CountOnEveryLayerPlacement(count);
    }

    /**
     * Create with constant count
     * Reference: CountOnEveryLayerPlacement.java lines 27-29
     */
    static CountOnEveryLayerPlacement of(int32_t count) {
        static std::vector<carver::ConstantInt> constants;
        constants.push_back(carver::ConstantInt(count));
        return CountOnEveryLayerPlacement(&constants.back());
    }

    /**
     * Get positions on every layer
     * Reference: CountOnEveryLayerPlacement.java lines 31-54
     *
     * Algorithm:
     * 1. For each layer (starting from 0)
     * 2. Sample count() positions at random X,Z within chunk
     * 3. For each position, find the ground Y at that layer
     * 4. Continue until no valid positions found on a layer
     */
    std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) override {
        std::vector<core::BlockPos> positions;
        int32_t layer = 0;
        bool foundAny;

        do {
            foundAny = false;
            int32_t count = m_count->sample(random);

            for (int32_t i = 0; i < count; ++i) {
                int32_t x = random.nextInt(16) + origin.getX();
                int32_t z = random.nextInt(16) + origin.getZ();
                int32_t startY = context.getHeight(Heightmap::Types::MOTION_BLOCKING, x, z);
                int32_t y = findOnGroundYPosition(context, x, startY, z, layer);

                if (y != std::numeric_limits<int32_t>::max()) {
                    positions.push_back(core::BlockPos(x, y, z));
                    foundAny = true;
                }
            }

            ++layer;
        } while (foundAny);

        return positions;
    }

private:
    explicit CountOnEveryLayerPlacement(const carver::IntProvider* count) : m_count(count) {}

    /**
     * Find ground Y position at specified layer
     * Reference: CountOnEveryLayerPlacement.java lines 60-80
     */
    static int32_t findOnGroundYPosition(
        PlacementContext& context,
        int32_t xStart,
        int32_t yStart,
        int32_t zStart,
        int32_t layerToPlaceOn
    ) {
        core::BlockPos::MutableBlockPos currentPos(xStart, yStart, zStart);
        int32_t currentLayer = 0;
        ::world::BlockState currentBlock = context.getBlockState(currentPos);

        for (int32_t y = yStart; y >= context.getMinY() + 1; --y) {
            currentPos.set(xStart, y - 1, zStart);
            ::world::BlockState belowBlock = context.getBlockState(currentPos);

            if (!isEmpty(belowBlock) && isEmpty(currentBlock) &&
                belowBlock.getBlockName() != "minecraft:bedrock") {
                if (currentLayer == layerToPlaceOn) {
                    return currentPos.getY() + 1;
                }
                ++currentLayer;
            }

            currentBlock = belowBlock;
        }

        return std::numeric_limits<int32_t>::max();
    }

    /**
     * Check if block is empty (air, water, or lava)
     * Reference: CountOnEveryLayerPlacement.java lines 82-84
     */
    static bool isEmpty(const ::world::BlockState& state) {
        const std::string& name = state.getBlockName();
        return state.isAir() ||
               name == "minecraft:water" ||
               name == "minecraft:lava";
    }
};

} // namespace placement
} // namespace levelgen
} // namespace minecraft
