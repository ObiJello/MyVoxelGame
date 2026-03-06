#pragma once

#include "core/BlockPos.h"
#include "core/Vec3i.h"
#include "core/Direction.h"
#include "world/level/block/state/BlockState.h"
#include "levelgen/WorldGenLevel.h"
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <unordered_set>

// Reference: net/minecraft/world/level/levelgen/blockpredicates/BlockPredicate.java
// Reference: net/minecraft/world/level/levelgen/blockpredicates/*.java

namespace minecraft {
namespace levelgen {
namespace blockpredicates {

// Import Direction and related functions from core namespace
using core::Direction;
using core::Axis;
using core::getAxis;
using core::getOpposite;
using core::getStepX;
using core::getStepY;
using core::getStepZ;
using core::fromHorizontalIndex;
using core::fromIndex;
using core::rotateYClockwise;
using core::rotateYCounterClockwise;
using core::opposite;

// Use the real WorldGenLevel from levelgen namespace
using levelgen::WorldGenLevel;

// Forward declaration
class BlockPredicate;

/**
 * BlockPredicate - Base interface for block predicates
 * Reference: BlockPredicate.java
 *
 * Tests whether a block at a position satisfies some condition.
 */
class BlockPredicate {
public:
    virtual ~BlockPredicate() = default;

    /**
     * Test if the predicate is satisfied at the given position
     * Reference: BlockPredicate.java - BiPredicate<WorldGenLevel, BlockPos>
     */
    virtual bool test(const WorldGenLevel& level, const core::BlockPos& pos) const = 0;

    // ============= Static Factory Methods =============
    // Reference: BlockPredicate.java static methods

    /**
     * Create a predicate that matches all of the given predicates
     * Reference: BlockPredicate.java lines 26-36
     */
    static std::shared_ptr<BlockPredicate> allOf(const std::vector<std::shared_ptr<BlockPredicate>>& predicates);
    static std::shared_ptr<BlockPredicate> allOf(std::shared_ptr<BlockPredicate> a, std::shared_ptr<BlockPredicate> b);

    /**
     * Create a predicate that matches any of the given predicates
     * Reference: BlockPredicate.java lines 38-48
     */
    static std::shared_ptr<BlockPredicate> anyOf(const std::vector<std::shared_ptr<BlockPredicate>>& predicates);
    static std::shared_ptr<BlockPredicate> anyOf(std::shared_ptr<BlockPredicate> a, std::shared_ptr<BlockPredicate> b);

    /**
     * Create a predicate that matches specific blocks
     * Reference: BlockPredicate.java lines 50-64
     */
    static std::shared_ptr<BlockPredicate> matchesBlocks(const core::Vec3i& offset, const std::vector<std::string>& blocks);
    static std::shared_ptr<BlockPredicate> matchesBlocks(const std::vector<std::string>& blocks);
    static std::shared_ptr<BlockPredicate> matchesBlocks(const std::string& block);
    static std::shared_ptr<BlockPredicate> matchesBlocks(const core::Vec3i& offset, const std::string& block);

    /**
     * Create a predicate that matches a block tag
     * Reference: BlockPredicate.java lines 66-72
     */
    static std::shared_ptr<BlockPredicate> matchesTag(const core::Vec3i& offset, const std::string& tag);
    static std::shared_ptr<BlockPredicate> matchesTag(const std::string& tag);

    /**
     * Create a predicate that matches specific fluids
     * Reference: BlockPredicate.java lines 74-84
     */
    static std::shared_ptr<BlockPredicate> matchesFluids(const core::Vec3i& offset, const std::vector<std::string>& fluids);
    static std::shared_ptr<BlockPredicate> matchesFluids(const std::vector<std::string>& fluids);
    static std::shared_ptr<BlockPredicate> matchesFluids(const std::string& fluid);

