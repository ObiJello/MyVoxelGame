#pragma once

#include "levelgen/SurfaceRules.h"
#include "levelgen/Heightmap.h"
#include "levelgen/carver/CarvingMask.h"
#include "world/ChunkPos.h"
#include "world/level/block/state/BlockState.h"
#include "core/BlockPos.h"
#include <optional>
#include <functional>

// Reference: net/minecraft/world/level/levelgen/placement/PlacementContext.java

namespace minecraft {
namespace levelgen {

class ChunkGenerator;

namespace placement {

// Forward declaration
class PlacedFeature;

/**
 * PlacementContext - Context for feature placement operations
 * Extends WorldGenerationContext with level access
 * Reference: PlacementContext.java
 */
class PlacementContext : public WorldGenerationContext {
private:
    // Reference to world generation level
    std::function<int32_t(Heightmap::Types, int32_t, int32_t)> m_heightGetter;
    std::function<BlockState*(const core::BlockPos&)> m_blockGetter;
    std::function<void*(const core::BlockPos&)> m_biomeGetter;
    std::function<carver::CarvingMask*(const world::ChunkPos&)> m_carvingMaskGetter;
    ChunkGenerator* m_generator;
    std::optional<const PlacedFeature*> m_topFeature;
    int32_t m_minY;

public:
    /**
     * Constructor
     * Reference: PlacementContext.java lines 19-24
     */
    PlacementContext(
        int32_t minY,
        int32_t height,
        std::function<int32_t(Heightmap::Types, int32_t, int32_t)> heightGetter,
        std::function<BlockState*(const core::BlockPos&)> blockGetter,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        std::function<carver::CarvingMask*(const world::ChunkPos&)> carvingMaskGetter,
        ChunkGenerator* generator,
        std::optional<const PlacedFeature*> topFeature = std::nullopt
    )
        : WorldGenerationContext(minY, height)
        , m_heightGetter(heightGetter)
        , m_blockGetter(blockGetter)
        , m_biomeGetter(biomeGetter)
        , m_carvingMaskGetter(carvingMaskGetter)
        , m_generator(generator)
        , m_topFeature(topFeature)
        , m_minY(minY)
    {}

    /**
     * Get height at position using specified heightmap type
     * Reference: PlacementContext.java lines 26-28
     */
    int32_t getHeight(Heightmap::Types type, int32_t x, int32_t z) const {
        return m_heightGetter(type, x, z);
    }

    /**
     * Get carving mask for chunk
     * Reference: PlacementContext.java lines 30-32
     */
    carver::CarvingMask* getCarvingMask(const world::ChunkPos& pos) const {
        return m_carvingMaskGetter(pos);
    }

    /**
     * Get block state at position
     * Reference: PlacementContext.java lines 34-36
     */
    BlockState* getBlockState(const core::BlockPos& pos) const {
        return m_blockGetter(pos);
    }

    /**
     * Get minimum Y coordinate
     * Reference: PlacementContext.java lines 38-40
     */
    int32_t getMinY() const {
        return m_minY;
    }

    /**
     * Get the top feature being placed (for biome filtering)
     * Reference: PlacementContext.java lines 46-48
     */
    std::optional<const PlacedFeature*> topFeature() const {
        return m_topFeature;
    }

    /**
     * Get the chunk generator
     * Reference: PlacementContext.java lines 50-52
     */
    ChunkGenerator* generator() const {
        return m_generator;
    }

    /**
     * Get biome at position
     */
    void* getBiome(const core::BlockPos& pos) const {
        return m_biomeGetter(pos);
    }
};

} // namespace placement
} // namespace levelgen
} // namespace minecraft
