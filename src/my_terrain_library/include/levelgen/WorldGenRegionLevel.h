#pragma once

/**
 * WorldGenRegionLevel - Adapter that wraps WorldGenRegion as WorldGenLevel
 * Reference: This is the C++ equivalent of how Java's WorldGenRegion implements WorldGenLevel
 *
 * This adapter enables multi-chunk access for features by delegating to WorldGenRegion's
 * chunk cache. It's the proper way to give features access to neighboring chunks.
 */

#include "levelgen/WorldGenLevel.h"
#include "levelgen/feature/BlockChangeTrace.h"
#include "server/level/WorldGenRegion.h"
#include "core/BlockPos.h"
#include "core/QuartPos.h"
#include "world/ChunkPos.h"
#include "world/level/block/state/BlockState.h"
#include "world/biome/Biome.h"
#include "world/biome/BiomeManager.h"

namespace minecraft {
namespace levelgen {

/**
 * WorldGenRegionLevel - Wraps WorldGenRegion to implement WorldGenLevel interface
 */
class WorldGenRegionLevel : public WorldGenLevel {
private:
    server::level::WorldGenRegion* m_region;

public:
    /**
     * Constructor
     * @param region - The WorldGenRegion to wrap
     */
    explicit WorldGenRegionLevel(server::level::WorldGenRegion* region)
        : m_region(region)
    {}

    //=========================================================================
    // Block Access (delegated to WorldGenRegion)
    //=========================================================================

    BlockState* getBlockState(const core::BlockPos& pos) const override {
        // WorldGenRegion::getBlockState throws if position is outside loaded chunks.
        // Features may read blocks at arbitrary positions (e.g., checking neighbors),
        // so we catch exceptions and return AIR for out-of-bounds — matching Java's
        // behavior where distant chunks have empty sections returning AIR.
        try {
            return m_region->getBlockState(pos);
        } catch (...) {
            // Return AIR for out-of-bounds (not nullptr — callers may dereference)
            return world::level::block::Blocks::AIR->defaultBlockState();
        }
    }

    bool setBlock(const core::BlockPos& pos, BlockState* state, int flags) override {
        // Silently fail for positions outside writable region.
        // WorldGenRegion::ensureCanWrite already handles this, but getChunk can
        // throw for positions outside the cache even after ensureCanWrite passes.
        try {
            BlockState* oldState = nullptr;
            if (feature::BlockChangeTrace::isEnabled()) {
                oldState = getBlockState(pos);
            }

            bool result = m_region->setBlock(pos, state, flags, 512);
            if (result && state && oldState) {
                feature::BlockChangeTrace::log(
                    pos.getX(),
                    pos.getY(),
                    pos.getZ(),
                    oldState->getIdentifier(),
                    state->getIdentifier()
                );
            }
            return result;
        } catch (...) {
            return false;
        }
    }

    bool isStateAtPosition(const core::BlockPos& pos,
                          std::function<bool(BlockState*)> predicate) const override {
        BlockState* state = getBlockState(pos);
        if (!state) return predicate(world::level::block::Blocks::AIR->defaultBlockState());
        return predicate(state);
    }

    bool isFluidAtPosition(const core::BlockPos& pos,
                          std::function<bool(BlockState*)> predicate) const override {
        BlockState* state = getBlockState(pos);
        return state && state->hasAnyFluid() && predicate(state);
    }

    //=========================================================================
    // Chunk Access (delegated to WorldGenRegion)
    //=========================================================================

    ::world::IChunk* getChunk(int chunkX, int chunkZ) override {
        try {
            return m_region->getChunk(chunkX, chunkZ);
        } catch (...) {
            return nullptr;
        }
    }

    //=========================================================================
    // Height Access
    //=========================================================================

    int getHeight(Heightmap::Types type, int x, int z) const override {
        try {
            int chunkX = x >> 4;
            int chunkZ = z >> 4;
            ::world::IChunk* chunk = const_cast<server::level::WorldGenRegion*>(m_region)->getChunk(chunkX, chunkZ);
            if (chunk) {
                return chunk->getHeight(static_cast<int>(type), x & 15, z & 15) + 1;
            }
        } catch (...) {}
        return 0;
    }

    int getMinY() const override {
        return m_region->getMinY();
    }

    int getMaxY() const override {
        return m_region->getMinY() + m_region->getHeight();
    }

    //=========================================================================
    // Biome Access
    //=========================================================================

    const world::biome::Biome* getBiome(const core::BlockPos& pos) const override {
        try {
            class RegionBiomeSource : public world::biome::BiomeManager::NoiseBiomeSource {
            public:
                explicit RegionBiomeSource(const server::level::WorldGenRegion* region)
                    : m_region(region) {}

                world::biome::BiomeHolder getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ) const override {
                    int32_t blockX = core::QuartPos::toBlock(quartX);
                    int32_t blockY = core::QuartPos::toBlock(quartY);
                    int32_t blockZ = core::QuartPos::toBlock(quartZ);
                    int chunkX = blockX >> 4;
                    int chunkZ = blockZ >> 4;

                    ::world::IChunk* chunk = const_cast<server::level::WorldGenRegion*>(m_region)->getChunk(
                        chunkX,
                        chunkZ,
                        world::chunk::status::ChunkStatus::BIOMES,
                        false
                    );
                    if (chunk) {
                        return chunk->getBiome(core::BlockPos(blockX, blockY, blockZ));
                    }

                    return world::biome::Biomes::get(world::biome::BiomeKeys::PLAINS);
                }

            private:
                const server::level::WorldGenRegion* m_region;
            };

            RegionBiomeSource biomeSource(m_region);
            world::biome::BiomeManager biomeManager(
                &biomeSource,
                world::biome::BiomeManager::obfuscateSeed(m_region->getSeed())
            );
            return biomeManager.getBiome(pos);
        } catch (...) {}
        return nullptr;
    }

    //=========================================================================
    // World Properties
    //=========================================================================

    int64_t getSeed() const override {
        return m_region->getSeed();
    }

    bool ensureCanWrite(const core::BlockPos& pos) const override {
        return m_region->ensureCanWrite(pos);
    }

    void setCurrentlyGenerating(const std::string& description) override {
        m_region->setCurrentlyGenerating([description]() { return description; });
    }

    //=========================================================================
    // Additional Accessors
    //=========================================================================

    /**
     * Get the underlying WorldGenRegion
     */
    server::level::WorldGenRegion* getRegion() const {
        return m_region;
    }
};

} // namespace levelgen
} // namespace minecraft