    /**
     * Create a predicate that negates another predicate
     * Reference: BlockPredicate.java lines 86-88
     */
    static std::shared_ptr<BlockPredicate> not_(std::shared_ptr<BlockPredicate> predicate);

    /**
     * Create a predicate that matches replaceable blocks
     * Reference: BlockPredicate.java lines 90-96
     */
    static std::shared_ptr<BlockPredicate> replaceable(const core::Vec3i& offset);
    static std::shared_ptr<BlockPredicate> replaceable();

    /**
     * Create a predicate that checks if a block would survive at a position
     * Reference: BlockPredicate.java lines 98-100
     */
    static std::shared_ptr<BlockPredicate> wouldSurvive(BlockState* state, const core::Vec3i& offset);

    /**
     * Create a predicate that checks for a sturdy face
     * Reference: BlockPredicate.java lines 102-108
     */
    static std::shared_ptr<BlockPredicate> hasSturdyFace(const core::Vec3i& offset, Direction direction);
    static std::shared_ptr<BlockPredicate> hasSturdyFace(Direction direction);

    /**
     * Create a predicate that matches solid blocks
     * Reference: BlockPredicate.java lines 110-116
     */
    static std::shared_ptr<BlockPredicate> solid(const core::Vec3i& offset);
    static std::shared_ptr<BlockPredicate> solid();

    /**
     * Create a predicate that matches blocks with no fluid
     * Reference: BlockPredicate.java lines 118-124
     */
    static std::shared_ptr<BlockPredicate> noFluid();
    static std::shared_ptr<BlockPredicate> noFluid(const core::Vec3i& offset);

    /**
     * Create a predicate that checks if position is inside world bounds
     * Reference: BlockPredicate.java lines 126-128
     */
    static std::shared_ptr<BlockPredicate> insideWorld(const core::Vec3i& offset);

    /**
     * Create a predicate that always returns true
     * Reference: BlockPredicate.java lines 130-132
     */
    static std::shared_ptr<BlockPredicate> alwaysTrue();

    /**
     * Create a predicate that checks if position is unobstructed
     * Reference: BlockPredicate.java lines 134-140
     */
    static std::shared_ptr<BlockPredicate> unobstructed(const core::Vec3i& offset);
    static std::shared_ptr<BlockPredicate> unobstructed();

    // ============= Common Predicate Instances =============
    // Reference: BlockPredicate.java lines 21-22

    static std::shared_ptr<BlockPredicate> ONLY_IN_AIR_PREDICATE;
    static std::shared_ptr<BlockPredicate> ONLY_IN_AIR_OR_WATER_PREDICATE;
};

//=============================================================================
// StateTestingPredicate - Base class for predicates that test block state
// Reference: StateTestingPredicate.java
//=============================================================================

class StateTestingPredicate : public BlockPredicate {
protected:
    core::Vec3i m_offset;

public:
    explicit StateTestingPredicate(const core::Vec3i& offset) : m_offset(offset) {}

    /**
     * Test at position with offset
     * Reference: StateTestingPredicate.java lines 21-23
     */
    bool test(const WorldGenLevel& level, const core::BlockPos& pos) const override final {
        return test(level.getBlockState(pos.offset(m_offset)));
    }

protected:
    /**
     * Test the block state
     * Reference: StateTestingPredicate.java line 25
     */
    virtual bool test(BlockState* state) const = 0;
};

//=============================================================================
// CombiningPredicate - Base class for predicates that combine others
// Reference: CombiningPredicate.java
//=============================================================================

class CombiningPredicate : public BlockPredicate {
protected:
    std::vector<std::shared_ptr<BlockPredicate>> m_predicates;

public:
    explicit CombiningPredicate(const std::vector<std::shared_ptr<BlockPredicate>>& predicates)
        : m_predicates(predicates) {}
};

//=============================================================================
// AllOfPredicate - Matches if all predicates match
// Reference: AllOfPredicate.java
//=============================================================================

class AllOfPredicate : public CombiningPredicate {
public:
    explicit AllOfPredicate(const std::vector<std::shared_ptr<BlockPredicate>>& predicates)
        : CombiningPredicate(predicates) {}

