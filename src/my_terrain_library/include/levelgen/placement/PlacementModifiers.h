#pragma once

#include "levelgen/placement/PlacementModifier.h"
#include "levelgen/placement/PlacementContext.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/Heightmap.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/ChunkGenerator.h"
#include "world/biome/Biome.h"
#include "synth/PerlinSimplexNoise.h"
#include <cmath>
#include <limits>
#include <deque>

// Reference: Various placement modifier files in net/minecraft/world/level/levelgen/placement/

namespace minecraft {
namespace levelgen {
namespace placement {

inline const char* heightmapTypeName(Heightmap::Types type) {
    switch (type) {
        case Heightmap::Types::WORLD_SURFACE_WG: return "WORLD_SURFACE_WG";
        case Heightmap::Types::WORLD_SURFACE: return "WORLD_SURFACE";
        case Heightmap::Types::OCEAN_FLOOR_WG: return "OCEAN_FLOOR_WG";
        case Heightmap::Types::OCEAN_FLOOR: return "OCEAN_FLOOR";
        case Heightmap::Types::MOTION_BLOCKING: return "MOTION_BLOCKING";
        case Heightmap::Types::MOTION_BLOCKING_NO_LEAVES: return "MOTION_BLOCKING_NO_LEAVES";
    }
    return "UNKNOWN";
}

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
        WorldgenRandom& random,
        const core::BlockPos& origin
    ) override {
        int32_t x = random.nextInt(16) + origin.getX();
        int32_t z = random.nextInt(16) + origin.getZ();
        return {core::BlockPos(x, origin.getY(), z)};
    }

