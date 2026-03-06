#pragma once

#include "levelgen/carver/WorldCarver.h"
#include <cmath>

// Reference: net/minecraft/world/level/levelgen/carver/CaveWorldCarver.java

namespace minecraft {
namespace levelgen {
namespace carver {

/**
 * CaveWorldCarver - Carves cave systems with rooms and tunnels
 * Reference: CaveWorldCarver.java
 */
class CaveWorldCarver : public WorldCarver<CaveCarverConfiguration> {
public:
    CaveWorldCarver() = default;

    /**
     * Check if this chunk should start carving (XoroshiroRandomSource version)
     * Reference: CaveWorldCarver.java lines 21-23
     */
    bool isStartChunk(const CaveCarverConfiguration& configuration, XoroshiroRandomSource& random) override {
        return random.nextFloat() <= configuration.probability;
    }

    /**
     * Check if this chunk should start carving (LegacyRandomSource version)
     * Reference: CaveWorldCarver.java lines 21-23
     * Note: Java uses LegacyRandomSource for carving
     */
    bool isStartChunk(const CaveCarverConfiguration& configuration, LegacyRandomSource& random) override {
        return random.nextFloat() <= configuration.probability;
    }

    /**
     * Carve caves in the given chunk (XoroshiroRandomSource version)
     * Reference: CaveWorldCarver.java lines 25-56
     */
    bool carve(
        CarvingContext& context,
        const CaveCarverConfiguration& configuration,
        world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        XoroshiroRandomSource& random,
        Aquifer* aquifer,
        const world::ChunkPos& sourceChunkPos,
        CarvingMask& mask
    ) override;

    /**
     * Carve caves in the given chunk (LegacyRandomSource version)
     * Reference: CaveWorldCarver.java lines 25-56
     * Note: Java uses LegacyRandomSource for carving
     */
    bool carve(
        CarvingContext& context,
        const CaveCarverConfiguration& configuration,
        world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        LegacyRandomSource& random,
        Aquifer* aquifer,
        const world::ChunkPos& sourceChunkPos,
        CarvingMask& mask
    ) override;

protected:
    /**
     * Get the maximum number of caves per chunk
     * Reference: CaveWorldCarver.java lines 58-60
     */
    virtual int32_t getCaveBound() const {
        return 15;
    }

    /**
     * Get random thickness for a tunnel (XoroshiroRandomSource version)
     * Reference: CaveWorldCarver.java lines 62-69
     */
    virtual float getThickness(XoroshiroRandomSource& random) const;

    /**
     * Get random thickness for a tunnel (LegacyRandomSource version)
     * Reference: CaveWorldCarver.java lines 62-69
     */
    virtual float getThickness(LegacyRandomSource& random) const;

    /**
     * Get Y scale factor
     * Reference: CaveWorldCarver.java lines 71-73
     */
    virtual double getYScale() const {
        return 1.0;
    }

    /**
     * Create a cave room at the given position
     * Reference: CaveWorldCarver.java lines 75-79
     */
    void createRoom(
        CarvingContext& context,
        const CaveCarverConfiguration& configuration,
        world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        Aquifer* aquifer,
        double x, double y, double z,
        float thickness,
        double yScale,
        CarvingMask& mask,
        CarveSkipChecker skipChecker
    );

    /**
     * Create a cave tunnel from the given position (uses XoroshiroRandomSource)
     * Reference: CaveWorldCarver.java lines 81-117
     */
    void createTunnel(
        CarvingContext& context,
        const CaveCarverConfiguration& configuration,
        world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        int64_t tunnelSeed,
        Aquifer* aquifer,
        double x, double y, double z,
        double horizontalRadiusMultiplier,
        double verticalRadiusMultiplier,
        float thickness,
        float horizontalRotation,
        float verticalRotation,
        int32_t step,
        int32_t dist,
        double yScale,
        CarvingMask& mask,
        CarveSkipChecker skipChecker
    );

    /**
     * Create a cave tunnel from the given position (uses LegacyRandomSource)
     * Reference: CaveWorldCarver.java lines 81-117
     * Note: Java uses LegacyRandomSource for carving
     */
    void createTunnelLegacy(
        CarvingContext& context,
        const CaveCarverConfiguration& configuration,
        world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        int64_t tunnelSeed,
        Aquifer* aquifer,
        double x, double y, double z,
        double horizontalRadiusMultiplier,
        double verticalRadiusMultiplier,
        float thickness,
        float horizontalRotation,
        float verticalRotation,
        int32_t step,
        int32_t dist,
        double yScale,
        CarvingMask& mask,
        CarveSkipChecker skipChecker
    );

private:
    /**
     * Check if carving should be skipped at this position
     * Reference: CaveWorldCarver.java lines 119-125
     */
    static bool shouldSkip(double xd, double yd, double zd, double floorLevel);
};

} // namespace carver
} // namespace levelgen
} // namespace minecraft