    /**
     * Test all predicates
     * Reference: AllOfPredicate.java lines 15-23
     */
    bool test(const WorldGenLevel& level, const core::BlockPos& pos) const override {
        for (const auto& predicate : m_predicates) {
            if (!predicate->test(level, pos)) {
                return false;
            }
        }
        return true;
    }
};

//=============================================================================
// AnyOfPredicate - Matches if any predicate matches
// Reference: AnyOfPredicate.java
//=============================================================================

class AnyOfPredicate : public CombiningPredicate {
public:
    explicit AnyOfPredicate(const std::vector<std::shared_ptr<BlockPredicate>>& predicates)
        : CombiningPredicate(predicates) {}

    /**
     * Test any predicate
     * Reference: AnyOfPredicate.java lines 15-23
     */
    bool test(const WorldGenLevel& level, const core::BlockPos& pos) const override {
        for (const auto& predicate : m_predicates) {
            if (predicate->test(level, pos)) {
                return true;
            }
        }
        return false;
    }
};

//=============================================================================
// NotPredicate - Negates another predicate
// Reference: NotPredicate.java
//=============================================================================

class NotPredicate : public BlockPredicate {
private:
    std::shared_ptr<BlockPredicate> m_predicate;

public:
    explicit NotPredicate(std::shared_ptr<BlockPredicate> predicate)
        : m_predicate(predicate) {}

    /**
     * Test negation
     * Reference: NotPredicate.java lines 16-18
     */
    bool test(const WorldGenLevel& level, const core::BlockPos& pos) const override {
        return !m_predicate->test(level, pos);
    }
};

//=============================================================================
// TrueBlockPredicate - Always returns true
// Reference: TrueBlockPredicate.java
//=============================================================================

class TrueBlockPredicate : public BlockPredicate {
public:
    static TrueBlockPredicate INSTANCE;

