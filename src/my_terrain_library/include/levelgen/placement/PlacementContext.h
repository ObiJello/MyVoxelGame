#pragma once

#include "levelgen/WorldGenLevel.h"
#include "levelgen/SurfaceRules.h"
#include "levelgen/Heightmap.h"
#include "levelgen/carver/CarvingMask.h"
#include "world/ChunkPos.h"
#include "world/level/block/state/BlockState.h"
#include "world/IChunk.h"
#include "world/ProtoChunk.h"
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
    WorldGenLevel* m_level;
    ChunkGenerator* m_generator;
    std::optional<const PlacedFeature*> m_topFeature;

public:
    /**
     * Constructor matching Java signature
     * Reference: PlacementContext.java lines 19-24
     */
    PlacementContext(
        WorldGenLevel* level,
        ChunkGenerator* generator,
        std::optional<const PlacedFeature*> topFeature = std::nullopt
    );

    /**
     * Get height at position using specified heightmap type
     * Reference: PlacementContext.java lines 26-28
     */
    int32_t getHeight(Heightmap::Types type, int32_t x, int32_t z) const {
        return m_level->getHeight(type, x, z);
    }

    /**
     * Get carving mask for chunk
     * Reference: PlacementContext.java lines 30-32
     */
    carver::CarvingMask* getCarvingMask(const world::ChunkPos& pos) const {
        ::world::IChunk* chunk = const_cast<WorldGenLevel*>(m_level)->getChunk(pos.x(), pos.z());
        if (auto* protoChunk = dynamic_cast<::world::ProtoChunk*>(chunk)) {
            return &protoChunk->getOrCreateCarvingMask();
        }
        return nullptr;
    }

    /**
     * Get block state at position
     * Reference: PlacementContext.java lines 34-36
     */
    BlockState* getBlockState(const core::BlockPos& pos) const {
        return m_level->getBlockState(pos);
    }

    /**
     * Get minimum Y coordinate
     * Reference: PlacementContext.java lines 38-40
     */
    int32_t getMinY() const {
        return m_level->getMinY();
    }

    /**
     * Get the level
     * Reference: PlacementContext.java lines 42-44
     */
    WorldGenLevel* getLevel() const {
        return m_level;
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
     * Get biome at position (via WorldGenLevel)
     */
    const world::biome::Biome* getBiome(const core::BlockPos& pos) const {
        return m_level->getBiome(pos);
    }
};

} // namespace placement
} // namespace levelgen
} // namespace minecraft