    std::string getTypeName() const override { return "InSquarePlacement"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        (void)context;
        if (results.empty()) {
            return "offsets=none";
        }

        const core::BlockPos& result = results.front();
        return "dx=" + std::to_string(result.getX() - origin.getX()) +
               " dz=" + std::to_string(result.getZ() - origin.getZ());
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
     * NOTE: Using std::deque to avoid pointer invalidation on push_back
     */
    static CountPlacement of(int32_t count) {
        static std::deque<carver::ConstantInt> constants;
        constants.push_back(carver::ConstantInt(count));
        return CountPlacement(&constants.back());
    }

    /**
     * Create count with extra chance
     * Reference: PlacementUtils.java countExtra()
     * Returns base count, plus `extra` with probability `chance`
     */
    static CountPlacement countExtra(int32_t base, float chance, int32_t extra) {
        // Create a custom IntProvider that returns base + extra if random < chance
        // Using WeightedRandomInt-style approach
        static std::deque<carver::CountExtraIntProvider> providers;
        providers.push_back(carver::CountExtraIntProvider(base, chance, extra));
        return CountPlacement(&providers.back());
    }

    std::string getTypeName() const override { return "CountPlacement"; }

protected:
    /**
     * Get count for this placement
     * Reference: CountPlacement.java lines 25-27
     */
    int32_t count(WorldgenRandom& random, const core::BlockPos& origin) override {
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
        WorldgenRandom& random,
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

    std::string getTypeName() const override { return "HeightmapPlacement"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        int32_t height = context.getHeight(m_heightmap, origin.getX(), origin.getZ());
        return std::string("heightmap=") + heightmapTypeName(m_heightmap) +
               " height=" + std::to_string(height) +
               " accepted=" + (results.empty() ? "false" : "true");
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
     * NOTE: Using std::deque instead of std::vector to avoid pointer invalidation
     *       when elements are added (deque never invalidates pointers on push_back)
     */
    static HeightRangePlacement uniform(const VerticalAnchor& minInclusive, const VerticalAnchor& maxInclusive) {
        static std::deque<carver::UniformHeight> heights;
        heights.push_back(carver::UniformHeight(minInclusive, maxInclusive));
        return HeightRangePlacement(&heights.back());
    }

    /**
     * Create uniform height range from absolute Y values
     * Convenience overload that creates VerticalAnchor::absolute() internally
     */
    static HeightRangePlacement uniform(int32_t minY, int32_t maxY) {
        return uniform(VerticalAnchor::absolute(minY), VerticalAnchor::absolute(maxY));
    }

    /**
     * Create triangle height range (trapezoid with plateau=0)
     * Reference: HeightRangePlacement.java lines 29-31
     * NOTE: Using std::deque instead of std::vector to avoid pointer invalidation
     */
    static HeightRangePlacement triangle(const VerticalAnchor& minInclusive, const VerticalAnchor& maxInclusive) {
        static std::deque<carver::TrapezoidHeight> heights;
        heights.push_back(carver::TrapezoidHeight(minInclusive, maxInclusive, 0));
        return HeightRangePlacement(&heights.back());
    }

    /**
     * Get positions - set Y from height provider
     * Reference: HeightRangePlacement.java lines 33-35
     */
    std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        WorldgenRandom& random,
        const core::BlockPos& origin
    ) override {
        int32_t y = m_height->sample(random, context);
        return {core::BlockPos(origin.getX(), y, origin.getZ())};
    }

    std::string getTypeName() const override { return "HeightRangePlacement"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        (void)context;
        (void)origin;
        if (results.empty()) {
            return "selected_y=none";
        }
        return "selected_y=" + std::to_string(results.front().getY());
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

    std::string getTypeName() const override { return "RarityFilter"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        (void)context;
        (void)origin;
        return "accepted=" + std::string(results.empty() ? "false" : "true") +
               " chance=1/" + std::to_string(m_chance);
    }

protected:
    /**
     * Check if placement should occur
     * Reference: RarityFilter.java lines 20-22
     */
    bool shouldPlace(
        PlacementContext& context,
        WorldgenRandom& random,
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

    /**
     * Debug helpers for tracking BiomeFilter behavior
     */
    static void setDebugEnabled(bool enabled);
    static void printDebugStats();
    static int getFilteredCount();
    static int getCallCount();

    std::string getTypeName() const override { return "BiomeFilter"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        const world::biome::Biome* biome = context.getBiome(origin);
        std::string biomeName = biome ? biome->getName() : "(null)";
        auto topFeature = context.topFeature();
        std::string featureName = topFeature.has_value() && *topFeature
            ? (*topFeature)->getDebugName()
            : "(no-top-feature)";
        return "accepted=" + std::string(results.empty() ? "false" : "true") +
               " biome=" + biomeName +
               " top_feature=" + featureName;
    }

protected:
    /**
     * Check if biome has this feature
     * Reference: BiomeFilter.java lines 20-24
     */
    bool shouldPlace(
        PlacementContext& context,
        WorldgenRandom& random,
        const core::BlockPos& origin
    ) override;

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
        WorldgenRandom& random,
        const core::BlockPos& origin
    ) override {
        int32_t scatterX = origin.getX() + m_xzSpread->sample(random);
        int32_t scatterY = origin.getY() + m_ySpread->sample(random);
        int32_t scatterZ = origin.getZ() + m_xzSpread->sample(random);
        return {core::BlockPos(scatterX, scatterY, scatterZ)};
    }

    std::string getTypeName() const override { return "RandomOffsetPlacement"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        (void)context;
        if (results.empty()) {
            return "offsets=none";
        }
        const core::BlockPos& result = results.front();
        return "dx=" + std::to_string(result.getX() - origin.getX()) +
               " dy=" + std::to_string(result.getY() - origin.getY()) +
               " dz=" + std::to_string(result.getZ() - origin.getZ());
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

    std::string getTypeName() const override { return "SurfaceWaterDepthFilter"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        int32_t surfaceY = context.getHeight(Heightmap::Types::OCEAN_FLOOR, origin.getX(), origin.getZ());
        int32_t waterY = context.getHeight(Heightmap::Types::WORLD_SURFACE, origin.getX(), origin.getZ());
        int32_t delta = waterY - surfaceY;
        return "accepted=" + std::string(results.empty() ? "false" : "true") +
               " ocean_floor=" + std::to_string(surfaceY) +
               " world_surface=" + std::to_string(waterY) +
               " delta=" + std::to_string(delta) +
               " max_depth=" + std::to_string(m_maxWaterDepth);
    }

protected:
    /**
     * Check water depth at position
     * Reference: SurfaceWaterDepthFilter.java shouldPlace
     */
    bool shouldPlace(
        PlacementContext& context,
        WorldgenRandom& random,
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

    std::string getTypeName() const override { return "NoiseBasedCountPlacement"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        (void)context;
        double flowerNoise = synth::BiomeInfoNoise::getValue(
            static_cast<double>(origin.getX()) / m_noiseFactor,
            static_cast<double>(origin.getZ()) / m_noiseFactor
        );
        return "noise=" + std::to_string(flowerNoise) +
               " count=" + std::to_string(results.size()) +
               " ratio=" + std::to_string(m_noiseToCountRatio) +
               " offset=" + std::to_string(m_noiseOffset);
    }

protected:
    /**
     * Get count based on biome noise
     * Reference: NoiseBasedCountPlacement.java count() lines 26-28
     * Algorithm: ceil((noise + offset) * ratio)
     *
     * Uses Biome.BIOME_INFO_NOISE at (x/noiseFactor, z/noiseFactor)
     */
    int32_t count(WorldgenRandom& random, const core::BlockPos& origin) override {
        double flowerNoise = synth::BiomeInfoNoise::getValue(
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

    std::string getTypeName() const override { return "NoiseThresholdCountPlacement"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        (void)context;
        double flowerNoise = synth::BiomeInfoNoise::getValue(
            static_cast<double>(origin.getX()) / 200.0,
            static_cast<double>(origin.getZ()) / 200.0
        );
        return "noise=" + std::to_string(flowerNoise) +
               " threshold=" + std::to_string(m_noiseLevel) +
               " count=" + std::to_string(results.size()) +
               " below=" + std::to_string(m_belowNoise) +
               " above=" + std::to_string(m_aboveNoise);
    }

protected:
    /**
     * Get count based on biome noise threshold
     * Reference: NoiseThresholdCountPlacement.java count() lines 26-28
     * Algorithm: if noise < threshold -> belowNoise else aboveNoise
     *
     * Uses Biome.BIOME_INFO_NOISE at (x/200.0, z/200.0)
     */
    int32_t count(WorldgenRandom& random, const core::BlockPos& origin) override {
        double flowerNoise = synth::BiomeInfoNoise::getValue(
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

    std::string getTypeName() const override { return "SurfaceRelativeThresholdFilter"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        int64_t surfaceY = static_cast<int64_t>(context.getHeight(m_heightmap, origin.getX(), origin.getZ()));
        int64_t minY = surfaceY + static_cast<int64_t>(m_minInclusive);
        int64_t maxY = surfaceY + static_cast<int64_t>(m_maxInclusive);
        return std::string("accepted=") + (results.empty() ? "false" : "true") +
               " heightmap=" + heightmapTypeName(m_heightmap) +
               " y=" + std::to_string(origin.getY()) +
               " min=" + std::to_string(minY) +
               " max=" + std::to_string(maxY);
    }

protected:
    /**
     * Check if Y is within range relative to surface
     * Reference: SurfaceRelativeThresholdFilter.java shouldPlace()
     * Algorithm: surfaceY + min <= originY <= surfaceY + max
     */
    bool shouldPlace(
        PlacementContext& context,
        WorldgenRandom& random,
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
    std::function<bool(PlacementContext&, const core::BlockPos&)> m_targetCondition;
    std::function<bool(PlacementContext&, const core::BlockPos&)> m_allowedSearchCondition;
    int32_t m_maxSteps;

public:
    /**
     * Create environment scan placement
     * Reference: EnvironmentScanPlacement.java of()
     */
    static EnvironmentScanPlacement scanningFor(
        Direction direction,
        std::shared_ptr<blockpredicates::BlockPredicate> targetCondition,
        std::shared_ptr<blockpredicates::BlockPredicate> allowedCondition,
        int32_t maxSteps
    ) {
        return EnvironmentScanPlacement(
            direction,
            [targetCondition](PlacementContext& context, const core::BlockPos& pos) {
                return targetCondition && targetCondition->test(*context.getLevel(), pos);
            },
            [allowedCondition](PlacementContext& context, const core::BlockPos& pos) {
                return allowedCondition && allowedCondition->test(*context.getLevel(), pos);
            },
            maxSteps
        );
    }

    static EnvironmentScanPlacement scanningFor(
        Direction direction,
        std::shared_ptr<blockpredicates::BlockPredicate> targetCondition,
        int32_t maxSteps
    ) {
        return scanningFor(direction, targetCondition, blockpredicates::BlockPredicate::alwaysTrue(), maxSteps);
    }

    static EnvironmentScanPlacement scanningFor(
        Direction direction,
        std::function<bool(BlockState*)> targetCondition,
        std::function<bool(BlockState*)> allowedCondition,
        int32_t maxSteps
    ) {
        return EnvironmentScanPlacement(
            direction,
            [targetCondition](PlacementContext& context, const core::BlockPos& pos) {
                return targetCondition(context.getBlockState(pos));
            },
            [allowedCondition](PlacementContext& context, const core::BlockPos& pos) {
                return allowedCondition(context.getBlockState(pos));
            },
            maxSteps
        );
    }

    std::string getTypeName() const override { return "EnvironmentScanPlacement"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        (void)context;
        std::string direction = (m_directionOfSearch == Direction::UP) ? "UP" : "DOWN";
        if (results.empty()) {
            return "direction=" + direction + " found=false max_steps=" + std::to_string(m_maxSteps);
        }
        return "direction=" + direction +
               " found=true max_steps=" + std::to_string(m_maxSteps) +
               " result_y=" + std::to_string(results.front().getY());
    }

    /**
     * Scan for target position
     * Reference: EnvironmentScanPlacement.java getPositions()
     * Algorithm: Scan up/down until finding target or hitting boundary
     */
    std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        WorldgenRandom& random,
        const core::BlockPos& origin
    ) override {
        core::BlockPos::MutableBlockPos pos(origin.getX(), origin.getY(), origin.getZ());

        if (!m_allowedSearchCondition(context, pos)) {
            return {};
        }

        int32_t yDelta = (m_directionOfSearch == Direction::UP) ? 1 : -1;

        for (int32_t i = 0; i < m_maxSteps; ++i) {
            if (m_targetCondition(context, pos)) {
                return {core::BlockPos(pos.getX(), pos.getY(), pos.getZ())};
            }

            pos.setY(pos.getY() + yDelta);

            if (context.getLevel()->isOutsideBuildHeight(pos)) {
                return {};
            }

            if (!m_allowedSearchCondition(context, pos)) {
                if (m_targetCondition(context, pos)) {
                    return {core::BlockPos(pos.getX(), pos.getY(), pos.getZ())};
                }
                return {};
            }
        }

        if (m_targetCondition(context, pos)) {
            return {core::BlockPos(pos.getX(), pos.getY(), pos.getZ())};
        }

        return {};
    }

private:
    EnvironmentScanPlacement(
        Direction direction,
        std::function<bool(PlacementContext&, const core::BlockPos&)> targetCondition,
        std::function<bool(PlacementContext&, const core::BlockPos&)> allowedCondition,
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

    std::string getTypeName() const override { return "FixedPlacement"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        (void)context;
        return "matched_positions=" + std::to_string(results.size()) +
               " chunk=" + std::to_string(origin.getX() >> 4) + "," + std::to_string(origin.getZ() >> 4);
    }

    /**
     * Return fixed positions in same chunk
     * Reference: FixedPlacement.java getPositions()
     * Algorithm: Filter positions to current chunk
     */
    std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        WorldgenRandom& random,
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
    static BlockPredicateFilter forPredicate(
        std::shared_ptr<blockpredicates::BlockPredicate> predicate
    ) {
        return BlockPredicateFilter([predicate](const PlacementContext& context, const core::BlockPos& origin) {
            return predicate->test(*context.getLevel(), origin);
        });
    }

    static BlockPredicateFilter hasSturdyFace(
        std::function<bool(const PlacementContext&, const core::BlockPos&)> predicate
    ) {
        return BlockPredicateFilter(predicate);
    }

    std::string getTypeName() const override { return "BlockPredicateFilter"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        (void)context;
        (void)origin;
        return "accepted=" + std::string(results.empty() ? "false" : "true");
    }

protected:
    /**
     * Test predicate at position
     * Reference: BlockPredicateFilter.java shouldPlace()
     */
    bool shouldPlace(
        PlacementContext& context,
        WorldgenRandom& random,
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

    std::string getTypeName() const override { return "CountOnEveryLayerPlacement"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        (void)context;
        (void)origin;
        return "count=" + std::to_string(results.size());
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
        WorldgenRandom& random,
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
        BlockState* currentBlock = context.getBlockState(currentPos);

        for (int32_t y = yStart; y >= context.getMinY() + 1; --y) {
            currentPos.set(xStart, y - 1, zStart);
            BlockState* belowBlock = context.getBlockState(currentPos);

            if (!isEmpty(belowBlock) && isEmpty(currentBlock) &&
                belowBlock && belowBlock->getBlockName() != "minecraft:bedrock") {
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
    static bool isEmpty(BlockState* state) {
        if (!state) return true;
        const std::string& name = state->getBlockName();
        return state->isAir() ||
               name == "minecraft:water" ||
               name == "minecraft:lava";
    }
};

} // namespace placement
} // namespace levelgen
} // namespace minecraft