    /**
     * Always returns true
     * Reference: TrueBlockPredicate.java lines 14-16
     */
    bool test(const WorldGenLevel& level, const core::BlockPos& pos) const override {
        return true;
    }
};

//=============================================================================
// MatchingBlocksPredicate - Matches specific blocks
// Reference: MatchingBlocksPredicate.java
//=============================================================================

class MatchingBlocksPredicate : public StateTestingPredicate {
private:
    std::unordered_set<std::string> m_blocks;

public:
    MatchingBlocksPredicate(const core::Vec3i& offset, const std::vector<std::string>& blocks)
        : StateTestingPredicate(offset)
        , m_blocks(blocks.begin(), blocks.end()) {}

protected:
    /**
     * Check if block matches
     * Reference: MatchingBlocksPredicate.java lines 21-23
     */
    bool test(BlockState* state) const override {
        if (!state) return false;
        return m_blocks.find(state->getIdentifier()) != m_blocks.end();
    }
};

//=============================================================================
// MatchingBlockTagPredicate - Matches blocks by tag
// Reference: MatchingBlockTagPredicate.java
//=============================================================================

class MatchingBlockTagPredicate : public StateTestingPredicate {
private:
    std::string m_tag;

public:
    MatchingBlockTagPredicate(const core::Vec3i& offset, const std::string& tag)
        : StateTestingPredicate(offset)
        , m_tag(tag) {}

protected:
    /**
     * Check if block has tag
     * Reference: MatchingBlockTagPredicate.java lines 20-22
     *
     * Note: Tag checking requires registry access. For now, we implement
     * common tags inline. A full implementation would use a tag registry.
     */
    bool test(BlockState* state) const override {
        if (!state) return false;
        const std::string& name = state->getIdentifier();

        // Implement common block tags
        if (m_tag == "minecraft:replaceable") {
            return name == "minecraft:air" ||
                   name == "minecraft:cave_air" ||
                   name == "minecraft:void_air" ||
                   name == "minecraft:water" ||
                   name == "minecraft:lava" ||
                   name == "minecraft:tall_grass" ||
                   name == "minecraft:grass" ||
                   name == "minecraft:fern" ||
                   name == "minecraft:large_fern" ||
                   name == "minecraft:seagrass" ||
                   name == "minecraft:tall_seagrass" ||
                   name == "minecraft:vine" ||
                   name == "minecraft:snow";
        }
        if (m_tag == "minecraft:base_stone_overworld") {
            return name == "minecraft:stone" ||
                   name == "minecraft:granite" ||
                   name == "minecraft:diorite" ||
                   name == "minecraft:andesite" ||
                   name == "minecraft:tuff" ||
                   name == "minecraft:deepslate";
        }
        if (m_tag == "minecraft:dirt") {
            return name == "minecraft:dirt" ||
                   name == "minecraft:grass_block" ||
                   name == "minecraft:podzol" ||
                   name == "minecraft:coarse_dirt" ||
                   name == "minecraft:mycelium" ||
                   name == "minecraft:rooted_dirt" ||
                   name == "minecraft:mud" ||
                   name == "minecraft:muddy_mangrove_roots";
        }
        if (m_tag == "minecraft:stone_ore_replaceables") {
            return name == "minecraft:stone" ||
                   name == "minecraft:granite" ||
                   name == "minecraft:diorite" ||
                   name == "minecraft:andesite" ||
                   name == "minecraft:tuff";
        }
        if (m_tag == "minecraft:deepslate_ore_replaceables") {
            return name == "minecraft:deepslate" ||
                   name == "minecraft:tuff";
        }
        if (m_tag == "minecraft:leaves") {
            return name.find("_leaves") != std::string::npos;
        }
        if (m_tag == "minecraft:logs") {
            return name.find("_log") != std::string::npos ||
                   name.find("_wood") != std::string::npos ||
                   name.find("_stem") != std::string::npos ||
                   name.find("_hyphae") != std::string::npos;
        }
        if (m_tag == "minecraft:sand") {
            return name == "minecraft:sand" || name == "minecraft:red_sand";
        }

        // Default: no match for unknown tags
        return false;
    }
};

//=============================================================================
// MatchingFluidsPredicate - Matches blocks by fluid content
// Reference: MatchingFluidsPredicate.java
//=============================================================================

class MatchingFluidsPredicate : public StateTestingPredicate {
private:
    std::unordered_set<std::string> m_fluids;

public:
    MatchingFluidsPredicate(const core::Vec3i& offset, const std::vector<std::string>& fluids)
        : StateTestingPredicate(offset)
        , m_fluids(fluids.begin(), fluids.end()) {}

protected:
    /**
     * Check if block fluid matches
     * Reference: MatchingFluidsPredicate.java lines 21-23
     */
    bool test(BlockState* state) const override {
        if (!state) return false;

        std::string name = state->getIdentifier();
        bool isFluid = state->isFluid();

        // Check if empty fluid
        if (m_fluids.find("minecraft:empty") != m_fluids.end() && !isFluid) {
            return true;
        }

        // Check for water
        if (m_fluids.find("minecraft:water") != m_fluids.end()) {
            if (name == "minecraft:water") {
                return true;
            }
            // TODO: Check waterlogged property when properties are implemented
        }

        // Check for lava
        if (m_fluids.find("minecraft:lava") != m_fluids.end()) {
            if (name == "minecraft:lava") {
                return true;
            }
        }

        return false;
    }
};

//=============================================================================
// SolidPredicate - Matches solid blocks
// Reference: SolidPredicate.java
//=============================================================================

class SolidPredicate : public StateTestingPredicate {
public:
    explicit SolidPredicate(const core::Vec3i& offset) : StateTestingPredicate(offset) {}

protected:
    /**
     * Check if block is solid
     * Reference: SolidPredicate.java lines 17-19
     */
    bool test(BlockState* state) const override {
        if (!state) return false;
        // A block is solid if it's not air, water, lava, or other non-solid blocks
        const std::string& name = state->getIdentifier();

        // Non-solid blocks
        if (name == "minecraft:air" ||
            name == "minecraft:cave_air" ||
            name == "minecraft:void_air" ||
            name == "minecraft:water" ||
            name == "minecraft:lava" ||
            name == "minecraft:fire" ||
            name == "minecraft:soul_fire" ||
            name == "minecraft:light" ||
            name == "minecraft:barrier") {
            return false;
        }

        // Passthrough blocks
        if (name == "minecraft:tall_grass" ||
            name == "minecraft:grass" ||
            name == "minecraft:fern" ||
            name == "minecraft:dead_bush" ||
            name == "minecraft:bush" ||
            name == "minecraft:seagrass" ||
            name == "minecraft:tall_seagrass" ||
            name == "minecraft:kelp" ||
            name == "minecraft:kelp_plant" ||
            name == "minecraft:vine" ||
            name == "minecraft:glow_lichen") {
            return false;
        }

        return true;
    }
};

//=============================================================================
// ReplaceablePredicate - Matches replaceable blocks
// Reference: ReplaceablePredicate.java
//=============================================================================

class ReplaceablePredicate : public StateTestingPredicate {
public:
    explicit ReplaceablePredicate(const core::Vec3i& offset) : StateTestingPredicate(offset) {}

protected:
    /**
     * Check if block is replaceable
     * Reference: ReplaceablePredicate.java lines 14-16
     */
    bool test(BlockState* state) const override {
        if (!state) return false;
        const std::string& name = state->getIdentifier();

        // Air blocks
        if (name == "minecraft:air" ||
            name == "minecraft:cave_air" ||
            name == "minecraft:void_air") {
            return true;
        }

        // Fluids
        if (name == "minecraft:water" || name == "minecraft:lava") {
            return true;
        }

        // Vegetation that can be replaced
        if (name == "minecraft:tall_grass" ||
            name == "minecraft:grass" ||
            name == "minecraft:fern" ||
            name == "minecraft:large_fern" ||
            name == "minecraft:dead_bush" ||
            name == "minecraft:bush" ||
            name == "minecraft:seagrass" ||
            name == "minecraft:tall_seagrass" ||
            name == "minecraft:kelp" ||
            name == "minecraft:kelp_plant" ||
            name == "minecraft:vine" ||
            name == "minecraft:glow_lichen" ||
            name == "minecraft:hanging_roots" ||
            name == "minecraft:snow") {
            return true;
        }

        return false;
    }
};

//=============================================================================
// WouldSurvivePredicate - Checks if a block would survive at position
// Reference: WouldSurvivePredicate.java
//=============================================================================

class WouldSurvivePredicate : public BlockPredicate {
private:
    core::Vec3i m_offset;
    BlockState* m_state;

public:
    WouldSurvivePredicate(const core::Vec3i& offset, BlockState* state)
        : m_offset(offset)
        , m_state(state) {}

