#pragma once

#include "levelgen/carver/WorldCarver.h"
#include <cmath>
#include <vector>

// Reference: net/minecraft/world/level/levelgen/carver/CanyonWorldCarver.java

namespace minecraft {
namespace levelgen {
namespace carver {

/**
 * CanyonWorldCarver - Carves ravine/canyon formations
 * Reference: CanyonWorldCarver.java
 */
class CanyonWorldCarver : public WorldCarver<CanyonCarverConfiguration> {
public:
    CanyonWorldCarver() = default;

    /**
     * Check if this chunk should start carving (XoroshiroRandomSource version)
     * Reference: CanyonWorldCarver.java lines 20-22
     */
    bool isStartChunk(const CanyonCarverConfiguration& configuration, XoroshiroRandomSource& random) override {
        return random.nextFloat() <= configuration.probability;
    }

    /**
     * Check if this chunk should start carving (LegacyRandomSource version)
     * Reference: CanyonWorldCarver.java lines 20-22
     * Note: Java uses LegacyRandomSource for carving
     */
    bool isStartChunk(const CanyonCarverConfiguration& configuration, LegacyRandomSource& random) override {
        return random.nextFloat() <= configuration.probability;
    }

    /**
     * Carve canyons in the given chunk (XoroshiroRandomSource version)
     * Reference: CanyonWorldCarver.java lines 24-37
     */
    bool carve(
        CarvingContext& context,
        const CanyonCarverConfiguration& configuration,
        ::world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        XoroshiroRandomSource& random,
        Aquifer* aquifer,
        const ::world::ChunkPos& sourceChunkPos,
        CarvingMask& mask
    ) override;

    /**
     * Carve canyons in the given chunk (LegacyRandomSource version)
     * Reference: CanyonWorldCarver.java lines 24-37
     * Note: Java uses LegacyRandomSource for carving
     */
    bool carve(
        CarvingContext& context,
        const CanyonCarverConfiguration& configuration,
        ::world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        LegacyRandomSource& random,
        Aquifer* aquifer,
        const ::world::ChunkPos& sourceChunkPos,
        CarvingMask& mask
    ) override;

private:
    /**
     * Do the actual canyon carving (LegacyRandomSource version)
     * Reference: CanyonWorldCarver.java lines 39-71
     */
    void doCarve(
        CarvingContext& context,
        const CanyonCarverConfiguration& configuration,
        ::world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        int64_t tunnelSeed,
        Aquifer* aquifer,
        double x, double y, double z,
        float thickness,
        float horizontalRotation,
        float verticalRotation,
        int32_t step,
        int32_t distance,
        double yScale,
        CarvingMask& mask
    );

    /**
     * Initialize width factors per height level (LegacyRandomSource version)
     * Reference: CanyonWorldCarver.java lines 73-87
     */
    std::vector<float> initWidthFactors(
        const CarvingContext& context,
        const CanyonCarverConfiguration& configuration,
        LegacyRandomSource& random
    );

    /**
     * Update vertical radius based on position (LegacyRandomSource version)
     * Reference: CanyonWorldCarver.java lines 89-93
     */
    double updateVerticalRadius(
        const CanyonCarverConfiguration& configuration,
        LegacyRandomSource& random,
        double verticalRadius,
        float distance,
        float currentStep
    );

    /**
     * Check if carving should be skipped at this position
     * Reference: CanyonWorldCarver.java lines 95-98
     */
    bool shouldSkip(
        const CarvingContext& context,
        const std::vector<float>& widthFactorPerHeight,
        double xd, double yd, double zd,
        int32_t y
    );
};

} // namespace carver
} // namespace levelgen
} // namespace minecraft
