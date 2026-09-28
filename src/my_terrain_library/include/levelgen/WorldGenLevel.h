#pragma once

/**
 * WorldGenLevel - Interface for world generation level access
 * Reference: net/minecraft/world/level/WorldGenLevel.java
 *
 * This interface provides access to block/chunk data during world generation.
 * In Java, WorldGenLevel extends ServerLevelAccessor which extends LevelAccessor.
 * The key implementation is WorldGenRegion which provides multi-chunk access.
 *
 * Uses BlockState* for block access, matching Java's implementation.
 */

#include "core/BlockPos.h"
#include "random/XoroshiroRandomSource.h"
#include "world/ChunkPos.h"
#include "world/IChunk.h"
#include "world/level/block/state/BlockState.h"
#include "world/biome/Biome.h"
#include "levelgen/Heightmap.h"
#include "levelgen/WorldgenRandom.h"
#include <cstdio>
#include <functional>
#include <cstdint>

namespace minecraft {
namespace levelgen {

// Forward declarations
class ChunkGenerator;

/**
 * WorldGenLevel - Core interface for world generation level access
 * Reference: WorldGenLevel.java
 */
class WorldGenLevel {
public:
    virtual ~WorldGenLevel() = default;

    //=========================================================================
    // Block Access (from LevelAccessor)
    //=========================================================================

    /**
     * Get block state at position
     * Reference: LevelReader.getBlockState(BlockPos)
     */
    virtual BlockState* getBlockState(const core::BlockPos& pos) const = 0;

    virtual bool isEmptyBlock(const core::BlockPos& pos) const {
        BlockState* state = getBlockState(pos);
        bool result = state && state->isAir();
        // Parity-debug: mirrors the Java harness's WorldGenLevel proxy logging.
        if (std::FILE* trace = WorldgenRandom::s_rngTraceFile) {
            std::fprintf(trace, "CALL isEmptyBlock %d,%d,%d=%s\n",
                         pos.getX(), pos.getY(), pos.getZ(), result ? "true" : "false");
        }
        return result;
    }

    /**
     * Set block at position with update flags
     * Reference: LevelWriter.setBlock(BlockPos, BlockState, int)
     * @param flags Block update flags (19 = UPDATE_NEIGHBORS | UPDATE_CLIENTS typical for features)
     */
    virtual bool setBlock(const core::BlockPos& pos, BlockState* state, int flags) = 0;

    /**
     * Check if state at position matches predicate
     * Reference: LevelReader.isStateAtPosition(BlockPos, Predicate)
     */
    virtual bool isStateAtPosition(const core::BlockPos& pos,
        std::function<bool(BlockState*)> predicate) const = 0;

    /**
     * Check if fluid at position matches predicate
     * Reference: LevelReader.isFluidAtPosition(BlockPos, Predicate)
     */
    virtual bool isFluidAtPosition(const core::BlockPos& pos,
        std::function<bool(BlockState*)> predicate) const = 0;

    virtual bool isWaterAt(const core::BlockPos& pos) const {
        BlockState* state = getBlockState(pos);
        return state && state->hasWaterFluid();
    }

    //=========================================================================
    // Chunk Access (from ChunkSource pattern)
    //=========================================================================

    /**
     * Get chunk at chunk coordinates
     * Reference: WorldGenRegion.getChunk(int, int)
     */
    virtual ::world::IChunk* getChunk(int chunkX, int chunkZ) = 0;

    //=========================================================================
    // Height Access (from LevelHeightAccessor)
    //=========================================================================

    /**
     * Get height at position using specified heightmap type
     * Reference: WorldGenRegion.getHeight(Heightmap.Types, int, int)
     */
    virtual int getHeight(Heightmap::Types type, int x, int z) const = 0;

    /**
     * Get minimum Y coordinate
     * Reference: LevelHeightAccessor.getMinY()
     */
    virtual int getMinY() const = 0;

    /**
     * Get maximum Y coordinate
     * Reference: LevelHeightAccessor.getMaxY()
     */
    virtual int getMaxY() const = 0;

    /**
     * Whether the dimension has skylight (overworld true; nether/end false).
     * Worldgen brightness checks (e.g. MushroomBlock.canSurvive light<13)
     * resolve to 15 with skylight and 0 without during generation.
     */
    virtual bool hasSkyLight() const { return true; }