    /**
     * Check if block would survive
     * Reference: WouldSurvivePredicate.java lines 20-22
     *
     * Note: Full survival checking requires world context.
     * For now, we return true for most blocks.
     */
    bool test(const WorldGenLevel& level, const core::BlockPos& pos) const override {
        core::BlockPos testPos = pos.offset(m_offset);

        // Check if within world bounds
        if (level.isOutsideBuildHeight(testPos)) {
            return false;
        }

        // Reference: WouldSurvivePredicate.java - checks BlockState.canSurvive()
        // For saplings, this checks if the block below is in BlockTags.DIRT
        // (grass_block, dirt, coarse_dirt, podzol, rooted_dirt, moss_block, etc.)
        core::BlockPos belowPos(testPos.getX(), testPos.getY() - 1, testPos.getZ());
        BlockState* below = level.getBlockState(belowPos);
        if (!below) return false;

        const std::string& name = below->getIdentifier();
        return name == "minecraft:grass_block" ||
               name == "minecraft:dirt" ||
               name == "minecraft:coarse_dirt" ||
               name == "minecraft:podzol" ||
               name == "minecraft:rooted_dirt" ||
               name == "minecraft:mycelium" ||
               name == "minecraft:moss_block" ||
               name == "minecraft:mud" ||
               name == "minecraft:muddy_mangrove_roots" ||
               name == "minecraft:farmland";
    }
};

//=============================================================================
// HasSturdyFacePredicate - Checks for sturdy face in direction
// Reference: HasSturdyFacePredicate.java
//=============================================================================

class HasSturdyFacePredicate : public BlockPredicate {
private:
    core::Vec3i m_offset;
    Direction m_direction;

public:
    HasSturdyFacePredicate(const core::Vec3i& offset, Direction direction)
        : m_offset(offset)
        , m_direction(direction) {}

    /**
     * Check for sturdy face
     * Reference: HasSturdyFacePredicate.java lines 20-23
     */
    bool test(const WorldGenLevel& level, const core::BlockPos& pos) const override {
        core::BlockPos testPos = pos.offset(m_offset);
        BlockState* state = level.getBlockState(testPos);

        // Most solid blocks have sturdy faces
        const std::string& name = state->getIdentifier();

        // Non-sturdy blocks
        if (name == "minecraft:air" ||
            name == "minecraft:cave_air" ||
            name == "minecraft:void_air" ||
            name == "minecraft:water" ||
            name == "minecraft:lava") {
            return false;
        }

        // Blocks without sturdy faces
        if (name == "minecraft:soul_sand" && m_direction == Direction::UP) {
            return false; // Soul sand has no sturdy top face
        }

        // Transparent blocks typically don't have sturdy faces
        if (name == "minecraft:glass" ||
            name.find("_glass") != std::string::npos ||
            name == "minecraft:ice" ||
            name == "minecraft:packed_ice" ||
            name == "minecraft:blue_ice") {
            return false;
        }

        // Slabs only have sturdy faces on their solid half
        if (name.find("_slab") != std::string::npos) {
            // Would need to check block properties for half=top/bottom
            // For now, assume full sturdy
            return true;
        }

        // Stairs have partial sturdy faces
        if (name.find("_stairs") != std::string::npos) {
            return true; // Simplified
        }

        return true;
    }
};

//=============================================================================
// InsideWorldBoundsPredicate - Checks if position is in world bounds
// Reference: InsideWorldBoundsPredicate.java
//=============================================================================

class InsideWorldBoundsPredicate : public BlockPredicate {
private:
    core::Vec3i m_offset;

public:
    explicit InsideWorldBoundsPredicate(const core::Vec3i& offset) : m_offset(offset) {}

    /**
     * Check if inside world bounds
     * Reference: InsideWorldBoundsPredicate.java lines 17-19
     */
    bool test(const WorldGenLevel& level, const core::BlockPos& pos) const override {
        return !level.isOutsideBuildHeight(pos.offset(m_offset));
    }
};

//=============================================================================
// UnobstructedPredicate - Checks if position is unobstructed
// Reference: UnobstructedPredicate.java
//=============================================================================

class UnobstructedPredicate : public BlockPredicate {
private:
    core::Vec3i m_offset;

public:
    explicit UnobstructedPredicate(const core::Vec3i& offset) : m_offset(offset) {}

    /**
     * Check if position is unobstructed
     * Reference: UnobstructedPredicate.java lines 18-20
     */
    bool test(const WorldGenLevel& level, const core::BlockPos& pos) const override {
        core::BlockPos testPos = pos.offset(m_offset);
        return level.isUnobstructed(testPos);
    }
};

//=============================================================================
// IChunkWorldGenLevel - Adapter from IChunk to WorldGenLevel interface
// Allows BlockPredicate::test to work with IChunk*
//=============================================================================

} // namespace blockpredicates

// Forward declare IChunk
} // namespace levelgen
} // namespace minecraft