    /**
     * Check if position is outside build height
     * Reference: LevelHeightAccessor.isOutsideBuildHeight(BlockPos)
     */
    virtual bool isOutsideBuildHeight(const core::BlockPos& pos) const {
        int y = pos.getY();
        return y < getMinY() || y >= getMaxY();
    }

    /**
     * Check if position is unobstructed (no entity collision)
     * Reference: CollisionGetter.isUnobstructed - simplified for worldgen
     * In world generation, we just check if the block is air.
     */
    virtual bool isUnobstructed(const core::BlockPos& pos) const {
        BlockState* state = getBlockState(pos);
        return state && state->isAir();
    }

    //=========================================================================
    // Biome Access
    //=========================================================================

    /**
     * Get biome at position
     * Reference: BiomeManager.getBiome(BlockPos) pattern
     */
    virtual const world::biome::Biome* getBiome(const core::BlockPos& pos) const = 0;

    //=========================================================================
    // World Properties (from WorldGenLevel)
    //=========================================================================

    /**
     * Get world seed
     * Reference: WorldGenLevel.getSeed()
     */
    virtual int64_t getSeed() const = 0;

    /**
     * Get the level random source.
     * Reference: LevelAccessor.getRandom()
     */
    virtual minecraft::XoroshiroRandomSource& getRandom() = 0;

    /**
     * Check if position can be written to
     * Reference: WorldGenLevel.ensureCanWrite(BlockPos)
     * Default returns true - override for distance checking
     */
    virtual bool ensureCanWrite(const core::BlockPos& pos) const {
        return true;
    }

    /**
     * Set currently generating feature description (for debug/crash reports)
     * Reference: WorldGenLevel.setCurrentlyGenerating(Supplier<String>)
     * Default is no-op
     */
    virtual void setCurrentlyGenerating(const std::string& description) {
        // No-op by default
    }

    virtual void scheduleTick(const core::BlockPos& pos, const std::string& blockName, int delay) {
        (void)pos;
        (void)blockName;
        (void)delay;
    }

    //=========================================================================
    // Live-level hooks
    //
    // The features below were written against WorldGenRegion, but the game
    // also runs them against its LIVE world (a sapling growing into the same
    // tree worldgen places — TreeGrower.growTree). These three defaults are
    // the worldgen behaviour; a live-level adapter overrides them. None of
    // them is reached differently during generation, so the defaults keep
    // worldgen byte-identical.
    //=========================================================================

    /**
     * WorldGenRegion.hasChunkAt / LevelReader.hasChunkAt: is the chunk
     * holding `pos` accessible to this level.
     */
    virtual bool hasChunkAt(const core::BlockPos& pos) {
        return getChunk(pos.getX() >> 4, pos.getZ() >> 4) != nullptr;
    }

    /**
     * LevelWriter.destroyBlock(pos, dropResources): the cell becomes its
     * fluid (air here, WorldGenRegion keeps no drops) with flag 3. Only the
     * planted huge fungus reaches it, which worldgen never places.
     */
    virtual bool destroyBlock(const core::BlockPos& pos, bool dropResources);

    /**
     * One face of StructureTemplate.updateShapeAtEdge(level, flags, shape, …):
     * `pos` is inside the placed shape, `pos + step` outside it. A level that
     * owns real block behaviour runs BlockState.updateShape on both cells
     * itself and answers true; the default answers false and the caller
     * runs its own worldgen emulation of the step.
     */
    virtual bool updateShapeAtEdge(const core::BlockPos& pos, int stepX, int stepY, int stepZ,
                                   int flags) {
        (void)pos; (void)stepX; (void)stepY; (void)stepZ; (void)flags;
        return false;
    }

    /**
     * WorldGenerationContext.of(level).seaLevel(): the chunk generator's sea
     * level (VerticalAnchor.seaLevel() resolves against it). Stamped by
     * ChunkGenerator::applyBiomeDecoration before any feature runs.
     */
    int getSeaLevel() const { return m_seaLevel; }
    void setSeaLevel(int seaLevel) { m_seaLevel = seaLevel; }

private:
    int m_seaLevel = 63;
};

} // namespace levelgen
} // namespace minecraft