namespace minecraft {
namespace world {
    class IChunk;
}
}

#include "world/IChunk.h"

namespace minecraft {
namespace levelgen {
namespace blockpredicates {

class IChunkWorldGenLevel : public WorldGenLevel {
private:
    ::world::IChunk* m_chunk;

public:
    explicit IChunkWorldGenLevel(::world::IChunk* chunk) : m_chunk(chunk) {}

    //=========================================================================
    // Block Access
    //=========================================================================

    BlockState* getBlockState(const core::BlockPos& pos) const override {
        if (m_chunk) {
            // Check if position is within this chunk
            auto chunkPos = m_chunk->getPos();
            int localX = pos.getX() - (chunkPos.x() * 16);
            int localZ = pos.getZ() - (chunkPos.z() * 16);

            // If within chunk bounds, use local access
            if (localX >= 0 && localX < 16 && localZ >= 0 && localZ < 16) {
                return m_chunk->getBlockState(localX, pos.getY(), localZ);
            }
        }
        return nullptr;
    }

    bool setBlock(const core::BlockPos& pos, BlockState* state, int flags) override {
        if (m_chunk) {
            auto chunkPos = m_chunk->getPos();
            int localX = pos.getX() - (chunkPos.x() * 16);
            int localZ = pos.getZ() - (chunkPos.z() * 16);

            if (localX >= 0 && localX < 16 && localZ >= 0 && localZ < 16) {
                m_chunk->setBlockState(localX, pos.getY(), localZ, state, false);
                return true;
            }
        }
        return false;
    }

    bool isStateAtPosition(const core::BlockPos& pos,
        std::function<bool(BlockState*)> predicate) const override {
        BlockState* state = getBlockState(pos);
        return state && predicate(state);
    }

    bool isFluidAtPosition(const core::BlockPos& pos,
        std::function<bool(BlockState*)> predicate) const override {
        BlockState* state = getBlockState(pos);
        return state && predicate(state);
    }

    //=========================================================================
    // Chunk Access
    //=========================================================================

    ::world::IChunk* getChunk(int chunkX, int chunkZ) override {
        // Only return chunk if coordinates match the chunk we're adapting
        if (m_chunk) {
            auto pos = m_chunk->getPos();
            if (pos.x() == chunkX && pos.z() == chunkZ) {
                return m_chunk;
            }
        }
        return nullptr;
    }

    //=========================================================================
    // Height Access
    //=========================================================================

    int getHeight(Heightmap::Types type, int x, int z) const override {
        if (m_chunk) {
            auto chunkPos = m_chunk->getPos();
            int localX = x - (chunkPos.x() * 16);
            int localZ = z - (chunkPos.z() * 16);

            if (localX >= 0 && localX < 16 && localZ >= 0 && localZ < 16) {
                return m_chunk->getHeight(static_cast<int>(type), localX, localZ);
            }
        }
        return getMinY();
    }

    int getMinY() const override {
        return m_chunk ? m_chunk->getMinY() : -64;
    }

    int getMaxY() const override {
        return m_chunk ? m_chunk->getMaxY() : 320;
    }

    //=========================================================================
    // Biome Access
    //=========================================================================

    const world::biome::Biome* getBiome(const core::BlockPos& pos) const override {
        if (m_chunk) {
            // BiomeHolder is just const Biome*
            return m_chunk->getBiome(pos);
        }
        return nullptr;
    }

    //=========================================================================
    // World Properties
    //=========================================================================

    int64_t getSeed() const override {
        // IChunk doesn't have seed access - return 0
        // This is only used for certain features that need the world seed
        // For BlockPredicate testing, seed is not needed
        return 0;
    }
};

} // namespace blockpredicates
} // namespace levelgen
} // namespace minecraft
