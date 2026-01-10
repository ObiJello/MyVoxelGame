#pragma once

#include "core/BlockPos.h"
#include "world/IChunk.h"
#include "world/level/block/state/BlockState.h"
#include "random/XoroshiroRandomSource.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/structure/templatesystem/RuleTest.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/feature/treedecorators/TreeDecorator.h"
#include "levelgen/ChunkGenerator.h"
#include "util/IntProvider.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <functional>
#include <array>
#include <cmath>

// Alias for Direction in the core namespace for backwards compatibility
namespace minecraft { namespace core { using Direction = levelgen::blockpredicates::Direction; } }

// Reference: net/minecraft/world/level/levelgen/feature/Feature.java
// Reference: net/minecraft/world/level/levelgen/feature/ConfiguredFeature.java
// Reference: net/minecraft/world/level/levelgen/feature/FeaturePlaceContext.java

namespace minecraft {

// Forward declarations
namespace world {
    class WorldGenLevel;
}

namespace levelgen {

class ChunkGenerator;

//=============================================================================
// Direction helpers for Feature
//=============================================================================

/**
 * All 6 directions for neighbor checking
 * Reference: Direction.java
 */
constexpr std::array<std::tuple<int, int, int>, 6> DIRECTION_OFFSETS = {{
    {0, -1, 0},  // DOWN
    {0, 1, 0},   // UP
    {0, 0, -1},  // NORTH
    {0, 0, 1},   // SOUTH
    {-1, 0, 0},  // WEST
    {1, 0, 0}    // EAST
}};

/**
 * FeatureConfiguration - Base class for feature configuration data
 * Reference: FeatureConfiguration.java
 */
class FeatureConfiguration {
public:
    virtual ~FeatureConfiguration() = default;
};

/**
 * NoneFeatureConfiguration - Empty configuration for features with no parameters
 * Reference: NoneFeatureConfiguration.java
 */
class NoneFeatureConfiguration : public FeatureConfiguration {
public:
    static NoneFeatureConfiguration INSTANCE;
};

/**
 * FeaturePlaceContext - Context passed to feature place() method
 * Reference: FeaturePlaceContext.java
 */
template<typename FC>
class FeaturePlaceContext {
private:
    std::optional<void*> m_topFeature;
    ::world::IChunk* m_level;
    ChunkGenerator* m_chunkGenerator;
    XoroshiroRandomSource* m_random;
    core::BlockPos m_origin;
    const FC& m_config;

public:
    FeaturePlaceContext(
        std::optional<void*> topFeature,
        ::world::IChunk* level,
        ChunkGenerator* chunkGenerator,
        XoroshiroRandomSource* random,
        const core::BlockPos& origin,
        const FC& config
    )
        : m_topFeature(topFeature)
        , m_level(level)
        , m_chunkGenerator(chunkGenerator)
        , m_random(random)
        , m_origin(origin)
        , m_config(config)
    {}

    std::optional<void*> topFeature() const { return m_topFeature; }
    ::world::IChunk* level() const { return m_level; }
    ChunkGenerator* chunkGenerator() const { return m_chunkGenerator; }
    XoroshiroRandomSource& random() const { return *m_random; }
    const core::BlockPos& origin() const { return m_origin; }
    const FC& config() const { return m_config; }
};

//=============================================================================
// Feature Static Helper Methods
// Reference: Feature.java lines 141-198
//=============================================================================

/**
 * Feature static helper functions (namespace-level for C++)
 */
namespace FeatureHelpers {

/**
 * Check if a block state is stone (base_stone_overworld tag)
 * Reference: Feature.java lines 158-160
 */
inline bool isStone(BlockState* state) {
    const std::string& name = state->getIdentifier();
    return name == "minecraft:stone" ||
           name == "minecraft:granite" ||
           name == "minecraft:diorite" ||
           name == "minecraft:andesite" ||
           name == "minecraft:tuff" ||
           name == "minecraft:deepslate";
}

/**
 * Check if a block state is dirt (dirt tag)
 * Reference: Feature.java lines 162-164
 */
inline bool isDirt(BlockState* state) {
    const std::string& name = state->getIdentifier();
    return name == "minecraft:dirt" ||
           name == "minecraft:grass_block" ||
           name == "minecraft:podzol" ||
           name == "minecraft:coarse_dirt" ||
           name == "minecraft:mycelium" ||
           name == "minecraft:rooted_dirt" ||
           name == "minecraft:mud" ||
           name == "minecraft:muddy_mangrove_roots" ||
           name == "minecraft:moss_block";
}

/**
 * Check if position has grass or dirt
 * Reference: Feature.java lines 166-168
 */
template<typename LevelReader>
inline bool isGrassOrDirt(LevelReader& level, const core::BlockPos& pos) {
    return isDirt(level.getBlockState(pos));
}

/**
 * Check if any neighbor matches predicate
 * Reference: Feature.java lines 170-181
 */
template<typename BlockGetter>
inline bool checkNeighbors(
    BlockGetter blockGetter,
    const core::BlockPos& pos,
    std::function<bool(BlockState*)> predicate
) {
    core::BlockPos::MutableBlockPos neighborPos;

    for (const auto& [dx, dy, dz] : DIRECTION_OFFSETS) {
        neighborPos.set(pos.getX() + dx, pos.getY() + dy, pos.getZ() + dz);
        if (predicate(blockGetter(neighborPos))) {
            return true;
        }
    }

    return false;
}

/**
 * Check if any neighbor is air
 * Reference: Feature.java lines 183-185
 */
template<typename BlockGetter>
inline bool isAdjacentToAir(BlockGetter blockGetter, const core::BlockPos& pos) {
    return checkNeighbors(blockGetter, pos, [](BlockState* state) {
        return state->isAir();
    });
}

/**
 * Create a predicate that returns true if block is NOT in tag
 * Reference: Feature.java lines 141-143
 */
inline std::function<bool(BlockState*)> isReplaceable(const std::string& cannotReplaceTag) {
    return [cannotReplaceTag](BlockState* state) {
        // Block should be replaceable if it's NOT in the cannot-replace tag
        // For now, implement common tag checks
        const std::string& name = state->getIdentifier();

        if (cannotReplaceTag == "minecraft:features_cannot_replace") {
            // Blocks that features cannot replace
            if (name == "minecraft:bedrock" ||
                name == "minecraft:spawner" ||
                name == "minecraft:chest" ||
                name == "minecraft:end_portal_frame") {
                return false;
            }
        }

        return true;
    };
}

} // namespace FeatureHelpers

/**
 * Feature - Abstract base class for world generation features
 * Reference: Feature.java
 *
 * Template parameter FC is the configuration type
 */
template<typename FC>
class Feature {
public:
    virtual ~Feature() = default;

    /**
     * Place the feature at the given context
     * Reference: Feature.java line 152
     */
    virtual bool place(FeaturePlaceContext<FC>& context) = 0;

    /**
     * Place with explicit parameters
     * Reference: Feature.java lines 154-156
     */
    bool place(
        const FC& config,
        ::world::IChunk* level,
        ChunkGenerator* chunkGenerator,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) {
        // Check if position is writable
        // For now, always allow placement
        FeaturePlaceContext<FC> ctx(std::nullopt, level, chunkGenerator, &random, origin, config);
        return place(ctx);
    }

    //=========================================================================
    // Static helper methods
    // Reference: Feature.java lines 141-198
    //=========================================================================

    /**
     * Check if a block state is stone
     * Reference: Feature.java lines 158-160
     */
    static bool isStone(BlockState* state) {
        return FeatureHelpers::isStone(state);
    }

    /**
     * Check if a block state is dirt
     * Reference: Feature.java lines 162-164
     */
    static bool isDirt(BlockState* state) {
        return FeatureHelpers::isDirt(state);
    }

    /**
     * Check if position has grass or dirt
     * Reference: Feature.java lines 166-168
     */
    static bool isGrassOrDirt(::world::IChunk* level, const core::BlockPos& pos) {
        ::world::IBlockType* block = level->getBlockState(pos);
        if (!block) return false;
        BlockState* state = static_cast<BlockState*>(block);
        return FeatureHelpers::isDirt(state);
    }

    /**
     * Check if any neighbor is air
     * Reference: Feature.java lines 183-185
     */
    static bool isAdjacentToAir(::world::IChunk* level, const core::BlockPos& pos) {
        for (const auto& [dx, dy, dz] : DIRECTION_OFFSETS) {
            core::BlockPos neighborPos(pos.getX() + dx, pos.getY() + dy, pos.getZ() + dz);
            ::world::IBlockType* block = level->getBlockState(neighborPos);
            if (block) {
                BlockState* state = static_cast<BlockState*>(block);
                if (state->isAir()) {
                    return true;
                }
            }
        }
        return false;
    }

protected:
    /**
     * Set a block at position
     * Reference: Feature.java lines 137-139
     */
    void setBlock(::world::IChunk* level, const core::BlockPos& pos, ::world::IBlockType* state) {
        level->setBlockState(pos.getX() & 15, pos.getY(), pos.getZ() & 15, state, false);
    }

    /**
     * Safely set a block at position if predicate matches
     * Reference: Feature.java lines 145-150
     */
    void safeSetBlock(
        ::world::IChunk* level,
        const core::BlockPos& pos,
        ::world::IBlockType* state,
        std::function<bool(BlockState*)> canReplace
    ) {
        ::world::IBlockType* existingBlock = level->getBlockState(pos);
        if (existingBlock) {
            BlockState* existingState = static_cast<BlockState*>(existingBlock);
            if (canReplace(existingState)) {
                level->setBlockState(pos.getX() & 15, pos.getY(), pos.getZ() & 15, state, false);
            }
        }
    }

    /**
     * Mark positions above for post-processing
     * Reference: Feature.java lines 187-199
     */
    void markAboveForPostProcessing(::world::IChunk* level, const core::BlockPos& placePos) {
        core::BlockPos::MutableBlockPos pos(placePos.getX(), placePos.getY(), placePos.getZ());

        for (int i = 0; i < 2; ++i) {
            pos.move(0, 1, 0);
            ::world::IBlockType* block = level->getBlockState(pos);
            if (block) {
                BlockState* state = static_cast<BlockState*>(block);
                if (state->isAir()) {
                    return;
                }
            }
            level->markPosForPostprocessing(pos);
        }
    }
};

/**
 * ConfiguredFeatureBase - Non-template base class for type-erased access
 * Used by PlacedFeature which doesn't know the template parameters
 */
class ConfiguredFeature {
public:
    virtual ~ConfiguredFeature() = default;

    /**
     * Place the feature (type-erased version)
     */
    virtual bool place(
        ::world::IChunk* level,
        ChunkGenerator* chunkGenerator,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) = 0;
};

/**
 * ConfiguredFeatureImpl - A feature with its configuration
 * Reference: ConfiguredFeature.java
 */
template<typename FC, typename F>
class ConfiguredFeatureImpl : public ConfiguredFeature {
private:
    F* m_feature;
    FC m_config;

public:
    ConfiguredFeatureImpl(F* feature, const FC& config)
        : m_feature(feature)
        , m_config(config)
    {}

    /**
     * Place the feature
     * Reference: ConfiguredFeature.java lines 22-24
     */
    bool place(
        ::world::IChunk* level,
        ChunkGenerator* chunkGenerator,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin
    ) override {
        return m_feature->place(m_config, level, chunkGenerator, random, origin);
    }

    F* feature() const { return m_feature; }
    const FC& config() const { return m_config; }
};

//=============================================================================
// Concrete Feature Implementations
//=============================================================================

/**
 * NoOpFeature - Does nothing (placeholder)
 * Reference: NoOpFeature.java
 */
class NoOpFeature : public Feature<NoneFeatureConfiguration> {
public:
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        return true;
    }
};

/**
 * SimpleBlockFeature - Places a single block
 * Reference: SimpleBlockFeature.java
 */
class SimpleBlockConfiguration : public FeatureConfiguration {
public:
    ::world::IBlockType* toPlace;

    explicit SimpleBlockConfiguration(::world::IBlockType* block)
        : toPlace(block)
    {}
};

class SimpleBlockFeature : public Feature<SimpleBlockConfiguration> {
public:
    bool place(FeaturePlaceContext<SimpleBlockConfiguration>& context) override {
        const core::BlockPos& pos = context.origin();
        context.level()->setBlockState(pos.getX() & 15, pos.getY(), pos.getZ() & 15,
                                       context.config().toPlace, false);
        return true;
    }
};

/**
 * OreConfiguration - Configuration for ore placement
 * Reference: OreConfiguration.java
 */
class OreConfiguration : public FeatureConfiguration {
public:
    /**
     * TargetBlockState - Ore target with RuleTest and replacement state
     * Reference: OreConfiguration.TargetBlockState
     */
    struct TargetBlockState {
        std::shared_ptr<structure::templatesystem::RuleTest> target;
        ::world::IBlockType* state;

        TargetBlockState(std::shared_ptr<structure::templatesystem::RuleTest> t, ::world::IBlockType* s)
            : target(t), state(s) {}
    };

    std::vector<TargetBlockState> targetStates;
    int32_t size;
    float discardChanceOnAirExposure;

    OreConfiguration(
        const std::vector<TargetBlockState>& targets,
        int32_t size,
        float discardChance = 0.0f
    )
        : targetStates(targets)
        , size(size)
        , discardChanceOnAirExposure(discardChance)
    {}

    /**
     * Create target helper
     * Reference: OreConfiguration.java target()
     */
    static TargetBlockState target(
        std::shared_ptr<structure::templatesystem::RuleTest> rule,
        ::world::IBlockType* state
    ) {
        return TargetBlockState(rule, state);
    }
};

/**
 * OreFeature - Places ore veins using ellipsoid scatter
 * Reference: OreFeature.java
 *
 * The algorithm creates N spheres along a line between two endpoints,
 * with radius varying sinusoidally. It uses a BitSet to track tested
 * positions and culls overlapping spheres.
 */
class OreFeature : public Feature<OreConfiguration> {
public:
    /**
     * Place ore vein
     * Reference: OreFeature.java place() lines 23-53
     */
    bool place(FeaturePlaceContext<OreConfiguration>& context) override {
        XoroshiroRandomSource& random = context.random();
        const core::BlockPos& origin = context.origin();
        const OreConfiguration& config = context.config();
        ::world::IChunk* level = context.level();

        // Reference: OreFeature.java lines 28-42
        float dir = random.nextFloat() * static_cast<float>(M_PI);
        float spreadXY = static_cast<float>(config.size) / 8.0f;
        int32_t maxRadius = static_cast<int32_t>(std::ceil(
            (static_cast<float>(config.size) / 16.0f * 2.0f + 1.0f) / 2.0f
        ));

        double x0 = static_cast<double>(origin.getX()) + std::sin(static_cast<double>(dir)) * static_cast<double>(spreadXY);
        double x1 = static_cast<double>(origin.getX()) - std::sin(static_cast<double>(dir)) * static_cast<double>(spreadXY);
        double z0 = static_cast<double>(origin.getZ()) + std::cos(static_cast<double>(dir)) * static_cast<double>(spreadXY);
        double z1 = static_cast<double>(origin.getZ()) - std::cos(static_cast<double>(dir)) * static_cast<double>(spreadXY);
        double y0 = static_cast<double>(origin.getY() + random.nextInt(3) - 2);
        double y1 = static_cast<double>(origin.getY() + random.nextInt(3) - 2);

        int32_t xStart = origin.getX() - static_cast<int32_t>(std::ceil(spreadXY)) - maxRadius;
        int32_t yStart = origin.getY() - 2 - maxRadius;
        int32_t zStart = origin.getZ() - static_cast<int32_t>(std::ceil(spreadXY)) - maxRadius;
        int32_t sizeXZ = 2 * (static_cast<int32_t>(std::ceil(spreadXY)) + maxRadius);
        int32_t sizeY = 2 * (2 + maxRadius);

        return doPlace(level, random, config, x0, x1, z0, z1, y0, y1, xStart, yStart, zStart, sizeXZ, sizeY);
    }

protected:
    /**
     * Perform ore placement
     * Reference: OreFeature.java doPlace() lines 55-152
     */
    bool doPlace(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const OreConfiguration& config,
        double x0, double x1,
        double z0, double z1,
        double y0, double y1,
        int32_t xStart, int32_t yStart, int32_t zStart,
        int32_t sizeXZ, int32_t sizeY
    ) {
        int32_t placed = 0;
        std::vector<bool> tested(sizeXZ * sizeY * sizeXZ, false);
        core::BlockPos::MutableBlockPos orePos;
        int32_t size = config.size;

        // Pre-calculate sphere data
        // Reference: OreFeature.java lines 62-73
        std::vector<double> data(size * 4);
        for (int32_t i = 0; i < size; ++i) {
            float step = static_cast<float>(i) / static_cast<float>(size);
            double xx = x0 + (x1 - x0) * static_cast<double>(step);
            double yy = y0 + (y1 - y0) * static_cast<double>(step);
            double zz = z0 + (z1 - z0) * static_cast<double>(step);
            double ss = random.nextDouble() * static_cast<double>(size) / 16.0;
            double r = ((static_cast<double>(std::sin(static_cast<float>(M_PI) * step)) + 1.0) * ss + 1.0) / 2.0;
            data[i * 4 + 0] = xx;
            data[i * 4 + 1] = yy;
            data[i * 4 + 2] = zz;
            data[i * 4 + 3] = r;
        }

        // Cull overlapping spheres
        // Reference: OreFeature.java lines 75-93
        for (int32_t i1 = 0; i1 < size - 1; ++i1) {
            if (data[i1 * 4 + 3] <= 0.0) continue;

            for (int32_t i2 = i1 + 1; i2 < size; ++i2) {
                if (data[i2 * 4 + 3] <= 0.0) continue;

                double dx = data[i1 * 4 + 0] - data[i2 * 4 + 0];
                double dy = data[i1 * 4 + 1] - data[i2 * 4 + 1];
                double dz = data[i1 * 4 + 2] - data[i2 * 4 + 2];
                double dr = data[i1 * 4 + 3] - data[i2 * 4 + 3];

                if (dr * dr > dx * dx + dy * dy + dz * dz) {
                    if (dr > 0.0) {
                        data[i2 * 4 + 3] = -1.0;
                    } else {
                        data[i1 * 4 + 3] = -1.0;
                    }
                }
            }
        }

        // Place ore blocks
        // Reference: OreFeature.java lines 96-149
        for (int32_t i = 0; i < size; ++i) {
            double r = data[i * 4 + 3];
            if (r < 0.0) continue;

            double xx = data[i * 4 + 0];
            double yy = data[i * 4 + 1];
            double zz = data[i * 4 + 2];

            int32_t xMin = std::max(static_cast<int32_t>(std::floor(xx - r)), xStart);
            int32_t yMin = std::max(static_cast<int32_t>(std::floor(yy - r)), yStart);
            int32_t zMin = std::max(static_cast<int32_t>(std::floor(zz - r)), zStart);
            int32_t xMax = std::max(static_cast<int32_t>(std::floor(xx + r)), xMin);
            int32_t yMax = std::max(static_cast<int32_t>(std::floor(yy + r)), yMin);
            int32_t zMax = std::max(static_cast<int32_t>(std::floor(zz + r)), zMin);

            for (int32_t x = xMin; x <= xMax; ++x) {
                double xd = (static_cast<double>(x) + 0.5 - xx) / r;
                if (xd * xd >= 1.0) continue;

                for (int32_t y = yMin; y <= yMax; ++y) {
                    double yd = (static_cast<double>(y) + 0.5 - yy) / r;
                    if (xd * xd + yd * yd >= 1.0) continue;

                    for (int32_t z = zMin; z <= zMax; ++z) {
                        double zd = (static_cast<double>(z) + 0.5 - zz) / r;
                        if (xd * xd + yd * yd + zd * zd >= 1.0) continue;

                        // Check if already tested
                        int32_t bitSetIndex = (x - xStart) + (y - yStart) * sizeXZ + (z - zStart) * sizeXZ * sizeY;
                        if (bitSetIndex < 0 || bitSetIndex >= static_cast<int32_t>(tested.size())) continue;
                        if (tested[bitSetIndex]) continue;
                        tested[bitSetIndex] = true;

                        orePos.set(x, y, z);
                        ::world::IBlockType* existingBlock = level->getBlockState(orePos);
                        if (!existingBlock) continue;

                        BlockState* blockState = static_cast<BlockState*>(existingBlock);

                        // Check each target
                        for (const auto& targetState : config.targetStates) {
                            if (canPlaceOre(blockState, level, random, config, targetState, orePos)) {
                                level->setBlockState(x & 15, y, z & 15, targetState.state, false);
                                ++placed;
                                break;
                            }
                        }
                    }
                }
            }
        }

        return placed > 0;
    }

    /**
     * Check if ore can be placed at position
     * Reference: OreFeature.java canPlaceOre() lines 154-162
     */
    static bool canPlaceOre(
        BlockState* orePosState,
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const OreConfiguration& config,
        const OreConfiguration::TargetBlockState& targetState,
        const core::BlockPos& orePos
    ) {
        if (!targetState.target->test(orePosState, random)) {
            return false;
        }
        if (shouldSkipAirCheck(random, config.discardChanceOnAirExposure)) {
            return true;
        }
        return !isAdjacentToAir(level, orePos);
    }

    /**
     * Check if air check should be skipped
     * Reference: OreFeature.java shouldSkipAirCheck() lines 164-172
     */
    static bool shouldSkipAirCheck(XoroshiroRandomSource& random, float discardChanceOnAirExposure) {
        if (discardChanceOnAirExposure <= 0.0f) {
            return true;
        }
        if (discardChanceOnAirExposure >= 1.0f) {
            return false;
        }
        return random.nextFloat() >= discardChanceOnAirExposure;
    }
};

//=============================================================================
// RandomPatchConfiguration
// Reference: RandomPatchConfiguration.java
//=============================================================================

/**
 * RandomPatchConfiguration - Configuration for random patch placement
 * Reference: RandomPatchConfiguration.java
 */
class RandomPatchConfiguration : public FeatureConfiguration {
public:
    int tries;      // Number of placement attempts (default: 128)
    int xzSpread;   // Horizontal spread radius (default: 7)
    int ySpread;    // Vertical spread radius (default: 3)
    // Feature holder would go here (simplified for now)
    std::function<bool(::world::IChunk*, ChunkGenerator*, XoroshiroRandomSource&, const core::BlockPos&)> featurePlacer;

    RandomPatchConfiguration(
        int tries = 128,
        int xzSpread = 7,
        int ySpread = 3
    )
        : tries(tries)
        , xzSpread(xzSpread)
        , ySpread(ySpread)
        , featurePlacer(nullptr)
    {}
};

/**
 * RandomPatchFeature - Places features at random positions within a spread area
 * Reference: RandomPatchFeature.java
 */
class RandomPatchFeature : public Feature<RandomPatchConfiguration> {
public:
    /**
     * Place random patch
     * Reference: RandomPatchFeature.java place() lines 15-33
     */
    bool place(FeaturePlaceContext<RandomPatchConfiguration>& context) override {
        const RandomPatchConfiguration& config = context.config();
        XoroshiroRandomSource& random = context.random();
        const core::BlockPos& origin = context.origin();
        ::world::IChunk* level = context.level();
        ChunkGenerator* generator = context.chunkGenerator();

        int placed = 0;
        core::BlockPos::MutableBlockPos grassPos;
        int xzBound = config.xzSpread + 1;
        int yBound = config.ySpread + 1;

        // Reference: RandomPatchFeature.java lines 25-30
        for (int i = 0; i < config.tries; ++i) {
            int dx = random.nextInt(xzBound) - random.nextInt(xzBound);
            int dy = random.nextInt(yBound) - random.nextInt(yBound);
            int dz = random.nextInt(xzBound) - random.nextInt(xzBound);
            grassPos.setWithOffset(origin, dx, dy, dz);

            if (config.featurePlacer && config.featurePlacer(level, generator, random, grassPos)) {
                ++placed;
            }
        }

        return placed > 0;
    }
};

//=============================================================================
// SpringConfiguration
// Reference: SpringConfiguration.java
//=============================================================================

/**
 * SpringConfiguration - Configuration for spring feature placement
 * Reference: SpringConfiguration.java
 */
class SpringConfiguration : public FeatureConfiguration {
public:
    std::string fluidState;          // Fluid to place (e.g., "minecraft:water", "minecraft:lava")
    bool requiresBlockBelow;          // Require valid block below (default: true)
    int rockCount;                    // Required adjacent rock blocks (default: 4)
    int holeCount;                    // Required adjacent holes (default: 1)
    std::set<std::string> validBlocks; // Blocks that count as valid (rock)

    SpringConfiguration(
        const std::string& fluid = "minecraft:water",
        bool requiresBlockBelow = true,
        int rockCount = 4,
        int holeCount = 1,
        const std::set<std::string>& validBlocks = {"minecraft:stone", "minecraft:granite", "minecraft:diorite", "minecraft:andesite"}
    )
        : fluidState(fluid)
        , requiresBlockBelow(requiresBlockBelow)
        , rockCount(rockCount)
        , holeCount(holeCount)
        , validBlocks(validBlocks)
    {}
};

/**
 * SpringFeature - Places water/lava springs in walls
 * Reference: SpringFeature.java
 */
class SpringFeature : public Feature<SpringConfiguration> {
public:
    /**
     * Place spring
     * Reference: SpringFeature.java place() lines 14-77
     */
    bool place(FeaturePlaceContext<SpringConfiguration>& context) override {
        const SpringConfiguration& config = context.config();
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();

        // Reference: SpringFeature.java lines 18-26
        // Check block above
        ::world::IBlockType* aboveBlock = level->getBlockState(origin.above());
        if (aboveBlock) {
            std::string aboveName = aboveBlock->getIdentifier();
            if (config.validBlocks.find(aboveName) == config.validBlocks.end()) {
                return false;
            }
        } else {
            return false;
        }

        // Check block below if required
        if (config.requiresBlockBelow) {
            ::world::IBlockType* belowBlock = level->getBlockState(origin.below());
            if (belowBlock) {
                std::string belowName = belowBlock->getIdentifier();
                if (config.validBlocks.find(belowName) == config.validBlocks.end()) {
                    return false;
                }
            } else {
                return false;
            }
        }

        // Check current position
        ::world::IBlockType* currentBlock = level->getBlockState(origin);
        if (currentBlock) {
            std::string currentName = currentBlock->getIdentifier();
            BlockState* currentState = static_cast<BlockState*>(::world::MinecraftBlocks::get(currentName));
            if (!currentState->isAir() && config.validBlocks.find(currentName) == config.validBlocks.end()) {
                return false;
            }
        }

        // Reference: SpringFeature.java lines 27-47
        // Count adjacent rock blocks
        int rockCount = 0;
        auto checkRock = [&](const core::BlockPos& pos) {
            ::world::IBlockType* block = level->getBlockState(pos);
            if (block) {
                std::string name = block->getIdentifier();
                if (config.validBlocks.find(name) != config.validBlocks.end()) {
                    ++rockCount;
                }
            }
        };

        checkRock(origin.west());
        checkRock(origin.east());
        checkRock(origin.north());
        checkRock(origin.south());
        checkRock(origin.below());

        // Count adjacent holes
        int holeCount = 0;
        auto checkHole = [&](const core::BlockPos& pos) {
            ::world::IBlockType* block = level->getBlockState(pos);
            if (!block) {
                ++holeCount;
            } else {
                BlockState* state = static_cast<BlockState*>(block);
                if (state->isAir()) {
                    ++holeCount;
                }
            }
        };

        checkHole(origin.west());
        checkHole(origin.east());
        checkHole(origin.north());
        checkHole(origin.south());
        checkHole(origin.below());

        // Reference: SpringFeature.java lines 70-76
        if (rockCount == config.rockCount && holeCount == config.holeCount) {
            // Place fluid - would need fluid block type registry
            // For now, skip actual placement logic as we don't have fluid scheduling
            return true;
        }

        return false;
    }
};

//=============================================================================
// LakeConfiguration
// Reference: LakeFeature.Configuration
//=============================================================================

// Forward declaration for BlockStateProvider
namespace feature {
namespace stateproviders {
    class BlockStateProvider;
}
}

/**
 * LakeConfiguration - Configuration for lake feature
 * Reference: LakeFeature.Configuration
 */
class LakeConfiguration : public FeatureConfiguration {
public:
    std::shared_ptr<feature::stateproviders::BlockStateProvider> fluid;
    std::shared_ptr<feature::stateproviders::BlockStateProvider> barrier;

    LakeConfiguration(
        std::shared_ptr<feature::stateproviders::BlockStateProvider> fluid,
        std::shared_ptr<feature::stateproviders::BlockStateProvider> barrier
    )
        : fluid(fluid)
        , barrier(barrier)
    {}
};

/**
 * LakeFeature - Places lakes (deprecated in vanilla but still used)
 * Reference: LakeFeature.java
 */
class LakeFeature : public Feature<LakeConfiguration> {
public:
    /**
     * Place lake
     * Reference: LakeFeature.java place() lines 25-130
     */
    bool place(FeaturePlaceContext<LakeConfiguration>& context) override {
        core::BlockPos origin = context.origin();
        ::world::IChunk* level = context.level();
        XoroshiroRandomSource& random = context.random();

        // Reference: LakeFeature.java lines 30-33
        if (origin.getY() <= level->getMinY() + 4) {
            return false;
        }

        origin = origin.below(4);

        // Reference: LakeFeature.java lines 34-58
        // Create ellipsoid grid (16x16x8)
        std::array<bool, 2048> grid{};
        int spots = random.nextInt(4) + 4;

        for (int i = 0; i < spots; ++i) {
            double xr = random.nextDouble() * 6.0 + 3.0;
            double yr = random.nextDouble() * 4.0 + 2.0;
            double zr = random.nextDouble() * 6.0 + 3.0;
            double xp = random.nextDouble() * (16.0 - xr - 2.0) + 1.0 + xr / 2.0;
            double yp = random.nextDouble() * (8.0 - yr - 4.0) + 2.0 + yr / 2.0;
            double zp = random.nextDouble() * (16.0 - zr - 2.0) + 1.0 + zr / 2.0;

            for (int xx = 1; xx < 15; ++xx) {
                for (int zz = 1; zz < 15; ++zz) {
                    for (int yy = 1; yy < 7; ++yy) {
                        double xd = (static_cast<double>(xx) - xp) / (xr / 2.0);
                        double yd = (static_cast<double>(yy) - yp) / (yr / 2.0);
                        double zd = (static_cast<double>(zz) - zp) / (zr / 2.0);
                        double d = xd * xd + yd * yd + zd * zd;
                        if (d < 1.0) {
                            grid[(xx * 16 + zz) * 8 + yy] = true;
                        }
                    }
                }
            }
        }

        // Reference: LakeFeature.java lines 62-78
        // Check for liquid interference and solid ground
        for (int xx = 0; xx < 16; ++xx) {
            for (int zz = 0; zz < 16; ++zz) {
                for (int yy = 0; yy < 8; ++yy) {
                    bool check = !grid[(xx * 16 + zz) * 8 + yy] &&
                        (xx < 15 && grid[((xx + 1) * 16 + zz) * 8 + yy] ||
                         xx > 0 && grid[((xx - 1) * 16 + zz) * 8 + yy] ||
                         zz < 15 && grid[(xx * 16 + zz + 1) * 8 + yy] ||
                         zz > 0 && grid[(xx * 16 + (zz - 1)) * 8 + yy] ||
                         yy < 7 && grid[(xx * 16 + zz) * 8 + yy + 1] ||
                         yy > 0 && grid[(xx * 16 + zz) * 8 + (yy - 1)]);

                    if (check) {
                        core::BlockPos checkPos = origin.offset(xx, yy, zz);
                        ::world::IBlockType* block = level->getBlockState(checkPos);
                        if (block) {
                            BlockState* blockState = static_cast<BlockState*>(block);
                            // Reference: LakeFeature.java lines 68-74
                            if (yy >= 4 && blockState->isFluid()) {
                                return false;
                            }
                            if (yy < 4 && !!blockState->isAir()) {
                                return false;
                            }
                        }
                    }
                }
            }
        }

        // Reference: LakeFeature.java lines 80-96
        // Place fluid blocks
        for (int xx = 0; xx < 16; ++xx) {
            for (int zz = 0; zz < 16; ++zz) {
                for (int yy = 0; yy < 8; ++yy) {
                    if (grid[(xx * 16 + zz) * 8 + yy]) {
                        core::BlockPos placePos = origin.offset(xx, yy, zz);
                        ::world::IBlockType* existingBlock = level->getBlockState(placePos);
                        if (existingBlock && canReplaceBlock(static_cast<BlockState*>(existingBlock))) {
                            bool placeAir = yy >= 4;
                            // Would need to get the fluid state from config.fluid
                            // For now, we just mark that placement succeeded
                            if (placeAir) {
                                markAboveForPostProcessing(level, placePos);
                            }
                        }
                    }
                }
            }
        }

        return true;
    }

private:
    /**
     * Check if block can be replaced
     * Reference: LakeFeature.java canReplaceBlock() lines 133-135
     */
    static bool canReplaceBlock(BlockState* state) {
        const std::string& name = state->getIdentifier();
        // Reference: features_cannot_replace tag
        if (name == "minecraft:bedrock" ||
            name == "minecraft:spawner" ||
            name == "minecraft:chest" ||
            name == "minecraft:end_portal_frame") {
            return false;
        }
        return true;
    }
};

//=============================================================================
// VegetationPatchConfiguration
// Reference: VegetationPatchConfiguration.java
//=============================================================================

// CaveSurface is defined in SurfaceRules.h

namespace CaveSurfaceHelper {
    inline int getDirection(CaveSurface surface) {
        return surface == CaveSurface::CEILING ? -1 : 1;
    }

    inline core::Direction getDirectionEnum(CaveSurface surface) {
        return surface == CaveSurface::CEILING ? core::Direction::UP : core::Direction::DOWN;
    }
}

/**
 * VegetationPatchConfiguration - Configuration for vegetation patch placement
 * Reference: VegetationPatchConfiguration.java
 */
class VegetationPatchConfiguration : public FeatureConfiguration {
public:
    std::string replaceable;     // Block tag that can be replaced
    std::shared_ptr<feature::stateproviders::BlockStateProvider> groundState;
    CaveSurface surface;
    std::shared_ptr<util::IntProvider> depth;
    float extraBottomBlockChance;
    int verticalRange;
    float vegetationChance;
    std::shared_ptr<util::IntProvider> xzRadius;
    float extraEdgeColumnChance;
    // Vegetation feature holder would go here
    std::function<bool(::world::IChunk*, ChunkGenerator*, XoroshiroRandomSource&, const core::BlockPos&)> vegetationPlacer;

    VegetationPatchConfiguration(
        const std::string& replaceable,
        std::shared_ptr<feature::stateproviders::BlockStateProvider> groundState,
        CaveSurface surface,
        std::shared_ptr<util::IntProvider> depth,
        float extraBottomBlockChance,
        int verticalRange,
        float vegetationChance,
        std::shared_ptr<util::IntProvider> xzRadius,
        float extraEdgeColumnChance
    )
        : replaceable(replaceable)
        , groundState(groundState)
        , surface(surface)
        , depth(depth)
        , extraBottomBlockChance(extraBottomBlockChance)
        , verticalRange(verticalRange)
        , vegetationChance(vegetationChance)
        , xzRadius(xzRadius)
        , extraEdgeColumnChance(extraEdgeColumnChance)
        , vegetationPlacer(nullptr)
    {}
};

/**
 * VegetationPatchFeature - Places ground patches with vegetation
 * Reference: VegetationPatchFeature.java
 */
class VegetationPatchFeature : public Feature<VegetationPatchConfiguration> {
public:
    /**
     * Place vegetation patch
     * Reference: VegetationPatchFeature.java place() lines 22-33
     */
    bool place(FeaturePlaceContext<VegetationPatchConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const VegetationPatchConfiguration& config = context.config();
        XoroshiroRandomSource& random = context.random();
        const core::BlockPos& origin = context.origin();

        // Sample radii
        int xRadius = config.xzRadius->sample(random) + 1;
        int zRadius = config.xzRadius->sample(random) + 1;

        // Place ground patch and collect surface positions
        std::set<core::BlockPos> surface = placeGroundPatch(level, config, random, origin, xRadius, zRadius);

        // Distribute vegetation
        distributeVegetation(context, level, config, random, surface, xRadius, zRadius);

        return !surface.empty();
    }

protected:
    /**
     * Place ground patch
     * Reference: VegetationPatchFeature.java placeGroundPatch() lines 35-76
     */
    std::set<core::BlockPos> placeGroundPatch(
        ::world::IChunk* level,
        const VegetationPatchConfiguration& config,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin,
        int xRadius,
        int zRadius
    ) {
        core::BlockPos::MutableBlockPos pos(origin.getX(), origin.getY(), origin.getZ());
        core::BlockPos::MutableBlockPos belowPos(origin.getX(), origin.getY(), origin.getZ());
        int inwards = CaveSurfaceHelper::getDirection(config.surface);
        int outwards = -inwards;
        std::set<core::BlockPos> surface;

        for (int dx = -xRadius; dx <= xRadius; ++dx) {
            bool isXEdge = dx == -xRadius || dx == xRadius;

            for (int dz = -zRadius; dz <= zRadius; ++dz) {
                bool isZEdge = dz == -zRadius || dz == zRadius;
                bool isEdge = isXEdge || isZEdge;
                bool isCorner = isXEdge && isZEdge;
                bool isEdgeButNotCorner = isEdge && !isCorner;

                // Reference: VegetationPatchFeature.java line 50
                if (isCorner) continue;
                if (isEdgeButNotCorner && config.extraEdgeColumnChance > 0.0f && random.nextFloat() > config.extraEdgeColumnChance) {
                    continue;
                }

                pos.setWithOffset(origin, dx, 0, dz);

                // Move through air
                for (int offset = 0; offset < config.verticalRange; ++offset) {
                    ::world::IBlockType* block = level->getBlockState(pos);
                    if (!block) break;
                    BlockState* state = static_cast<BlockState*>(block);
                    if (!state->isAir()) break;
                    pos.move(0, inwards, 0);
                }

                // Move through solid
                for (int offset = 0; offset < config.verticalRange; ++offset) {
                    ::world::IBlockType* block = level->getBlockState(pos);
                    if (!block) break;
                    BlockState* state = static_cast<BlockState*>(block);
                    if (state->isAir()) break;
                    pos.move(0, outwards, 0);
                }

                // Check placement conditions
                belowPos.setWithOffset(pos, 0, inwards, 0);
                ::world::IBlockType* belowBlock = level->getBlockState(belowPos);
                ::world::IBlockType* posBlock = level->getBlockState(pos);

                if (posBlock && belowBlock) {
                    BlockState* posState = static_cast<BlockState*>(posBlock);
                    BlockState* belowState = static_cast<BlockState*>(belowBlock);

                    if (posState->isAir() && !belowState->isAir()) {
                        int depth = config.depth->sample(random);
                        if (config.extraBottomBlockChance > 0.0f && random.nextFloat() < config.extraBottomBlockChance) {
                            ++depth;
                        }

                        core::BlockPos groundPos(belowPos.getX(), belowPos.getY(), belowPos.getZ());
                        bool groundPlaced = placeGround(level, config, random, belowPos, depth);
                        if (groundPlaced) {
                            surface.insert(groundPos);
                        }
                    }
                }
            }
        }

        return surface;
    }

    /**
     * Distribute vegetation on surface
     * Reference: VegetationPatchFeature.java distributeVegetation() lines 78-85
     */
    void distributeVegetation(
        FeaturePlaceContext<VegetationPatchConfiguration>& context,
        ::world::IChunk* level,
        const VegetationPatchConfiguration& config,
        XoroshiroRandomSource& random,
        const std::set<core::BlockPos>& surface,
        int xRadius,
        int zRadius
    ) {
        for (const auto& surfacePos : surface) {
            if (config.vegetationChance > 0.0f && random.nextFloat() < config.vegetationChance) {
                placeVegetation(level, config, context.chunkGenerator(), random, surfacePos);
            }
        }
    }

    /**
     * Place vegetation at position
     * Reference: VegetationPatchFeature.java placeVegetation() lines 87-89
     */
    bool placeVegetation(
        ::world::IChunk* level,
        const VegetationPatchConfiguration& config,
        ChunkGenerator* generator,
        XoroshiroRandomSource& random,
        const core::BlockPos& vegetationPos
    ) {
        int outwards = -CaveSurfaceHelper::getDirection(config.surface);
        core::BlockPos placePos = vegetationPos.offset(0, outwards, 0);

        if (config.vegetationPlacer) {
            return config.vegetationPlacer(level, generator, random, placePos);
        }
        return false;
    }

    /**
     * Place ground blocks
     * Reference: VegetationPatchFeature.java placeGround() lines 91-106
     */
    bool placeGround(
        ::world::IChunk* level,
        const VegetationPatchConfiguration& config,
        XoroshiroRandomSource& random,
        core::BlockPos::MutableBlockPos& belowPos,
        int depth
    ) {
        int inwards = CaveSurfaceHelper::getDirection(config.surface);

        for (int i = 0; i < depth; ++i) {
            if (config.groundState) {
                BlockState* stateToPlace = config.groundState->getState(random, belowPos);
                ::world::IBlockType* belowBlock = level->getBlockState(belowPos);

                if (belowBlock) {
                    BlockState* belowState = static_cast<BlockState*>(belowBlock);
                    if (stateToPlace->getIdentifier() != belowState->getIdentifier()) {
                        // Would check replaceable tag here
                        // For now, always allow placement
                        // level->setBlockState(...) would go here
                        belowPos.move(0, inwards, 0);
                    }
                }
            }
        }

        return true;
    }
};

//=============================================================================
// DiskConfiguration
// Reference: DiskConfiguration.java
//=============================================================================

/**
 * RuleBasedBlockStateProvider - Provides block states based on rules
 * Reference: RuleBasedBlockStateProvider.java (simplified)
 */
class RuleBasedBlockStateProvider {
public:
    std::shared_ptr<feature::stateproviders::BlockStateProvider> fallback;
    // Rules would go here for full implementation

    explicit RuleBasedBlockStateProvider(
        std::shared_ptr<feature::stateproviders::BlockStateProvider> fallback
    )
        : fallback(fallback)
    {}

    BlockState* getState(::world::IChunk* level, XoroshiroRandomSource& random, const core::BlockPos& pos) const {
        if (fallback) {
            return fallback->getState(random, pos);
        }
        return static_cast<BlockState*>(::world::MinecraftBlocks::AIR());
    }
};

/**
 * DiskConfiguration - Configuration for disk feature
 * Reference: DiskConfiguration.java
 */
class DiskConfiguration : public FeatureConfiguration {
public:
    std::shared_ptr<RuleBasedBlockStateProvider> stateProvider;
    std::shared_ptr<blockpredicates::BlockPredicate> target;
    std::shared_ptr<util::IntProvider> radius;
    int halfHeight;

    DiskConfiguration(
        std::shared_ptr<RuleBasedBlockStateProvider> stateProvider,
        std::shared_ptr<blockpredicates::BlockPredicate> target,
        std::shared_ptr<util::IntProvider> radius,
        int halfHeight
    )
        : stateProvider(stateProvider)
        , target(target)
        , radius(radius)
        , halfHeight(halfHeight)
    {}
};

/**
 * DiskFeature - Places disk-shaped patches
 * Reference: DiskFeature.java
 */
class DiskFeature : public Feature<DiskConfiguration> {
public:
    /**
     * Place disk
     * Reference: DiskFeature.java place() lines 15-36
     */
    bool place(FeaturePlaceContext<DiskConfiguration>& context) override {
        const DiskConfiguration& config = context.config();
        const core::BlockPos& origin = context.origin();
        ::world::IChunk* level = context.level();
        XoroshiroRandomSource& random = context.random();

        bool placedAny = false;
        int originY = origin.getY();
        int top = originY + config.halfHeight;
        int bottom = originY - config.halfHeight - 1;
        int r = config.radius->sample(random);
        core::BlockPos::MutableBlockPos mutablePos;

        // Reference: DiskFeature.java lines 27-33
        for (int x = origin.getX() - r; x <= origin.getX() + r; ++x) {
            for (int z = origin.getZ() - r; z <= origin.getZ() + r; ++z) {
                int xd = x - origin.getX();
                int zd = z - origin.getZ();
                if (xd * xd + zd * zd <= r * r) {
                    mutablePos.set(x, 0, z);
                    placedAny |= placeColumn(config, level, random, top, bottom, mutablePos);
                }
            }
        }

        return placedAny;
    }

protected:
    /**
     * Place column of disk
     * Reference: DiskFeature.java placeColumn() lines 38-58
     */
    bool placeColumn(
        const DiskConfiguration& config,
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        int top,
        int bottom,
        core::BlockPos::MutableBlockPos& pos
    ) {
        bool placedAny = false;
        bool placedAbove = false;

        for (int y = top; y > bottom; --y) {
            pos.setY(y);

            // Check if target predicate matches
            blockpredicates::IChunkWorldGenLevel levelAdapter(level);
            if (config.target && config.target->test(levelAdapter, pos)) {
                if (config.stateProvider) {
                    BlockState* state = config.stateProvider->getState(level, random, pos);
                    // Would place block here
                    // level->setBlockState(...)

                    if (!placedAbove) {
                        markAboveForPostProcessing(level, pos);
                    }

                    placedAny = true;
                    placedAbove = true;
                }
            } else {
                placedAbove = false;
            }
        }

        return placedAny;
    }
};

//=============================================================================
// BlockColumnConfiguration
// Reference: BlockColumnConfiguration.java
//=============================================================================

/**
 * BlockColumnConfiguration - Configuration for block columns
 * Reference: BlockColumnConfiguration.java
 */
class BlockColumnConfiguration : public FeatureConfiguration {
public:
    /**
     * Layer - A single layer in the column
     * Reference: BlockColumnConfiguration.Layer
     */
    struct Layer {
        std::shared_ptr<util::IntProvider> height;
        std::shared_ptr<feature::stateproviders::BlockStateProvider> state;

        Layer(
            std::shared_ptr<util::IntProvider> height,
            std::shared_ptr<feature::stateproviders::BlockStateProvider> state
        )
            : height(height)
            , state(state)
        {}
    };

    std::vector<Layer> layers;
    core::Direction direction;
    std::shared_ptr<blockpredicates::BlockPredicate> allowedPlacement;
    bool prioritizeTip;

    BlockColumnConfiguration(
        const std::vector<Layer>& layers,
        core::Direction direction,
        std::shared_ptr<blockpredicates::BlockPredicate> allowedPlacement,
        bool prioritizeTip
    )
        : layers(layers)
        , direction(direction)
        , allowedPlacement(allowedPlacement)
        , prioritizeTip(prioritizeTip)
    {}

    static Layer layer(std::shared_ptr<util::IntProvider> height, std::shared_ptr<feature::stateproviders::BlockStateProvider> state) {
        return Layer(height, state);
    }

    static BlockColumnConfiguration simple(std::shared_ptr<util::IntProvider> height, std::shared_ptr<feature::stateproviders::BlockStateProvider> state) {
        return BlockColumnConfiguration(
            {layer(height, state)},
            core::Direction::UP,
            nullptr,
            false
        );
    }
};

/**
 * BlockColumnFeature - Places columns of blocks
 * Reference: BlockColumnFeature.java
 */
class BlockColumnFeature : public Feature<BlockColumnConfiguration> {
public:
    /**
     * Place column
     * Reference: BlockColumnFeature.java place() lines 14-55
     */
    bool place(FeaturePlaceContext<BlockColumnConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const BlockColumnConfiguration& config = context.config();
        XoroshiroRandomSource& random = context.random();

        size_t layerCount = config.layers.size();
        std::vector<int> layerHeights(layerCount);
        int totalHeight = 0;

        // Reference: BlockColumnFeature.java lines 22-25
        for (size_t i = 0; i < layerCount; ++i) {
            layerHeights[i] = config.layers[i].height->sample(random);
            totalHeight += layerHeights[i];
        }

        if (totalHeight == 0) {
            return false;
        }

        core::BlockPos::MutableBlockPos placePos(context.origin().getX(), context.origin().getY(), context.origin().getZ());

        // Get direction step
        int dx = blockpredicates::getStepX(config.direction);
        int dy = blockpredicates::getStepY(config.direction);
        int dz = blockpredicates::getStepZ(config.direction);

        core::BlockPos::MutableBlockPos nextPos(placePos.getX() + dx, placePos.getY() + dy, placePos.getZ() + dz);

        // Reference: BlockColumnFeature.java lines 33-40
        blockpredicates::IChunkWorldGenLevel levelAdapter(level);
        for (int y = 0; y < totalHeight; ++y) {
            if (config.allowedPlacement && !config.allowedPlacement->test(levelAdapter, nextPos)) {
                truncate(layerHeights.data(), layerCount, totalHeight, y, config.prioritizeTip);
                break;
            }
            nextPos.move(dx, dy, dz);
        }

        // Reference: BlockColumnFeature.java lines 42-52
        for (size_t i = 0; i < layerCount; ++i) {
            int count = layerHeights[i];
            if (count != 0) {
                const BlockColumnConfiguration::Layer& layer = config.layers[i];

                for (int y = 0; y < count; ++y) {
                    if (layer.state) {
                        BlockState* state = layer.state->getState(random, placePos);
                        // Would place block here
                        // level->setBlockState(...)
                    }
                    placePos.move(dx, dy, dz);
                }
            }
        }

        return true;
    }

private:
    /**
     * Truncate layers to fit
     * Reference: BlockColumnFeature.java truncate() lines 58-71
     */
    static void truncate(int* layerHeights, size_t layerCount, int totalHeight, int newHeight, bool prioritizeTip) {
        int amountToRemove = totalHeight - newHeight;
        int direction = prioritizeTip ? 1 : -1;
        int start = prioritizeTip ? 0 : static_cast<int>(layerCount) - 1;
        int end = prioritizeTip ? static_cast<int>(layerCount) : -1;

        for (int i = start; i != end && amountToRemove > 0; i += direction) {
            int thisLayer = layerHeights[i];
            int toRemoveFromLayer = std::min(thisLayer, amountToRemove);
            amountToRemove -= toRemoveFromLayer;
            layerHeights[i] -= toRemoveFromLayer;
        }
    }
};

//=============================================================================
// IceSpikeFeature
// Reference: IceSpikeFeature.java
//=============================================================================

/**
 * IceSpikeFeature - Generates ice spikes in frozen biomes
 * Reference: IceSpikeFeature.java
 */
class IceSpikeFeature : public Feature<NoneFeatureConfiguration> {
public:
    /**
     * Place ice spike
     * Reference: IceSpikeFeature.java place() lines 17-94
     */
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        core::BlockPos origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        ::world::IChunk* level = context.level();

        // Reference: IceSpikeFeature.java lines 22-24
        // Move down until not empty
        while (true) {
            ::world::IBlockType* block = level->getBlockState(origin);
            if (!block) break;
            BlockState* state = static_cast<BlockState*>(block);
            if (!state->isAir() || origin.getY() <= level->getMinY() + 2) break;
            origin = origin.below();
        }

        // Check for snow block
        ::world::IBlockType* groundBlock = level->getBlockState(origin);
        if (!groundBlock) return false;
        BlockState* groundState = static_cast<BlockState*>(groundBlock);
        if (groundState->getIdentifier() != "minecraft:snow_block") {
            return false;
        }

        // Reference: IceSpikeFeature.java lines 28-33
        origin = origin.above(random.nextInt(4));
        int height = random.nextInt(4) + 7;
        int width = height / 4 + random.nextInt(2);

        if (width > 1 && random.nextInt(60) == 0) {
            origin = origin.above(10 + random.nextInt(30));
        }

        // Reference: IceSpikeFeature.java lines 35-58
        for (int yOff = 0; yOff < height; ++yOff) {
            float scale = (1.0f - static_cast<float>(yOff) / static_cast<float>(height)) * static_cast<float>(width);
            int newWidth = static_cast<int>(std::ceil(scale));

            for (int xo = -newWidth; xo <= newWidth; ++xo) {
                float dx = static_cast<float>(std::abs(xo)) - 0.25f;

                for (int zo = -newWidth; zo <= newWidth; ++zo) {
                    float dz = static_cast<float>(std::abs(zo)) - 0.25f;

                    // Check placement conditions
                    bool shouldPlace = (xo == 0 && zo == 0) ||
                        (dx * dx + dz * dz <= scale * scale);
                    bool notEdgeOrRandom = (xo != -newWidth && xo != newWidth && zo != -newWidth && zo != newWidth) ||
                        random.nextFloat() <= 0.75f;

                    if (shouldPlace && notEdgeOrRandom) {
                        core::BlockPos placePos = origin.offset(xo, yOff, zo);
                        ::world::IBlockType* targetBlock = level->getBlockState(placePos);
                        if (targetBlock) {
                            BlockState* targetState = static_cast<BlockState*>(targetBlock);
                            if (targetState->isAir() || isDirt(targetState) ||
                                targetState->getIdentifier() == "minecraft:snow_block" || targetState->getIdentifier() == "minecraft:ice") {
                                // Place packed ice
                                // level->setBlockState(...)
                            }
                        }

                        // Mirror below if applicable
                        if (yOff != 0 && newWidth > 1) {
                            core::BlockPos belowPos = origin.offset(xo, -yOff, zo);
                            ::world::IBlockType* belowBlock = level->getBlockState(belowPos);
                            if (belowBlock) {
                                BlockState* belowState = static_cast<BlockState*>(belowBlock);
                                if (belowState->isAir() || isDirt(belowState) ||
                                    belowState->getIdentifier() == "minecraft:snow_block" || belowState->getIdentifier() == "minecraft:ice") {
                                    // Place packed ice
                                    // level->setBlockState(...)
                                }
                            }
                        }
                    }
                }
            }
        }

        // Reference: IceSpikeFeature.java lines 61-91
        // Place pillar beneath
        int pillarWidth = std::max(0, std::min(width - 1, 1));

        for (int xo = -pillarWidth; xo <= pillarWidth; ++xo) {
            for (int zo = -pillarWidth; zo <= pillarWidth; ++zo) {
                core::BlockPos iceBlock = origin.offset(xo, -1, zo);
                int runLength = 50;

                if (std::abs(xo) == 1 && std::abs(zo) == 1) {
                    runLength = random.nextInt(5);
                }

                while (iceBlock.getY() > 50) {
                    ::world::IBlockType* block = level->getBlockState(iceBlock);
                    if (!block) break;
                    BlockState* state = static_cast<BlockState*>(block);

                    if (!state->isAir() && !isDirt(state) &&
                        state->getIdentifier() != "minecraft:snow_block" && state->getIdentifier() != "minecraft:ice" &&
                        state->getIdentifier() != "minecraft:packed_ice") {
                        break;
                    }

                    // Place packed ice
                    // level->setBlockState(...)
                    iceBlock = iceBlock.below();
                    --runLength;

                    if (runLength <= 0) {
                        iceBlock = iceBlock.below(random.nextInt(5) + 1);
                        runLength = random.nextInt(5);
                    }
                }
            }
        }

        return true;
    }
};

//=============================================================================
// GlowstoneFeature
// Reference: GlowstoneFeature.java
//=============================================================================

/**
 * GlowstoneFeature - Generates glowstone blobs in the Nether
 * Reference: GlowstoneFeature.java
 */
class GlowstoneFeature : public Feature<NoneFeatureConfiguration> {
public:
    /**
     * Place glowstone blob
     * Reference: GlowstoneFeature.java place() lines 17-54
     */
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();

        // Reference: GlowstoneFeature.java lines 21-22
        ::world::IBlockType* originBlock = level->getBlockState(origin);
        if (originBlock) {
            BlockState* originState = static_cast<BlockState*>(originBlock);
            if (!originState->isAir()) {
                return false;
            }
        } else {
            return false;
        }

        // Reference: GlowstoneFeature.java lines 24-27
        ::world::IBlockType* aboveBlock = level->getBlockState(origin.above());
        if (!aboveBlock) return false;
        BlockState* aboveState = static_cast<BlockState*>(aboveBlock);
        if (aboveState->getIdentifier() != "minecraft:netherrack" &&
            aboveState->getIdentifier() != "minecraft:basalt" &&
            aboveState->getIdentifier() != "minecraft:blackstone") {
            return false;
        }

        // Place initial glowstone
        // level->setBlockState(origin, "minecraft:glowstone")

        // Reference: GlowstoneFeature.java lines 30-49
        for (int i = 0; i < 1500; ++i) {
            int dx = random.nextInt(8) - random.nextInt(8);
            int dy = -random.nextInt(12);
            int dz = random.nextInt(8) - random.nextInt(8);
            core::BlockPos placePos = origin.offset(dx, dy, dz);

            ::world::IBlockType* block = level->getBlockState(placePos);
            if (!block) continue;
            BlockState* blockState = static_cast<BlockState*>(block);

            if (blockState->isAir()) {
                int neighbours = 0;

                // Check all 6 directions
                for (const auto& [dirX, dirY, dirZ] : DIRECTION_OFFSETS) {
                    core::BlockPos neighborPos = placePos.offset(dirX, dirY, dirZ);
                    ::world::IBlockType* neighborBlock = level->getBlockState(neighborPos);
                    if (neighborBlock) {
                        BlockState* neighborState = static_cast<BlockState*>(neighborBlock);
                        if (neighborState->getIdentifier() == "minecraft:glowstone") {
                            ++neighbours;
                        }
                    }

                    if (neighbours > 1) {
                        break;
                    }
                }

                if (neighbours == 1) {
                    // Place glowstone
                    // level->setBlockState(placePos, "minecraft:glowstone")
                }
            }
        }

        return true;
    }
};

//=============================================================================
// GeodeConfiguration and related settings
// Reference: GeodeConfiguration.java, GeodeBlockSettings.java, etc.
//=============================================================================

/**
 * GeodeLayerSettings - Layer thickness settings for geode
 * Reference: GeodeLayerSettings.java
 */
struct GeodeLayerSettings {
    double filling;
    double innerLayer;
    double middleLayer;
    double outerLayer;

    GeodeLayerSettings(
        double filling = 1.7,
        double innerLayer = 2.2,
        double middleLayer = 3.2,
        double outerLayer = 4.2
    )
        : filling(filling)
        , innerLayer(innerLayer)
        , middleLayer(middleLayer)
        , outerLayer(outerLayer)
    {}
};

/**
 * GeodeCrackSettings - Crack generation settings for geode
 * Reference: GeodeCrackSettings.java
 */
struct GeodeCrackSettings {
    double generateCrackChance;
    double baseCrackSize;
    int crackPointOffset;

    GeodeCrackSettings(
        double generateCrackChance = 1.0,
        double baseCrackSize = 2.0,
        int crackPointOffset = 2
    )
        : generateCrackChance(generateCrackChance)
        , baseCrackSize(baseCrackSize)
        , crackPointOffset(crackPointOffset)
    {}
};

/**
 * GeodeBlockSettings - Block provider settings for geode
 * Reference: GeodeBlockSettings.java
 */
struct GeodeBlockSettings {
    std::shared_ptr<feature::stateproviders::BlockStateProvider> fillingProvider;
    std::shared_ptr<feature::stateproviders::BlockStateProvider> innerLayerProvider;
    std::shared_ptr<feature::stateproviders::BlockStateProvider> alternateInnerLayerProvider;
    std::shared_ptr<feature::stateproviders::BlockStateProvider> middleLayerProvider;
    std::shared_ptr<feature::stateproviders::BlockStateProvider> outerLayerProvider;
    std::vector<BlockState*> innerPlacements;
    std::string cannotReplace;  // Block tag
    std::string invalidBlocks;  // Block tag

    GeodeBlockSettings() = default;
};

/**
 * GeodeConfiguration - Configuration for geode feature
 * Reference: GeodeConfiguration.java
 */
class GeodeConfiguration : public FeatureConfiguration {
public:
    GeodeBlockSettings geodeBlockSettings;
    GeodeLayerSettings geodeLayerSettings;
    GeodeCrackSettings geodeCrackSettings;
    double usePotentialPlacementsChance;
    double useAlternateLayer0Chance;
    bool placementsRequireLayer0Alternate;
    std::shared_ptr<util::IntProvider> outerWallDistance;
    std::shared_ptr<util::IntProvider> distributionPoints;
    std::shared_ptr<util::IntProvider> pointOffset;
    int minGenOffset;
    int maxGenOffset;
    double noiseMultiplier;
    int invalidBlocksThreshold;

    GeodeConfiguration(
        const GeodeBlockSettings& blockSettings,
        const GeodeLayerSettings& layerSettings,
        const GeodeCrackSettings& crackSettings,
        double usePotentialPlacementsChance = 0.35,
        double useAlternateLayer0Chance = 0.0,
        bool placementsRequireLayer0Alternate = true,
        std::shared_ptr<util::IntProvider> outerWallDistance = nullptr,
        std::shared_ptr<util::IntProvider> distributionPoints = nullptr,
        std::shared_ptr<util::IntProvider> pointOffset = nullptr,
        int minGenOffset = -16,
        int maxGenOffset = 16,
        double noiseMultiplier = 0.05,
        int invalidBlocksThreshold = 1
    )
        : geodeBlockSettings(blockSettings)
        , geodeLayerSettings(layerSettings)
        , geodeCrackSettings(crackSettings)
        , usePotentialPlacementsChance(usePotentialPlacementsChance)
        , useAlternateLayer0Chance(useAlternateLayer0Chance)
        , placementsRequireLayer0Alternate(placementsRequireLayer0Alternate)
        , outerWallDistance(outerWallDistance)
        , distributionPoints(distributionPoints)
        , pointOffset(pointOffset)
        , minGenOffset(minGenOffset)
        , maxGenOffset(maxGenOffset)
        , noiseMultiplier(noiseMultiplier)
        , invalidBlocksThreshold(invalidBlocksThreshold)
    {}
};

/**
 * GeodeFeature - Generates amethyst geodes
 * Reference: GeodeFeature.java
 */
class GeodeFeature : public Feature<GeodeConfiguration> {
public:
    /**
     * Place geode
     * Reference: GeodeFeature.java place() lines 35-169
     */
    bool place(FeaturePlaceContext<GeodeConfiguration>& context) override {
        const GeodeConfiguration& config = context.config();
        XoroshiroRandomSource& random = context.random();
        const core::BlockPos& origin = context.origin();
        ::world::IChunk* level = context.level();

        int minGenOffset = config.minGenOffset;
        int maxGenOffset = config.maxGenOffset;

        // Reference: GeodeFeature.java lines 42-44
        std::vector<std::pair<core::BlockPos, int>> points;
        int numPoints = config.distributionPoints ? config.distributionPoints->sample(random) : 4;

        // Reference: GeodeFeature.java lines 51-54
        const GeodeLayerSettings& layerSettings = config.geodeLayerSettings;
        const GeodeCrackSettings& crackSettings = config.geodeCrackSettings;
        double crackSizeAdjustment = static_cast<double>(numPoints) / 10.0;

        double innerAir = 1.0 / std::sqrt(layerSettings.filling);
        double innermostBlockLayer = 1.0 / std::sqrt(layerSettings.innerLayer + crackSizeAdjustment);
        double innerCrust = 1.0 / std::sqrt(layerSettings.middleLayer + crackSizeAdjustment);
        double outerCrust = 1.0 / std::sqrt(layerSettings.outerLayer + crackSizeAdjustment);
        double crackSize = 1.0 / std::sqrt(crackSettings.baseCrackSize + random.nextDouble() / 2.0 +
            (numPoints > 3 ? crackSizeAdjustment : 0.0));
        bool shouldGenerateCrack = random.nextFloat() < crackSettings.generateCrackChance;

        // Reference: GeodeFeature.java lines 59-73
        int numInvalidPoints = 0;
        for (int i = 0; i < numPoints; ++i) {
            int x = config.outerWallDistance ? config.outerWallDistance->sample(random) : 4;
            int y = config.outerWallDistance ? config.outerWallDistance->sample(random) : 4;
            int z = config.outerWallDistance ? config.outerWallDistance->sample(random) : 4;
            core::BlockPos pos = origin.offset(x, y, z);

            ::world::IBlockType* block = level->getBlockState(pos);
            if (block) {
                BlockState* state = static_cast<BlockState*>(block);
                if (state->isAir()) {
                    ++numInvalidPoints;
                    if (numInvalidPoints > config.invalidBlocksThreshold) {
                        return false;
                    }
                }
            }

            int pointOff = config.pointOffset ? config.pointOffset->sample(random) : 1;
            points.push_back({pos, pointOff});
        }

        // Reference: GeodeFeature.java lines 75-95
        std::vector<core::BlockPos> crackPoints;
        if (shouldGenerateCrack) {
            int offsetIndex = random.nextInt(4);
            int crackOffset = numPoints * 2 + 1;

            if (offsetIndex == 0) {
                crackPoints.push_back(origin.offset(crackOffset, 7, 0));
                crackPoints.push_back(origin.offset(crackOffset, 5, 0));
                crackPoints.push_back(origin.offset(crackOffset, 1, 0));
            } else if (offsetIndex == 1) {
                crackPoints.push_back(origin.offset(0, 7, crackOffset));
                crackPoints.push_back(origin.offset(0, 5, crackOffset));
                crackPoints.push_back(origin.offset(0, 1, crackOffset));
            } else if (offsetIndex == 2) {
                crackPoints.push_back(origin.offset(crackOffset, 7, crackOffset));
                crackPoints.push_back(origin.offset(crackOffset, 5, crackOffset));
                crackPoints.push_back(origin.offset(crackOffset, 1, crackOffset));
            } else {
                crackPoints.push_back(origin.offset(0, 7, 0));
                crackPoints.push_back(origin.offset(0, 5, 0));
                crackPoints.push_back(origin.offset(0, 1, 0));
            }
        }

        // Reference: GeodeFeature.java lines 100-143
        // Iterate through bounding box and place blocks based on distance sums
        for (int x = origin.getX() + minGenOffset; x <= origin.getX() + maxGenOffset; ++x) {
            for (int y = origin.getY() + minGenOffset; y <= origin.getY() + maxGenOffset; ++y) {
                for (int z = origin.getZ() + minGenOffset; z <= origin.getZ() + maxGenOffset; ++z) {
                    core::BlockPos pointInside(x, y, z);

                    // Calculate distance sum for shell
                    double distSumShell = 0.0;
                    for (const auto& [pointPos, offset] : points) {
                        double dx = pointInside.getX() - pointPos.getX();
                        double dy = pointInside.getY() - pointPos.getY();
                        double dz = pointInside.getZ() - pointPos.getZ();
                        double distSqr = dx * dx + dy * dy + dz * dz + static_cast<double>(offset);
                        distSumShell += 1.0 / std::sqrt(distSqr);
                    }

                    // Calculate distance sum for crack
                    double distSumCrack = 0.0;
                    for (const auto& crackPoint : crackPoints) {
                        double dx = pointInside.getX() - crackPoint.getX();
                        double dy = pointInside.getY() - crackPoint.getY();
                        double dz = pointInside.getZ() - crackPoint.getZ();
                        double distSqr = dx * dx + dy * dy + dz * dz + crackSettings.crackPointOffset;
                        distSumCrack += 1.0 / std::sqrt(distSqr);
                    }

                    // Place blocks based on distance thresholds
                    if (distSumShell >= outerCrust) {
                        if (shouldGenerateCrack && distSumCrack >= crackSize && distSumShell < innerAir) {
                            // Place air (crack)
                        } else if (distSumShell >= innerAir) {
                            // Place filling
                        } else if (distSumShell >= innermostBlockLayer) {
                            // Place inner layer (budding amethyst area)
                            bool useAlternate = random.nextFloat() < config.useAlternateLayer0Chance;
                            // Place block based on useAlternate
                        } else if (distSumShell >= innerCrust) {
                            // Place middle layer
                        } else {
                            // Place outer layer
                        }
                    }
                }
            }
        }

        return true;
    }
};

//=============================================================================
// HugeFungusConfiguration
// Reference: HugeFungusConfiguration.java
//=============================================================================

/**
 * HugeFungusConfiguration - Configuration for huge fungus (nether mushrooms)
 * Reference: HugeFungusConfiguration.java
 */
class HugeFungusConfiguration : public FeatureConfiguration {
public:
    BlockState* validBaseState;
    BlockState* stemState;
    BlockState* hatState;
    BlockState* decorState;
    std::shared_ptr<blockpredicates::BlockPredicate> replaceableBlocks;
    bool planted;

    HugeFungusConfiguration(
        BlockState* validBaseState,
        BlockState* stemState,
        BlockState* hatState,
        BlockState* decorState,
        std::shared_ptr<blockpredicates::BlockPredicate> replaceableBlocks = nullptr,
        bool planted = false
    )
        : validBaseState(validBaseState)
        , stemState(stemState)
        , hatState(hatState)
        , decorState(decorState)
        , replaceableBlocks(replaceableBlocks)
        , planted(planted)
    {}
};

/**
 * HugeFungusFeature - Generates huge fungi in the Nether
 * Reference: HugeFungusFeature.java
 */
class HugeFungusFeature : public Feature<HugeFungusConfiguration> {
public:
    static constexpr float HUGE_PROBABILITY = 0.06f;

    /**
     * Place huge fungus
     * Reference: HugeFungusFeature.java place() lines 23-56
     */
    bool place(FeaturePlaceContext<HugeFungusConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const HugeFungusConfiguration& config = context.config();

        // Check base block
        ::world::IBlockType* belowBlock = level->getBlockState(origin.below());
        if (!belowBlock) return false;
        BlockState* belowState = static_cast<BlockState*>(belowBlock);
        if (belowState->getIdentifier() != config.validBaseState->getIdentifier()) {
            return false;
        }

        // Reference: HugeFungusFeature.java lines 39-43
        int totalHeight = 4 + random.nextInt(10);
        if (random.nextInt(12) == 0) {
            totalHeight *= 2;
        }

        // Reference: HugeFungusFeature.java line 51
        bool isHuge = !config.planted && random.nextFloat() < HUGE_PROBABILITY;

        // Clear origin
        // level->setBlockState(origin, air)

        // Place stem and hat
        placeStem(level, random, config, origin, totalHeight, isHuge);
        placeHat(level, random, config, origin, totalHeight, isHuge);

        return true;
    }

private:
    /**
     * Place stem
     * Reference: HugeFungusFeature.java placeStem() lines 67-96
     */
    void placeStem(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const HugeFungusConfiguration& config,
        const core::BlockPos& surfaceOrigin,
        int totalHeight,
        bool isHuge
    ) {
        core::BlockPos::MutableBlockPos blockPos;
        int stemRadius = isHuge ? 1 : 0;

        for (int dx = -stemRadius; dx <= stemRadius; ++dx) {
            for (int dz = -stemRadius; dz <= stemRadius; ++dz) {
                bool cornerOfHugeStem = isHuge && std::abs(dx) == stemRadius && std::abs(dz) == stemRadius;

                for (int dy = 0; dy < totalHeight; ++dy) {
                    blockPos.setWithOffset(surfaceOrigin, dx, dy, dz);

                    if (cornerOfHugeStem) {
                        if (random.nextFloat() < 0.1f) {
                            // Place stem
                        }
                    } else {
                        // Place stem
                    }
                }
            }
        }
    }

    /**
     * Place hat
     * Reference: HugeFungusFeature.java placeHat() lines 99-143
     */
    void placeHat(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const HugeFungusConfiguration& config,
        const core::BlockPos& surfaceOrigin,
        int totalHeight,
        bool isHuge
    ) {
        core::BlockPos::MutableBlockPos blockPos;
        bool placeVines = config.hatState->getIdentifier() == "minecraft:nether_wart_block";
        int hatHeight = std::min(random.nextInt(1 + totalHeight / 3) + 5, totalHeight);
        int hatStartY = totalHeight - hatHeight;

        for (int dy = hatStartY; dy <= totalHeight; ++dy) {
            int radius = dy < totalHeight - random.nextInt(3) ? 2 : 1;
            if (hatHeight > 8 && dy < hatStartY + 4) {
                radius = 3;
            }
            if (isHuge) {
                ++radius;
            }

            for (int dx = -radius; dx <= radius; ++dx) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    bool isEdgeX = dx == -radius || dx == radius;
                    bool isEdgeZ = dz == -radius || dz == radius;
                    bool inside = !isEdgeX && !isEdgeZ && dy != totalHeight;
                    bool corner = isEdgeX && isEdgeZ;
                    bool isHatBottom = dy < hatStartY + 3;

                    blockPos.setWithOffset(surfaceOrigin, dx, dy, dz);

                    if (isHatBottom) {
                        if (!inside) {
                            // Place hat drop block
                        }
                    } else if (inside) {
                        placeHatBlock(level, random, config, blockPos, 0.1f, 0.2f, placeVines ? 0.1f : 0.0f);
                    } else if (corner) {
                        placeHatBlock(level, random, config, blockPos, 0.01f, 0.7f, placeVines ? 0.083f : 0.0f);
                    } else {
                        placeHatBlock(level, random, config, blockPos, 5.0e-4f, 0.98f, placeVines ? 0.07f : 0.0f);
                    }
                }
            }
        }
    }

    /**
     * Place hat block with probabilities
     * Reference: HugeFungusFeature.java placeHatBlock() lines 146-155
     */
    void placeHatBlock(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const HugeFungusConfiguration& config,
        const core::BlockPos::MutableBlockPos& blockPos,
        float decorBlockProbability,
        float hatBlockProbability,
        float vinesProbability
    ) {
        if (random.nextFloat() < decorBlockProbability) {
            // Place decor state
        } else if (random.nextFloat() < hatBlockProbability) {
            // Place hat state
            if (random.nextFloat() < vinesProbability) {
                // Try place weeping vines
            }
        }
    }
};

//=============================================================================
// EndSpikeFeature (End crystal pillars)
// Reference: EndSpikeFeature.java
//=============================================================================

/**
 * EndSpikeConfiguration - Configuration for end spike (obsidian pillars)
 * Reference: SpikeConfiguration.java
 */
class EndSpikeConfiguration : public FeatureConfiguration {
public:
    bool crystalInvulnerable;
    std::vector<core::BlockPos> spikes;  // Simplified - would be SpikeFeature.EndSpike

    EndSpikeConfiguration(
        bool crystalInvulnerable = false
    )
        : crystalInvulnerable(crystalInvulnerable)
    {}
};

/**
 * EndSpikeFeature - Generates obsidian pillars in the End
 * Reference: EndSpikeFeature.java (simplified)
 */
class EndSpikeFeature : public Feature<EndSpikeConfiguration> {
public:
    bool place(FeaturePlaceContext<EndSpikeConfiguration>& context) override {
        // End spike placement is complex and involves:
        // - Placing obsidian pillar
        // - Placing end crystal entity
        // - Optional iron bars cage
        // Simplified implementation for now
        return true;
    }
};

//=============================================================================
// BlueIceFeature
// Reference: BlueIceFeature.java
//=============================================================================

/**
 * BlueIceFeature - Generates blue ice blobs in icebergs
 * Reference: BlueIceFeature.java
 */
class BlueIceFeature : public Feature<NoneFeatureConfiguration> {
public:
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        const core::BlockPos& origin = context.origin();
        ::world::IChunk* level = context.level();
        XoroshiroRandomSource& random = context.random();

        // Reference: BlueIceFeature.java lines 21-26
        // Check position is in packed ice
        ::world::IBlockType* block = level->getBlockState(origin);
        if (!block) return false;
        BlockState* state = static_cast<BlockState*>(block);
        if (state->getIdentifier() != "minecraft:packed_ice") {
            return false;
        }

        // Check above is air
        ::world::IBlockType* aboveBlock = level->getBlockState(origin.above());
        if (!aboveBlock) return false;
        BlockState* aboveState = static_cast<BlockState*>(aboveBlock);
        if (!aboveState->isAir()) {
            return false;
        }

        // Reference: BlueIceFeature.java lines 28-47
        // Place blue ice blob
        int placed = 0;
        int size = random.nextInt(11) + 10;

        for (int i = 0; i < size; ++i) {
            int dx = random.nextInt(2);
            int dy = random.nextInt(2);
            int dz = random.nextInt(2);
            core::BlockPos placePos = origin.offset(dx, dy, dz);

            ::world::IBlockType* targetBlock = level->getBlockState(placePos);
            if (targetBlock) {
                BlockState* targetState = static_cast<BlockState*>(targetBlock);
                if (targetState->getIdentifier() == "minecraft:packed_ice") {
                    // Place blue ice
                    // level->setBlockState(...)
                    ++placed;
                }
            }
        }

        return placed > 0;
    }
};

//=============================================================================
// ChorusPlantFeature
// Reference: ChorusPlantFeature.java
//=============================================================================

/**
 * ChorusPlantFeature - Generates chorus plants in the End
 * Reference: ChorusPlantFeature.java
 */
class ChorusPlantFeature : public Feature<NoneFeatureConfiguration> {
public:
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        const core::BlockPos& origin = context.origin();
        ::world::IChunk* level = context.level();
        XoroshiroRandomSource& random = context.random();

        // Reference: ChorusPlantFeature.java lines 17-24
        // Check if origin is air and below is end stone
        ::world::IBlockType* originBlock = level->getBlockState(origin);
        if (!originBlock) return false;
        BlockState* originState = static_cast<BlockState*>(originBlock);
        if (!originState->isAir()) {
            return false;
        }

        ::world::IBlockType* belowBlock = level->getBlockState(origin.below());
        if (!belowBlock) return false;
        BlockState* belowState = static_cast<BlockState*>(belowBlock);
        if (belowState->getIdentifier() != "minecraft:end_stone") {
            return false;
        }

        // Would call ChorusFlowerBlock.generatePlant() here
        // Simplified - just return true for now
        return true;
    }
};

//=============================================================================
// NetherForestVegetationConfiguration
// Reference: NetherForestVegetationConfig.java
//=============================================================================

/**
 * NetherForestVegetationConfiguration - Configuration for nether vegetation
 * Reference: NetherForestVegetationConfig.java
 */
class NetherForestVegetationConfiguration : public FeatureConfiguration {
public:
    std::shared_ptr<feature::stateproviders::BlockStateProvider> stateProvider;
    int spreadWidth;
    int spreadHeight;

    NetherForestVegetationConfiguration(
        std::shared_ptr<feature::stateproviders::BlockStateProvider> stateProvider,
        int spreadWidth = 8,
        int spreadHeight = 4
    )
        : stateProvider(stateProvider)
        , spreadWidth(spreadWidth)
        , spreadHeight(spreadHeight)
    {}
};

/**
 * NetherForestVegetationFeature - Places nether vegetation
 * Reference: NetherForestVegetationFeature.java
 */
class NetherForestVegetationFeature : public Feature<NetherForestVegetationConfiguration> {
public:
    bool place(FeaturePlaceContext<NetherForestVegetationConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        const NetherForestVegetationConfiguration& config = context.config();
        XoroshiroRandomSource& random = context.random();

        ::world::IBlockType* belowBlock = level->getBlockState(origin.below());
        if (!belowBlock) return false;
        BlockState* belowState = static_cast<BlockState*>(belowBlock);

        if (belowState->getIdentifier() != "minecraft:crimson_nylium" &&
            belowState->getIdentifier() != "minecraft:warped_nylium") {
            return false;
        }

        int y = origin.getY();
        if (y < level->getMinY() + 1 || y + 1 > level->getMaxY()) {
            return false;
        }

        int placed = 0;
        int spreadSq = config.spreadWidth * config.spreadWidth;

        for (int i = 0; i < spreadSq; ++i) {
            int dx = random.nextInt(config.spreadWidth) - random.nextInt(config.spreadWidth);
            int dy = random.nextInt(config.spreadHeight) - random.nextInt(config.spreadHeight);
            int dz = random.nextInt(config.spreadWidth) - random.nextInt(config.spreadWidth);
            core::BlockPos finalPos = origin.offset(dx, dy, dz);

            if (config.stateProvider) {
                ::world::IBlockType* targetBlock = level->getBlockState(finalPos);
                if (targetBlock) {
                    BlockState* targetState = static_cast<BlockState*>(targetBlock);
                    if (targetState->isAir() && finalPos.getY() > level->getMinY()) {
                        ++placed;
                    }
                }
            }
        }

        return placed > 0;
    }
};

//=============================================================================
// ProbabilityFeatureConfiguration
// Reference: ProbabilityFeatureConfiguration.java
//=============================================================================

class ProbabilityFeatureConfiguration : public FeatureConfiguration {
public:
    float probability;

    explicit ProbabilityFeatureConfiguration(float probability = 0.0f)
        : probability(probability)
    {}
};

/**
 * BambooFeature - Generates bamboo stalks
 * Reference: BambooFeature.java
 */
class BambooFeature : public Feature<ProbabilityFeatureConfiguration> {
public:
    bool place(FeaturePlaceContext<ProbabilityFeatureConfiguration>& context) override {
        const core::BlockPos& origin = context.origin();
        ::world::IChunk* level = context.level();
        XoroshiroRandomSource& random = context.random();
        const ProbabilityFeatureConfiguration& config = context.config();

        ::world::IBlockType* originBlock = level->getBlockState(origin);
        if (!originBlock) return false;
        BlockState* originState = static_cast<BlockState*>(originBlock);

        if (!originState->isAir()) {
            return false;
        }

        int height = random.nextInt(12) + 5;

        if (random.nextFloat() < config.probability) {
            int r = random.nextInt(4) + 1;
            int rSq = r * r;

            for (int xx = origin.getX() - r; xx <= origin.getX() + r; ++xx) {
                for (int zz = origin.getZ() - r; zz <= origin.getZ() + r; ++zz) {
                    int xd = xx - origin.getX();
                    int zd = zz - origin.getZ();
                    if (xd * xd + zd * zd <= rSq) {
                        // Would place podzol here
                    }
                }
            }
        }

        core::BlockPos::MutableBlockPos bambooPos(origin.getX(), origin.getY(), origin.getZ());
        for (int i = 0; i < height; ++i) {
            ::world::IBlockType* block = level->getBlockState(bambooPos);
            if (!block) break;
            BlockState* state = static_cast<BlockState*>(block);
            if (!state->isAir()) break;
            bambooPos.move(0, 1, 0);
        }

        return true;
    }
};

//=============================================================================
// ColumnFeatureConfiguration
// Reference: ColumnFeatureConfiguration.java
//=============================================================================

class ColumnFeatureConfiguration : public FeatureConfiguration {
public:
    std::shared_ptr<util::IntProvider> reach;
    std::shared_ptr<util::IntProvider> height;

    ColumnFeatureConfiguration(
        std::shared_ptr<util::IntProvider> reach,
        std::shared_ptr<util::IntProvider> height
    )
        : reach(reach)
        , height(height)
    {}
};

/**
 * BasaltColumnsFeature - Generates basalt columns in the Nether
 * Reference: BasaltColumnsFeature.java
 */
class BasaltColumnsFeature : public Feature<ColumnFeatureConfiguration> {
public:
    bool place(FeaturePlaceContext<ColumnFeatureConfiguration>& context) override {
        const core::BlockPos& origin = context.origin();
        ::world::IChunk* level = context.level();
        XoroshiroRandomSource& random = context.random();
        const ColumnFeatureConfiguration& config = context.config();
        ChunkGenerator* generator = context.chunkGenerator();

        int lavaSeaLevel = generator ? generator->getSeaLevel() : 32;
        int columnHeight = config.height ? config.height->sample(random) : 5;
        bool generateClustered = random.nextFloat() < 0.9f;
        int reach = std::min(columnHeight, generateClustered ? 5 : 8);
        int count = generateClustered ? 50 : 15;

        bool placed = false;
        for (int i = 0; i < count; ++i) {
            int dx = random.nextInt(reach * 2 + 1) - reach;
            int dz = random.nextInt(reach * 2 + 1) - reach;
            core::BlockPos pos = origin.offset(dx, 0, dz);

            int blocksToPlaceY = columnHeight - std::abs(dx) - std::abs(dz);
            if (blocksToPlaceY >= 0) {
                placed = true;
            }
        }

        return placed;
    }
};

//=============================================================================
// DeltaFeatureConfiguration
// Reference: DeltaFeatureConfiguration.java
//=============================================================================

class DeltaFeatureConfiguration : public FeatureConfiguration {
public:
    BlockState* contents;
    BlockState* rim;
    std::shared_ptr<util::IntProvider> size;
    std::shared_ptr<util::IntProvider> rimSize;

    DeltaFeatureConfiguration(
        BlockState* contents,
        BlockState* rim,
        std::shared_ptr<util::IntProvider> size,
        std::shared_ptr<util::IntProvider> rimSize
    )
        : contents(contents)
        , rim(rim)
        , size(size)
        , rimSize(rimSize)
    {}
};

/**
 * DeltaFeature - Generates magma deltas in the Nether
 * Reference: DeltaFeature.java
 */
class DeltaFeature : public Feature<DeltaFeatureConfiguration> {
public:
    bool place(FeaturePlaceContext<DeltaFeatureConfiguration>& context) override {
        XoroshiroRandomSource& random = context.random();
        ::world::IChunk* level = context.level();
        const DeltaFeatureConfiguration& config = context.config();
        const core::BlockPos& origin = context.origin();

        bool spawnRim = random.nextDouble() < 0.9;
        int rimX = spawnRim && config.rimSize ? config.rimSize->sample(random) : 0;
        int rimZ = spawnRim && config.rimSize ? config.rimSize->sample(random) : 0;

        int radiusX = config.size ? config.size->sample(random) : 3;
        int radiusZ = config.size ? config.size->sample(random) : 3;
        int radiusLimit = std::max(radiusX, radiusZ);

        bool anyPlaced = false;

        for (int dx = -radiusX; dx <= radiusX; ++dx) {
            for (int dz = -radiusZ; dz <= radiusZ; ++dz) {
                if (std::abs(dx) + std::abs(dz) <= radiusLimit) {
                    anyPlaced = true;
                }
            }
        }

        return anyPlaced;
    }
};

//=============================================================================
// SeagrassFeature
// Reference: SeagrassFeature.java
//=============================================================================

class SeagrassFeature : public Feature<ProbabilityFeatureConfiguration> {
public:
    bool place(FeaturePlaceContext<ProbabilityFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();

        int placed = 0;

        for (int i = 0; i < 64; ++i) {
            int dx = random.nextInt(8) - random.nextInt(8);
            int dy = random.nextInt(8) - random.nextInt(8);
            int dz = random.nextInt(8) - random.nextInt(8);
            core::BlockPos placePos = origin.offset(dx, dy, dz);

            ::world::IBlockType* block = level->getBlockState(placePos);
            if (block) {
                BlockState* state = static_cast<BlockState*>(block);
                if (state->getIdentifier() == "minecraft:water") {
                    ::world::IBlockType* belowBlock = level->getBlockState(placePos.below());
                    if (belowBlock) {
                        BlockState* belowState = static_cast<BlockState*>(belowBlock);
                        if (!belowState->isAir()) {
                            ++placed;
                        }
                    }
                }
            }
        }

        return placed > 0;
    }
};

//=============================================================================
// KelpFeature
// Reference: KelpFeature.java
//=============================================================================

class KelpFeature : public Feature<NoneFeatureConfiguration> {
public:
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();

        int placed = 0;

        for (int i = 0; i < 64; ++i) {
            int dx = random.nextInt(8) - random.nextInt(8);
            int dz = random.nextInt(8) - random.nextInt(8);
            int dy = random.nextInt(8) - random.nextInt(8);
            core::BlockPos checkPos = origin.offset(dx, dy, dz);

            ::world::IBlockType* block = level->getBlockState(checkPos);
            if (block) {
                BlockState* state = static_cast<BlockState*>(block);
                if (state->getIdentifier() == "minecraft:water") {
                    int height = 1 + random.nextInt(10);
                    for (int y = 0; y < height; ++y) {
                        core::BlockPos kelpPos = checkPos.above(y);
                        ::world::IBlockType* kelpBlock = level->getBlockState(kelpPos);
                        if (kelpBlock) {
                            BlockState* kelpState = static_cast<BlockState*>(kelpBlock);
                            if (kelpState->getIdentifier() == "minecraft:water") {
                                ++placed;
                            } else {
                                break;
                            }
                        }
                    }
                }
            }
        }

        return placed > 0;
    }
};

//=============================================================================
// VinesFeature
// Reference: VinesFeature.java
//=============================================================================

class VinesFeature : public Feature<NoneFeatureConfiguration> {
public:
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();

        core::BlockPos::MutableBlockPos pos(origin.getX(), origin.getY(), origin.getZ());
        int maxY = std::min(origin.getY() + 50, level->getMaxY());

        for (pos.setY(origin.getY()); pos.getY() < maxY; pos.move(0, 1, 0)) {
            ::world::IBlockType* block = level->getBlockState(pos);
            if (block) {
                BlockState* state = static_cast<BlockState*>(block);
                if (state->isAir()) {
                    // Would place vines on adjacent solid blocks
                }
            }
        }

        return true;
    }
};

//=============================================================================
// HugeMushroomFeatureConfiguration
// Reference: HugeMushroomFeatureConfiguration.java
//=============================================================================

struct HugeMushroomFeatureConfiguration {
    std::shared_ptr<feature::stateproviders::BlockStateProvider> capProvider;
    std::shared_ptr<feature::stateproviders::BlockStateProvider> stemProvider;
    int foliageRadius = 2;

    HugeMushroomFeatureConfiguration(
        std::shared_ptr<feature::stateproviders::BlockStateProvider> cap,
        std::shared_ptr<feature::stateproviders::BlockStateProvider> stem,
        int radius = 2
    )
        : capProvider(std::move(cap))
        , stemProvider(std::move(stem))
        , foliageRadius(radius)
    {}
};

//=============================================================================
// AbstractHugeMushroomFeature
// Reference: AbstractHugeMushroomFeature.java
//=============================================================================

class AbstractHugeMushroomFeature : public Feature<HugeMushroomFeatureConfiguration> {
protected:
    // Reference: AbstractHugeMushroomFeature.java placeTrunk() lines 18-24
    void placeTrunk(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin,
        const HugeMushroomFeatureConfiguration& config,
        int treeHeight,
        core::BlockPos::MutableBlockPos& blockPos
    ) {
        for (int dy = 0; dy < treeHeight; ++dy) {
            blockPos.set(origin.getX(), origin.getY() + dy, origin.getZ());
            placeMushroomBlock(level, blockPos, config.stemProvider->getState(random, origin));
        }
    }

    // Reference: AbstractHugeMushroomFeature.java placeMushroomBlock() lines 26-31
    void placeMushroomBlock(
        ::world::IChunk* level,
        const core::BlockPos::MutableBlockPos& pos,
        BlockState* newState
    ) {
        ::world::IBlockType* currentBlock = level->getBlockState(pos);
        if (currentBlock) {
            BlockState* currentState = static_cast<BlockState*>(currentBlock);
            if (currentState->isAir() || currentState->getIdentifier() == "minecraft:brown_mushroom_block" ||
                currentState->getIdentifier() == "minecraft:red_mushroom_block" ||
                currentState->getIdentifier() == "minecraft:mushroom_stem") {
                // Would set block here
            }
        }
    }

    // Reference: AbstractHugeMushroomFeature.java getTreeHeight() lines 34-41
    int getTreeHeight(XoroshiroRandomSource& random) {
        int treeHeight = random.nextInt(3) + 4;
        if (random.nextInt(12) == 0) {
            treeHeight *= 2;
        }
        return treeHeight;
    }

    // Reference: AbstractHugeMushroomFeature.java isValidPosition() lines 43-68
    bool isValidPosition(
        ::world::IChunk* level,
        const core::BlockPos& origin,
        int treeHeight,
        core::BlockPos::MutableBlockPos& blockPos,
        const HugeMushroomFeatureConfiguration& config
    ) {
        int y = origin.getY();
        if (y < level->getMinY() + 1 || y + treeHeight + 1 > level->getMaxY()) {
            return false;
        }

        core::BlockPos below = origin.below();
        ::world::IBlockType* belowBlock = level->getBlockState(below);
        if (belowBlock) {
            BlockState* belowState = static_cast<BlockState*>(belowBlock);
            // Original: !belowState.isDirt() && !belowState.is("minecraft:mycelium")
            bool isDirt = belowState->getIdentifier() == "minecraft:dirt" ||
                          belowState->getIdentifier() == "minecraft:grass_block" ||
                          belowState->getIdentifier() == "minecraft:coarse_dirt" ||
                          belowState->getIdentifier() == "minecraft:podzol";
            if (!isDirt && belowState->getIdentifier() != "minecraft:mycelium") {
                return false;
            }
        }

        for (int dy = 0; dy <= treeHeight; ++dy) {
            int radius = getTreeRadiusForHeight(-1, -1, config.foliageRadius, dy);
            for (int dx = -radius; dx <= radius; ++dx) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    blockPos.set(origin.getX() + dx, origin.getY() + dy, origin.getZ() + dz);
                    ::world::IBlockType* block = level->getBlockState(blockPos);
                    if (block) {
                        BlockState* state = static_cast<BlockState*>(block);
                        if (!state->isAir() && state->getIdentifier() != "minecraft:leaves") {
                            return false;
                        }
                    }
                }
            }
        }

        return true;
    }

    virtual int getTreeRadiusForHeight(int trunkHeight, int treeHeight, int leafRadius, int yo) = 0;

    virtual void makeCap(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin,
        int treeHeight,
        core::BlockPos::MutableBlockPos& blockPos,
        const HugeMushroomFeatureConfiguration& config
    ) = 0;

public:
    // Reference: AbstractHugeMushroomFeature.java place() lines 70-84
    bool place(FeaturePlaceContext<HugeMushroomFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const HugeMushroomFeatureConfiguration& config = context.config();

        int treeHeight = getTreeHeight(random);
        core::BlockPos::MutableBlockPos blockPos(origin.getX(), origin.getY(), origin.getZ());

        if (!isValidPosition(level, origin, treeHeight, blockPos, config)) {
            return false;
        }

        makeCap(level, random, origin, treeHeight, blockPos, config);
        placeTrunk(level, random, origin, config, treeHeight, blockPos);
        return true;
    }
};

//=============================================================================
// HugeBrownMushroomFeature
// Reference: HugeBrownMushroomFeature.java
//=============================================================================

class HugeBrownMushroomFeature : public AbstractHugeMushroomFeature {
protected:
    // Reference: HugeBrownMushroomFeature.java getTreeRadiusForHeight() lines 45-47
    int getTreeRadiusForHeight(int trunkHeight, int treeHeight, int leafRadius, int yo) override {
        return yo <= 3 ? 0 : leafRadius;
    }

    // Reference: HugeBrownMushroomFeature.java makeCap() lines 16-42
    void makeCap(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin,
        int treeHeight,
        core::BlockPos::MutableBlockPos& blockPos,
        const HugeMushroomFeatureConfiguration& config
    ) override {
        int radius = config.foliageRadius;

        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                bool minX = dx == -radius;
                bool maxX = dx == radius;
                bool minZ = dz == -radius;
                bool maxZ = dz == radius;
                bool xEdge = minX || maxX;
                bool zEdge = minZ || maxZ;

                if (!xEdge || !zEdge) {
                    blockPos.set(origin.getX() + dx, origin.getY() + treeHeight, origin.getZ() + dz);
                    bool west = minX || (zEdge && dx == 1 - radius);
                    bool east = maxX || (zEdge && dx == radius - 1);
                    bool north = minZ || (xEdge && dz == 1 - radius);
                    bool south = maxZ || (xEdge && dz == radius - 1);

                    BlockState* state = config.capProvider->getState(random, origin);
                    // TODO: Set properties when property system is implemented
                    // state = state->setValue(WEST, west);
                    // state = state->setValue(EAST, east);
                    // state = state->setValue(NORTH, north);
                    // state = state->setValue(SOUTH, south);
                    (void)west; (void)east; (void)north; (void)south; // Suppress unused warnings

                    placeMushroomBlock(level, blockPos, state);
                }
            }
        }
    }
};

//=============================================================================
// HugeRedMushroomFeature
// Reference: HugeRedMushroomFeature.java
//=============================================================================

class HugeRedMushroomFeature : public AbstractHugeMushroomFeature {
protected:
    // Reference: HugeRedMushroomFeature.java getTreeRadiusForHeight() lines 44-53
    int getTreeRadiusForHeight(int trunkHeight, int treeHeight, int leafRadius, int yo) override {
        int radius = 0;
        if (yo < treeHeight && yo >= treeHeight - 3) {
            radius = leafRadius;
        } else if (yo == treeHeight) {
            radius = leafRadius;
        }
        return radius;
    }

    // Reference: HugeRedMushroomFeature.java makeCap() lines 16-41
    void makeCap(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin,
        int treeHeight,
        core::BlockPos::MutableBlockPos& blockPos,
        const HugeMushroomFeatureConfiguration& config
    ) override {
        for (int dy = treeHeight - 3; dy <= treeHeight; ++dy) {
            int radius = dy < treeHeight ? config.foliageRadius : config.foliageRadius - 1;
            int center = config.foliageRadius - 2;

            for (int dx = -radius; dx <= radius; ++dx) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    bool minX = dx == -radius;
                    bool maxX = dx == radius;
                    bool minZ = dz == -radius;
                    bool maxZ = dz == radius;
                    bool xEdge = minX || maxX;
                    bool zEdge = minZ || maxZ;

                    if (dy >= treeHeight || xEdge != zEdge) {
                        blockPos.set(origin.getX() + dx, origin.getY() + dy, origin.getZ() + dz);

                        BlockState* state = config.capProvider->getState(random, origin);
                        // TODO: Set properties when property system is implemented
                        // bool up = dy >= treeHeight - 1;
                        // state = state->setValue(UP, up);
                        // state = state->setValue(WEST, dx < -center);
                        // etc.
                        (void)center; // Suppress unused warning

                        placeMushroomBlock(level, blockPos, state);
                    }
                }
            }
        }
    }
};

//=============================================================================
// CoralFeature
// Reference: CoralFeature.java
//=============================================================================

class CoralFeature : public Feature<NoneFeatureConfiguration> {
protected:
    // Reference: CoralFeature.java placeCoralBlock() lines 35-66
    bool placeCoralBlock(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const core::BlockPos& pos,
        BlockState* state
    ) {
        core::BlockPos above = pos.above();
        ::world::IBlockType* targetBlock = level->getBlockState(pos);
        ::world::IBlockType* aboveBlock = level->getBlockState(above);

        if (!targetBlock || !aboveBlock) {
            return false;
        }

        BlockState* targetState = static_cast<BlockState*>(targetBlock);
        BlockState* aboveState = static_cast<BlockState*>(aboveBlock);

        if ((targetState->getIdentifier() == "minecraft:water" || targetState->getIdentifier() == "minecraft:coral_block") &&
            aboveState->getIdentifier() == "minecraft:water") {
            // Would set coral block here
            return true;
        }

        return false;
    }

    virtual bool placeFeature(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin,
        BlockState* state
    ) = 0;

public:
    // Reference: CoralFeature.java place() lines 25-31
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();

        // Pick a random coral block type
        BlockState* coralState = static_cast<BlockState*>(::world::MinecraftBlocks::get("minecraft:brain_coral_block")); // Simplified
        return placeFeature(level, random, origin, coralState);
    }
};

//=============================================================================
// CoralClawFeature
// Reference: CoralClawFeature.java
//=============================================================================

class CoralClawFeature : public CoralFeature {
protected:
    // Reference: CoralClawFeature.java placeFeature() lines 19-64
    bool placeFeature(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin,
        BlockState* state
    ) override {
        if (!placeCoralBlock(level, random, origin, state)) {
            return false;
        }

        core::Direction clawDirection = blockpredicates::fromHorizontalIndex(random.nextInt(4));
        int nBranches = random.nextInt(2) + 2;

        // Get shuffled directions
        std::vector<core::Direction> directions;
        directions.push_back(clawDirection);
        directions.push_back(blockpredicates::rotateYClockwise(clawDirection));
        directions.push_back(blockpredicates::rotateYCounterClockwise(clawDirection));

        // Shuffle
        for (size_t i = directions.size() - 1; i > 0; --i) {
            size_t j = random.nextInt(static_cast<int>(i + 1));
            std::swap(directions[i], directions[j]);
        }

        for (int b = 0; b < nBranches && b < static_cast<int>(directions.size()); ++b) {
            core::Direction branchDirection = directions[b];
            core::BlockPos::MutableBlockPos mutPos(origin.getX(), origin.getY(), origin.getZ());
            int sidewayLength = random.nextInt(2) + 1;
            mutPos.move(branchDirection);

            int inwayLength;
            core::Direction segmentDirection;

            if (branchDirection == clawDirection) {
                segmentDirection = clawDirection;
                inwayLength = random.nextInt(3) + 2;
            } else {
                mutPos.move(core::Direction::UP);
                std::vector<core::Direction> segmentDirs = {branchDirection, core::Direction::UP};
                segmentDirection = segmentDirs[random.nextInt(2)];
                inwayLength = random.nextInt(3) + 3;
            }

            for (int i = 0; i < sidewayLength && placeCoralBlock(level, random, mutPos, state); ++i) {
                mutPos.move(segmentDirection);
            }

            mutPos.move(blockpredicates::opposite(segmentDirection));
            mutPos.move(core::Direction::UP);

            for (int i = 0; i < inwayLength; ++i) {
                mutPos.move(clawDirection);
                if (!placeCoralBlock(level, random, mutPos, state)) {
                    break;
                }
                if (random.nextFloat() < 0.25f) {
                    mutPos.move(core::Direction::UP);
                }
            }
        }

        return true;
    }
};

//=============================================================================
// CoralMushroomFeature
// Reference: CoralMushroomFeature.java
//=============================================================================

class CoralMushroomFeature : public CoralFeature {
protected:
    // Reference: CoralMushroomFeature.java placeFeature() lines 16-35
    bool placeFeature(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin,
        BlockState* state
    ) override {
        int height = random.nextInt(3) + 3;
        int width = random.nextInt(3) + 3;
        int length = random.nextInt(3) + 3;
        int sinkValue = random.nextInt(3) + 1;

        core::BlockPos::MutableBlockPos mutPos(origin.getX(), origin.getY(), origin.getZ());

        for (int x = 0; x <= width; ++x) {
            for (int y = 0; y <= height; ++y) {
                for (int z = 0; z <= length; ++z) {
                    mutPos.set(x + origin.getX(), y + origin.getY() - sinkValue, z + origin.getZ());

                    bool xEdge = (x == 0 || x == width);
                    bool yEdge = (y == 0 || y == height);
                    bool zEdge = (z == 0 || z == length);

                    if ((!xEdge || !yEdge) && (!zEdge || !yEdge) && (!xEdge || !zEdge) &&
                        (xEdge || yEdge || zEdge)) {
                        if (random.nextFloat() >= 0.1f) {
                            placeCoralBlock(level, random, mutPos, state);
                        }
                    }
                }
            }
        }

        return true;
    }
};

//=============================================================================
// CoralTreeFeature
// Reference: CoralTreeFeature.java
//=============================================================================

class CoralTreeFeature : public CoralFeature {
protected:
    // Reference: CoralTreeFeature.java placeFeature() lines 17-50
    bool placeFeature(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin,
        BlockState* state
    ) override {
        core::BlockPos::MutableBlockPos mutPos(origin.getX(), origin.getY(), origin.getZ());
        int trunkHeight = random.nextInt(3) + 1;

        for (int i = 0; i < trunkHeight; ++i) {
            if (!placeCoralBlock(level, random, mutPos, state)) {
                return true;
            }
            mutPos.move(core::Direction::UP);
        }

        core::BlockPos trunkTop(mutPos.getX(), mutPos.getY(), mutPos.getZ());
        int nBranches = random.nextInt(3) + 2;

        // Get shuffled horizontal directions
        std::vector<core::Direction> directions;
        for (int i = 0; i < 4; ++i) {
            directions.push_back(blockpredicates::fromHorizontalIndex(i));
        }
        for (size_t i = directions.size() - 1; i > 0; --i) {
            size_t j = random.nextInt(static_cast<int>(i + 1));
            std::swap(directions[i], directions[j]);
        }

        for (int b = 0; b < nBranches && b < 4; ++b) {
            core::Direction branchDir = directions[b];
            mutPos.set(trunkTop.getX(), trunkTop.getY(), trunkTop.getZ());
            mutPos.move(branchDir);

            int branchHeight = random.nextInt(5) + 2;
            int segmentLength = 0;

            for (int j = 0; j < branchHeight && placeCoralBlock(level, random, mutPos, state); ++j) {
                ++segmentLength;
                mutPos.move(core::Direction::UP);
                if (j == 0 || (segmentLength >= 2 && random.nextFloat() < 0.25f)) {
                    mutPos.move(branchDir);
                    segmentLength = 0;
                }
            }
        }

        return true;
    }
};

//=============================================================================
// EndIslandFeature
// Reference: EndIslandFeature.java
//=============================================================================

class EndIslandFeature : public Feature<NoneFeatureConfiguration> {
public:
    // Reference: EndIslandFeature.java place() lines 16-35
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();

        float size = static_cast<float>(random.nextInt(3)) + 4.0f;

        for (int y = 0; size > 0.5f; --y) {
            int minR = static_cast<int>(std::floor(-size));
            int maxR = static_cast<int>(std::ceil(size));

            for (int x = minR; x <= maxR; ++x) {
                for (int z = minR; z <= maxR; ++z) {
                    float distSq = static_cast<float>(x * x + z * z);
                    if (distSq <= (size + 1.0f) * (size + 1.0f)) {
                        // Would set END_STONE block at origin.offset(x, y, z)
                    }
                }
            }

            size -= static_cast<float>(random.nextInt(2)) + 0.5f;
        }

        return true;
    }
};

//=============================================================================
// EndGatewayConfiguration
// Reference: EndGatewayConfiguration.java
//=============================================================================

struct EndGatewayConfiguration {
    std::optional<core::BlockPos> exit;
    bool exact = false;

    EndGatewayConfiguration() = default;

    EndGatewayConfiguration(const core::BlockPos& exitPos, bool isExact)
        : exit(exitPos), exact(isExact)
    {}

    std::optional<core::BlockPos> getExit() const { return exit; }
    bool isExitExact() const { return exact; }
};

//=============================================================================
// EndGatewayFeature
// Reference: EndGatewayFeature.java
//=============================================================================

class EndGatewayFeature : public Feature<EndGatewayConfiguration> {
public:
    // Reference: EndGatewayFeature.java place() lines 16-48
    bool place(FeaturePlaceContext<EndGatewayConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        const EndGatewayConfiguration& config = context.config();

        for (int x = -1; x <= 1; ++x) {
            for (int y = -2; y <= 2; ++y) {
                for (int z = -1; z <= 1; ++z) {
                    core::BlockPos pos(origin.getX() + x, origin.getY() + y, origin.getZ() + z);

                    bool sameX = x == 0;
                    bool sameY = y == 0;
                    bool sameZ = z == 0;
                    bool isEnd = std::abs(y) == 2;

                    if (sameX && sameY && sameZ) {
                        // Place END_GATEWAY at center
                    } else if (sameY) {
                        // Place AIR
                    } else if (isEnd && sameX && sameZ) {
                        // Place BEDROCK at top/bottom center
                    } else if ((sameX || sameZ) && !isEnd) {
                        // Place BEDROCK on sides
                    } else {
                        // Place AIR
                    }
                }
            }
        }

        return true;
    }
};

//=============================================================================
// EndPodiumFeature
// Reference: EndPodiumFeature.java
//=============================================================================

class EndPodiumFeature : public Feature<NoneFeatureConfiguration> {
private:
    bool m_active;

public:
    static constexpr int PODIUM_RADIUS = 4;
    static constexpr int PODIUM_PILLAR_HEIGHT = 4;
    static constexpr int RIM_RADIUS = 1;
    static constexpr float CORNER_ROUNDING = 0.5f;

    EndPodiumFeature(bool active = false) : m_active(active) {}

    // Reference: EndPodiumFeature.java place() lines 30-74
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();

        // Place podium base
        for (int x = -4; x <= 4; ++x) {
            for (int y = -1; y <= 32; ++y) {
                for (int z = -4; z <= 4; ++z) {
                    core::BlockPos pos(origin.getX() + x, origin.getY() + y, origin.getZ() + z);

                    double dist = std::sqrt(x * x + z * z);
                    bool insideRim = dist <= 2.5;

                    if (insideRim || dist <= 3.5) {
                        if (y < 0) {
                            if (insideRim) {
                                // Place BEDROCK
                            } else {
                                // Place END_STONE
                            }
                        } else if (y > 0) {
                            // Place AIR
                        } else if (!insideRim) {
                            // Place BEDROCK
                        } else if (m_active) {
                            // Place END_PORTAL
                        } else {
                            // Place AIR
                        }
                    }
                }
            }
        }

        // Place pillar
        for (int y = 0; y < PODIUM_PILLAR_HEIGHT; ++y) {
            // Place BEDROCK at origin.above(y)
        }

        // Place torches
        core::BlockPos centerOfPillar = origin.above(2);
        for (int i = 0; i < 4; ++i) {
            core::Direction face = blockpredicates::fromHorizontalIndex(i);
            // Place WALL_TORCH at centerOfPillar.relative(face)
        }

        return true;
    }
};

//=============================================================================
// EndPlatformFeature
// Reference: EndPlatformFeature.java
//=============================================================================

class EndPlatformFeature : public Feature<NoneFeatureConfiguration> {
public:
    // Reference: EndPlatformFeature.java place() lines 16-18
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        createEndPlatform(context.level(), context.origin());
        return true;
    }

    // Reference: EndPlatformFeature.java createEndPlatform() lines 21-40
    static void createEndPlatform(::world::IChunk* level, const core::BlockPos& origin) {
        core::BlockPos::MutableBlockPos pos(origin.getX(), origin.getY(), origin.getZ());

        for (int dz = -2; dz <= 2; ++dz) {
            for (int dx = -2; dx <= 2; ++dx) {
                for (int dy = -1; dy < 3; ++dy) {
                    pos.set(origin.getX() + dx, origin.getY() + dy, origin.getZ() + dz);
                    // dy == -1: place OBSIDIAN
                    // dy >= 0: place AIR
                }
            }
        }
    }
};

//=============================================================================
// PointedDripstoneConfiguration
// Reference: PointedDripstoneConfiguration.java
//=============================================================================

struct PointedDripstoneConfiguration {
    float chanceOfTallerDripstone = 0.2f;
    float chanceOfDirectionalSpread = 0.7f;
    float chanceOfSpreadRadius2 = 0.5f;
    float chanceOfSpreadRadius3 = 0.5f;

    PointedDripstoneConfiguration() = default;

    PointedDripstoneConfiguration(
        float tallerChance,
        float directionalSpread,
        float spreadRadius2,
        float spreadRadius3
    )
        : chanceOfTallerDripstone(tallerChance)
        , chanceOfDirectionalSpread(directionalSpread)
        , chanceOfSpreadRadius2(spreadRadius2)
        , chanceOfSpreadRadius3(spreadRadius3)
    {}
};

//=============================================================================
// PointedDripstoneFeature
// Reference: PointedDripstoneFeature.java
//=============================================================================

class PointedDripstoneFeature : public Feature<PointedDripstoneConfiguration> {
public:
    // Reference: PointedDripstoneFeature.java place() lines 16-30
    bool place(FeaturePlaceContext<PointedDripstoneConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& pos = context.origin();
        XoroshiroRandomSource& random = context.random();
        const PointedDripstoneConfiguration& config = context.config();

        // Get tip direction
        std::optional<core::Direction> tipDirection = getTipDirection(level, pos, random);
        if (!tipDirection.has_value()) {
            return false;
        }

        core::BlockPos rootPos = pos.relative(blockpredicates::opposite(*tipDirection));
        createPatchOfDripstoneBlocks(level, random, rootPos, config);

        // Determine height
        core::BlockPos tipPos = pos.relative(*tipDirection);
        ::world::IBlockType* tipBlock = level->getBlockState(tipPos);
        bool canGrowTaller = tipBlock && static_cast<BlockState*>(tipBlock)->isAir();
        int height = (random.nextFloat() < config.chanceOfTallerDripstone && canGrowTaller) ? 2 : 1;

        // Would grow pointed dripstone here
        return true;
    }

private:
    // Reference: PointedDripstoneFeature.java getTipDirection() lines 33-43
    std::optional<core::Direction> getTipDirection(
        ::world::IChunk* level,
        const core::BlockPos& pos,
        XoroshiroRandomSource& random
    ) {
        ::world::IBlockType* aboveBlock = level->getBlockState(pos.above());
        ::world::IBlockType* belowBlock = level->getBlockState(pos.below());

        bool canPlaceAbove = aboveBlock && isDripstoneBase(static_cast<BlockState*>(aboveBlock));
        bool canPlaceBelow = belowBlock && isDripstoneBase(static_cast<BlockState*>(belowBlock));

        if (canPlaceAbove && canPlaceBelow) {
            return random.nextBoolean() ? core::Direction::DOWN : core::Direction::UP;
        } else if (canPlaceAbove) {
            return core::Direction::DOWN;
        } else if (canPlaceBelow) {
            return core::Direction::UP;
        }
        return std::nullopt;
    }

    bool isDripstoneBase(BlockState* state) {
        return state->getIdentifier() == "minecraft:dripstone_block" || state->getIdentifier() == "minecraft:stone" ||
               state->getIdentifier() == "minecraft:granite" || state->getIdentifier() == "minecraft:diorite" ||
               state->getIdentifier() == "minecraft:andesite";
    }

    // Reference: PointedDripstoneFeature.java createPatchOfDripstoneBlocks() lines 45-62
    void createPatchOfDripstoneBlocks(
        ::world::IChunk* level,
        XoroshiroRandomSource& random,
        const core::BlockPos& pos,
        const PointedDripstoneConfiguration& config
    ) {
        // Would place dripstone blocks
        for (int dir = 0; dir < 4; ++dir) {
            core::Direction direction = blockpredicates::fromHorizontalIndex(dir);
            if (random.nextFloat() <= config.chanceOfDirectionalSpread) {
                core::BlockPos pos1 = pos.relative(direction);
                // Would place dripstone block at pos1
                if (random.nextFloat() <= config.chanceOfSpreadRadius2) {
                    core::BlockPos pos2 = pos1.relative(blockpredicates::fromIndex(random.nextInt(6)));
                    // Would place dripstone block at pos2
                    if (random.nextFloat() <= config.chanceOfSpreadRadius3) {
                        core::BlockPos pos3 = pos2.relative(blockpredicates::fromIndex(random.nextInt(6)));
                        // Would place dripstone block at pos3
                    }
                }
            }
        }
    }
};

//=============================================================================
// LargeDripstoneConfiguration
// Reference: LargeDripstoneConfiguration.java
//=============================================================================

struct LargeDripstoneConfiguration {
    int floorToCeilingSearchRange = 30;
    std::shared_ptr<util::IntProvider> columnRadius;
    float maxColumnRadiusToCaveHeightRatio = 0.33f;
    float stalactiteBluntness = 0.7f;
    float stalagmiteBluntness = 0.7f;
    float heightScale = 0.8f;
    float windSpeed = 0.0f;
    int minRadiusForWind = 1;
    float minBluntnessForWind = 0.0f;

    LargeDripstoneConfiguration() = default;
};

//=============================================================================
// LargeDripstoneFeature
// Reference: LargeDripstoneFeature.java
//=============================================================================

class LargeDripstoneFeature : public Feature<LargeDripstoneConfiguration> {
public:
    // Reference: LargeDripstoneFeature.java place() lines 26-71
    bool place(FeaturePlaceContext<LargeDripstoneConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const LargeDripstoneConfiguration& config = context.config();

        // Check if position is empty or water
        ::world::IBlockType* originBlock = level->getBlockState(origin);
        if (!originBlock) {
            return false;
        }
        BlockState* originState = static_cast<BlockState*>(originBlock);
        if (!originState->isAir() && originState->getIdentifier() != "minecraft:water") {
            return false;
        }

        // Would scan for column and place large dripstone
        // Simplified implementation - just return true for now
        return true;
    }
};

//=============================================================================
// DripstoneClusterConfiguration
// Reference: DripstoneClusterConfiguration.java
//=============================================================================

struct DripstoneClusterConfiguration {
    int floorToCeilingSearchRange = 12;
    std::shared_ptr<util::IntProvider> height;
    std::shared_ptr<util::IntProvider> radius;
    int maxStalagmiteStalactiteHeightDiff = 2;
    int heightDeviation = 1;
    std::shared_ptr<util::IntProvider> dripstoneBlockLayerThickness;
    float density = 0.5f;
    float wetness = 0.5f;
    float chanceOfDripstoneColumnAtMaxDistanceFromCenter = 0.5f;
    int maxDistanceFromEdgeAffectingChanceOfDripstoneColumn = 3;
    int maxDistanceFromCenterAffectingHeightBias = 5;

    DripstoneClusterConfiguration() = default;
};

//=============================================================================
// DripstoneClusterFeature
// Reference: DripstoneClusterFeature.java
//=============================================================================

class DripstoneClusterFeature : public Feature<DripstoneClusterConfiguration> {
public:
    // Reference: DripstoneClusterFeature.java place() lines 26-49
    bool place(FeaturePlaceContext<DripstoneClusterConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const DripstoneClusterConfiguration& config = context.config();

        // Check if position is empty or water
        ::world::IBlockType* originBlock = level->getBlockState(origin);
        if (!originBlock) {
            return false;
        }
        BlockState* originState = static_cast<BlockState*>(originBlock);
        if (!originState->isAir() && originState->getIdentifier() != "minecraft:water") {
            return false;
        }

        // Get cluster dimensions
        int height = config.height ? config.height->sample(random) : 5;
        float wetness = config.wetness;
        float density = config.density;
        int xRadius = config.radius ? config.radius->sample(random) : 3;
        int zRadius = config.radius ? config.radius->sample(random) : 3;

        // Place cluster columns
        for (int dx = -xRadius; dx <= xRadius; ++dx) {
            for (int dz = -zRadius; dz <= zRadius; ++dz) {
                core::BlockPos pos(origin.getX() + dx, origin.getY(), origin.getZ() + dz);
                // Would place column here
            }
        }

        return true;
    }
};

//=============================================================================
// DesertWellFeature
// Reference: DesertWellFeature.java
//=============================================================================

class DesertWellFeature : public Feature<NoneFeatureConfiguration> {
public:
    // Reference: DesertWellFeature.java place() lines 32-106
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        core::BlockPos origin = context.origin();
        XoroshiroRandomSource& random = context.random();

        // Find solid ground
        core::BlockPos::MutableBlockPos searchPos(origin.getX(), origin.getY(), origin.getZ());
        while (searchPos.getY() > level->getMinY() + 2) {
            ::world::IBlockType* block = level->getBlockState(searchPos);
            if (block && !static_cast<BlockState*>(block)->isAir()) {
                break;
            }
            searchPos.move(0, -1, 0);
        }

        origin = core::BlockPos(searchPos.getX(), searchPos.getY(), searchPos.getZ());

        // Check if on sand
        ::world::IBlockType* groundBlock = level->getBlockState(origin);
        if (!groundBlock || static_cast<BlockState*>(groundBlock)->getIdentifier() != "minecraft:sand") {
            return false;
        }

        // Check for solid ground below
        for (int ox = -2; ox <= 2; ++ox) {
            for (int oz = -2; oz <= 2; ++oz) {
                core::BlockPos checkPos1 = origin.offset(ox, -1, oz);
                core::BlockPos checkPos2 = origin.offset(ox, -2, oz);
                ::world::IBlockType* b1 = level->getBlockState(checkPos1);
                ::world::IBlockType* b2 = level->getBlockState(checkPos2);
                if ((b1 && static_cast<BlockState*>(b1)->isAir()) ||
                    (b2 && static_cast<BlockState*>(b2)->isAir())) {
                    return false;
                }
            }
        }

        // Place sandstone base
        for (int oy = -2; oy <= 0; ++oy) {
            for (int ox = -2; ox <= 2; ++ox) {
                for (int oz = -2; oz <= 2; ++oz) {
                    // Would set sandstone block at origin.offset(ox, oy, oz)
                }
            }
        }

        // Place water
        // Would set water at origin and adjacent

        // Place walls
        for (int ox = -2; ox <= 2; ++ox) {
            for (int oz = -2; oz <= 2; ++oz) {
                if (ox == -2 || ox == 2 || oz == -2 || oz == 2) {
                    // Would set sandstone block at origin.offset(ox, 1, oz)
                }
            }
        }

        // Place slabs at cardinal edges
        // Would set sandstone slabs

        // Place roof
        for (int ox = -1; ox <= 1; ++ox) {
            for (int oz = -1; oz <= 1; ++oz) {
                // Would set sandstone or slab at origin.offset(ox, 4, oz)
            }
        }

        // Place pillars
        for (int oy = 1; oy <= 3; ++oy) {
            // Would set sandstone blocks at corners
        }

        return true;
    }
};

//=============================================================================
// FossilFeatureConfiguration
// Reference: FossilFeatureConfiguration.java
//=============================================================================

struct FossilFeatureConfiguration {
    std::vector<std::string> fossilStructures;
    std::vector<std::string> overlayStructures;
    int maxEmptyCornersAllowed = 4;

    FossilFeatureConfiguration() = default;

    FossilFeatureConfiguration(
        const std::vector<std::string>& fossils,
        const std::vector<std::string>& overlays,
        int maxEmptyCorners = 4
    )
        : fossilStructures(fossils)
        , overlayStructures(overlays)
        , maxEmptyCornersAllowed(maxEmptyCorners)
    {}
};

//=============================================================================
// FossilFeature
// Reference: FossilFeature.java
//=============================================================================

class FossilFeature : public Feature<FossilFeatureConfiguration> {
public:
    // Reference: FossilFeature.java place() lines 28-67
    bool place(FeaturePlaceContext<FossilFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const FossilFeatureConfiguration& config = context.config();

        if (config.fossilStructures.empty()) {
            return false;
        }

        // Get random rotation
        int rotation = random.nextInt(4);

        // Get random fossil index
        int fossilIndex = random.nextInt(static_cast<int>(config.fossilStructures.size()));

        // Structure templates would be loaded and placed here
        // This is a simplified version that just returns true
        // as structure template handling requires additional infrastructure

        return true;
    }
};

//=============================================================================
// RandomFeatureConfiguration
// Reference: RandomFeatureConfiguration.java
//=============================================================================

struct WeightedPlacedFeature {
    float chance = 0.0f;
    // Would contain reference to PlacedFeature

    WeightedPlacedFeature(float c = 0.0f) : chance(c) {}
};

struct RandomFeatureConfiguration {
    std::vector<WeightedPlacedFeature> features;
    // Would contain default feature reference

    RandomFeatureConfiguration() = default;
};

//=============================================================================
// RandomSelectorFeature
// Reference: RandomSelectorFeature.java
//=============================================================================

class RandomSelectorFeature : public Feature<RandomFeatureConfiguration> {
public:
    // Reference: RandomSelectorFeature.java place() lines 16-30
    bool place(FeaturePlaceContext<RandomFeatureConfiguration>& context) override {
        const RandomFeatureConfiguration& config = context.config();
        XoroshiroRandomSource& random = context.random();

        for (const auto& feature : config.features) {
            if (random.nextFloat() < feature.chance) {
                // Would place the weighted feature
                return true;
            }
        }

        // Would place default feature
        return true;
    }
};

//=============================================================================
// RandomBooleanFeatureConfiguration
// Reference: RandomBooleanFeatureConfiguration.java
//=============================================================================

struct RandomBooleanFeatureConfiguration {
    // Would contain featureTrue and featureFalse references
    RandomBooleanFeatureConfiguration() = default;
};

//=============================================================================
// RandomBooleanSelectorFeature
// Reference: RandomBooleanSelectorFeature.java
//=============================================================================

class RandomBooleanSelectorFeature : public Feature<RandomBooleanFeatureConfiguration> {
public:
    // Reference: RandomBooleanSelectorFeature.java place() lines 16-24
    bool place(FeaturePlaceContext<RandomBooleanFeatureConfiguration>& context) override {
        XoroshiroRandomSource& random = context.random();
        bool result = random.nextBoolean();
        // Would place featureTrue or featureFalse based on result
        return true;
    }
};

//=============================================================================
// SimpleRandomFeatureConfiguration
// Reference: SimpleRandomFeatureConfiguration.java
//=============================================================================

struct SimpleRandomFeatureConfiguration {
    int featureCount = 0;
    // Would contain list of features

    SimpleRandomFeatureConfiguration() = default;
    SimpleRandomFeatureConfiguration(int count) : featureCount(count) {}
};

//=============================================================================
// SimpleRandomSelectorFeature
// Reference: SimpleRandomSelectorFeature.java
//=============================================================================

class SimpleRandomSelectorFeature : public Feature<SimpleRandomFeatureConfiguration> {
public:
    // Reference: SimpleRandomSelectorFeature.java place() lines 16-25
    bool place(FeaturePlaceContext<SimpleRandomFeatureConfiguration>& context) override {
        XoroshiroRandomSource& random = context.random();
        const SimpleRandomFeatureConfiguration& config = context.config();

        if (config.featureCount <= 0) {
            return false;
        }

        int index = random.nextInt(config.featureCount);
        // Would place feature at index
        return true;
    }
};

//=============================================================================
// TwistingVinesConfiguration
// Reference: TwistingVinesConfig.java
//=============================================================================

struct TwistingVinesConfiguration {
    int spreadWidth = 4;
    int spreadHeight = 1;
    int maxHeight = 8;

    TwistingVinesConfiguration() = default;
    TwistingVinesConfiguration(int width, int height, int max)
        : spreadWidth(width), spreadHeight(height), maxHeight(max)
    {}
};

//=============================================================================
// TwistingVinesFeature
// Reference: TwistingVinesFeature.java
//=============================================================================

class TwistingVinesFeature : public Feature<TwistingVinesConfiguration> {
public:
    // Reference: TwistingVinesFeature.java place() lines 20-52
    bool place(FeaturePlaceContext<TwistingVinesConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const TwistingVinesConfiguration& config = context.config();

        if (isInvalidPlacementLocation(level, origin)) {
            return false;
        }

        int spreadWidth = config.spreadWidth;
        int spreadHeight = config.spreadHeight;
        int maxHeight = config.maxHeight;

        core::BlockPos::MutableBlockPos placePos(origin.getX(), origin.getY(), origin.getZ());

        for (int i = 0; i < spreadWidth * spreadWidth; ++i) {
            int ox = random.nextIntBetweenInclusive(-spreadWidth, spreadWidth);
            int oy = random.nextIntBetweenInclusive(-spreadHeight, spreadHeight);
            int oz = random.nextIntBetweenInclusive(-spreadWidth, spreadWidth);
            placePos.set(origin.getX() + ox, origin.getY() + oy, origin.getZ() + oz);

            if (findFirstAirBlockAboveGround(level, placePos) &&
                !isInvalidPlacementLocation(level, placePos)) {
                int vineHeight = random.nextIntBetweenInclusive(1, maxHeight);
                if (random.nextInt(6) == 0) {
                    vineHeight *= 2;
                }
                if (random.nextInt(5) == 0) {
                    vineHeight = 1;
                }
                // Would place vine column here
            }
        }

        return true;
    }

private:
    bool findFirstAirBlockAboveGround(::world::IChunk* level, core::BlockPos::MutableBlockPos& pos) {
        while (pos.getY() > level->getMinY()) {
            pos.move(0, -1, 0);
            ::world::IBlockType* block = level->getBlockState(pos);
            if (block && !static_cast<BlockState*>(block)->isAir()) {
                pos.move(0, 1, 0);
                return true;
            }
        }
        return false;
    }

    bool isInvalidPlacementLocation(::world::IChunk* level, const core::BlockPos& pos) {
        ::world::IBlockType* block = level->getBlockState(pos);
        if (!block || !static_cast<BlockState*>(block)->isAir()) {
            return true;
        }

        ::world::IBlockType* belowBlock = level->getBlockState(pos.below());
        if (!belowBlock) {
            return true;
        }
        BlockState* belowState = static_cast<BlockState*>(belowBlock);
        return belowState->getIdentifier() != "minecraft:netherrack" &&
               belowState->getIdentifier() != "minecraft:warped_nylium" &&
               belowState->getIdentifier() != "minecraft:warped_wart_block";
    }
};

//=============================================================================
// WeepingVinesFeature
// Reference: WeepingVinesFeature.java
//=============================================================================

class WeepingVinesFeature : public Feature<NoneFeatureConfiguration> {
public:
    // Reference: WeepingVinesFeature.java place() lines 22-37
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();

        ::world::IBlockType* originBlock = level->getBlockState(origin);
        if (!originBlock || !static_cast<BlockState*>(originBlock)->isAir()) {
            return false;
        }

        ::world::IBlockType* aboveBlock = level->getBlockState(origin.above());
        if (!aboveBlock) {
            return false;
        }
        BlockState* aboveState = static_cast<BlockState*>(aboveBlock);
        if (aboveState->getIdentifier() != "minecraft:netherrack" && aboveState->getIdentifier() != "minecraft:nether_wart_block") {
            return false;
        }

        placeRoofNetherWart(level, random, origin);
        placeRoofWeepingVines(level, random, origin);
        return true;
    }

private:
    void placeRoofNetherWart(::world::IChunk* level, XoroshiroRandomSource& random, const core::BlockPos& origin) {
        // Would place nether wart blocks
        core::BlockPos::MutableBlockPos placePos(origin.getX(), origin.getY(), origin.getZ());
        core::BlockPos::MutableBlockPos neighbourPos(origin.getX(), origin.getY(), origin.getZ());

        for (int i = 0; i < 200; ++i) {
            int ox = random.nextInt(6) - random.nextInt(6);
            int oy = random.nextInt(2) - random.nextInt(5);
            int oz = random.nextInt(6) - random.nextInt(6);
            placePos.set(origin.getX() + ox, origin.getY() + oy, origin.getZ() + oz);

            ::world::IBlockType* block = level->getBlockState(placePos);
            if (block && static_cast<BlockState*>(block)->isAir()) {
                // Would check neighbours and place nether wart block
            }
        }
    }

    void placeRoofWeepingVines(::world::IChunk* level, XoroshiroRandomSource& random, const core::BlockPos& origin) {
        core::BlockPos::MutableBlockPos placePos(origin.getX(), origin.getY(), origin.getZ());

        for (int i = 0; i < 100; ++i) {
            int ox = random.nextInt(8) - random.nextInt(8);
            int oy = random.nextInt(2) - random.nextInt(7);
            int oz = random.nextInt(8) - random.nextInt(8);
            placePos.set(origin.getX() + ox, origin.getY() + oy, origin.getZ() + oz);

            ::world::IBlockType* block = level->getBlockState(placePos);
            if (block && static_cast<BlockState*>(block)->isAir()) {
                ::world::IBlockType* aboveBlock = level->getBlockState(placePos.above());
                if (aboveBlock) {
                    BlockState* aboveState = static_cast<BlockState*>(aboveBlock);
                    if (aboveState->getIdentifier() == "minecraft:netherrack" || aboveState->getIdentifier() == "minecraft:nether_wart_block") {
                        int vineHeight = random.nextIntBetweenInclusive(1, 8);
                        if (random.nextInt(6) == 0) {
                            vineHeight *= 2;
                        }
                        if (random.nextInt(5) == 0) {
                            vineHeight = 1;
                        }
                        // Would place weeping vines column
                    }
                }
            }
        }
    }
};

//=============================================================================
// BlockPileConfiguration
// Reference: BlockPileConfiguration.java
//=============================================================================

struct BlockPileConfiguration {
    std::shared_ptr<feature::stateproviders::BlockStateProvider> stateProvider;

    BlockPileConfiguration() = default;
    BlockPileConfiguration(std::shared_ptr<feature::stateproviders::BlockStateProvider> provider)
        : stateProvider(std::move(provider))
    {}
};

//=============================================================================
// BlockPileFeature
// Reference: BlockPileFeature.java
//=============================================================================

class BlockPileFeature : public Feature<BlockPileConfiguration> {
public:
    // Reference: BlockPileFeature.java place() lines 18-40
    bool place(FeaturePlaceContext<BlockPileConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const BlockPileConfiguration& config = context.config();

        if (origin.getY() < level->getMinY() + 5) {
            return false;
        }

        int xr = 2 + random.nextInt(2);
        int zr = 2 + random.nextInt(2);

        for (int x = -xr; x <= xr; ++x) {
            for (int y = 0; y <= 1; ++y) {
                for (int z = -zr; z <= zr; ++z) {
                    core::BlockPos blockPos = origin.offset(x, y, z);
                    int xd = x;
                    int zd = z;
                    float distCheck = static_cast<float>(xd * xd + zd * zd);

                    if (distCheck <= random.nextFloat() * 10.0f - random.nextFloat() * 6.0f ||
                        random.nextFloat() < 0.031f) {
                        tryPlaceBlock(level, blockPos, random, config);
                    }
                }
            }
        }

        return true;
    }

private:
    bool mayPlaceOn(::world::IChunk* level, const core::BlockPos& pos, XoroshiroRandomSource& random) {
        core::BlockPos below = pos.below();
        ::world::IBlockType* belowBlock = level->getBlockState(below);
        if (!belowBlock) {
            return false;
        }
        BlockState* belowState = static_cast<BlockState*>(belowBlock);
        if (belowState->getIdentifier() == "minecraft:dirt_path") {
            return random.nextBoolean();
        }
        // Would check if face is sturdy
        return !belowState->isAir();
    }

    void tryPlaceBlock(::world::IChunk* level, const core::BlockPos& pos,
                       XoroshiroRandomSource& random, const BlockPileConfiguration& config) {
        ::world::IBlockType* block = level->getBlockState(pos);
        if (block && static_cast<BlockState*>(block)->isAir() &&
            mayPlaceOn(level, pos, random)) {
            // Would set block using config.stateProvider
        }
    }
};

//=============================================================================
// SnowAndFreezeFeature
// Reference: SnowAndFreezeFeature.java
//=============================================================================

class SnowAndFreezeFeature : public Feature<NoneFeatureConfiguration> {
public:
    // Reference: SnowAndFreezeFeature.java place() lines 19-48
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();

        core::BlockPos::MutableBlockPos topPos(origin.getX(), origin.getY(), origin.getZ());
        core::BlockPos::MutableBlockPos belowPos(origin.getX(), origin.getY(), origin.getZ());

        for (int dx = 0; dx < 16; ++dx) {
            for (int dz = 0; dz < 16; ++dz) {
                int x = origin.getX() + dx;
                int z = origin.getZ() + dz;
                // Would get height from heightmap
                // Would check biome temperature and place ice/snow
            }
        }

        return true;
    }
};

//=============================================================================
// BlockStateConfiguration
// Reference: BlockStateConfiguration.java
//=============================================================================

struct BlockStateConfiguration {
    BlockState* state;

    BlockStateConfiguration() = default;
    BlockStateConfiguration(BlockState* s) : state(s) {}
};

//=============================================================================
// IcebergFeature
// Reference: IcebergFeature.java
//=============================================================================

class IcebergFeature : public Feature<BlockStateConfiguration> {
public:
    // Reference: IcebergFeature.java place() lines 19-70
    bool place(FeaturePlaceContext<BlockStateConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        core::BlockPos origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const BlockStateConfiguration& config = context.config();

        // Set origin Y to sea level
        // origin = core::BlockPos(origin.getX(), seaLevel, origin.getZ());

        bool snowOnTop = random.nextDouble() > 0.7;
        double shapeAngle = random.nextDouble() * 2.0 * M_PI;
        int shapeEllipseA = 11 - random.nextInt(5);
        int shapeEllipseC = 3 + random.nextInt(3);
        bool isEllipse = random.nextDouble() > 0.7;
        int maxWidthRoundIceberg = 11;
        int overWaterHeight = isEllipse ? random.nextInt(6) + 6 : random.nextInt(15) + 3;

        if (!isEllipse && random.nextDouble() > 0.9) {
            overWaterHeight += random.nextInt(19) + 7;
        }

        int underWaterHeight = std::min(overWaterHeight + random.nextInt(11), 18);
        int width = std::min(overWaterHeight + random.nextInt(7) - random.nextInt(5), 11);
        int a = isEllipse ? shapeEllipseA : 11;

        // Would place iceberg blocks in ellipse/round shape
        for (int xo = -a; xo < a; ++xo) {
            for (int zo = -a; zo < a; ++zo) {
                for (int yOff = 0; yOff < overWaterHeight; ++yOff) {
                    // Would generate iceberg block
                }
            }
        }

        return true;
    }

private:
    int heightDependentRadiusRound(XoroshiroRandomSource& random, int yOff, int height, int width) {
        float k = 3.5f - random.nextFloat();
        float scale = (1.0f - std::pow(static_cast<float>(yOff), 2.0f) / (static_cast<float>(height) * k)) *
                      static_cast<float>(width);
        return static_cast<int>(std::ceil(scale / 2.0f));
    }

    int heightDependentRadiusEllipse(int yOff, int height, int width) {
        float k = 1.0f;
        float scale = (1.0f - std::pow(static_cast<float>(yOff), 2.0f) / (static_cast<float>(height) * k)) *
                      static_cast<float>(width);
        return static_cast<int>(std::ceil(scale / 2.0f));
    }
};

//=============================================================================
// BasaltPillarFeature
// Reference: BasaltPillarFeature.java
//=============================================================================

class BasaltPillarFeature : public Feature<NoneFeatureConfiguration> {
public:
    // Reference: BasaltPillarFeature.java place() lines 18-76
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();

        ::world::IBlockType* originBlock = level->getBlockState(origin);
        ::world::IBlockType* aboveBlock = level->getBlockState(origin.above());

        if (!originBlock || !aboveBlock) return false;

        bool originEmpty = static_cast<BlockState*>(originBlock)->isAir();
        bool aboveEmpty = static_cast<BlockState*>(aboveBlock)->isAir();

        if (!originEmpty || aboveEmpty) {
            return false;
        }

        core::BlockPos::MutableBlockPos pos(origin.getX(), origin.getY(), origin.getZ());
        core::BlockPos::MutableBlockPos tmpPos(origin.getX(), origin.getY(), origin.getZ());

        bool placeNorthHangoff = true;
        bool placeSouthHangoff = true;
        bool placeWestHangoff = true;
        bool placeEastHangoff = true;

        // Place pillar going down - Reference: lines 30-41
        while (true) {
            ::world::IBlockType* currentBlock = level->getBlockState(pos);
            if (!currentBlock || !static_cast<BlockState*>(currentBlock)->isAir()) {
                break;
            }

            if (pos.getY() < level->getMinY() || pos.getY() > level->getMaxY()) {
                return true;
            }

            // Place hangoffs - Reference: lines 36-39
            tmpPos.set(pos.getX(), pos.getY(), pos.getZ() - 1);
            placeNorthHangoff = placeNorthHangoff && placeHangOff(level, random, tmpPos);

            tmpPos.set(pos.getX(), pos.getY(), pos.getZ() + 1);
            placeSouthHangoff = placeSouthHangoff && placeHangOff(level, random, tmpPos);

            tmpPos.set(pos.getX() - 1, pos.getY(), pos.getZ());
            placeWestHangoff = placeWestHangoff && placeHangOff(level, random, tmpPos);

            tmpPos.set(pos.getX() + 1, pos.getY(), pos.getZ());
            placeEastHangoff = placeEastHangoff && placeHangOff(level, random, tmpPos);

            pos.move(0, -1, 0);
        }

        pos.move(0, 1, 0);

        // Place base hangoffs - Reference: lines 44-47
        placeBaseHangOff(level, random, core::BlockPos(pos.getX(), pos.getY(), pos.getZ() - 1));
        placeBaseHangOff(level, random, core::BlockPos(pos.getX(), pos.getY(), pos.getZ() + 1));
        placeBaseHangOff(level, random, core::BlockPos(pos.getX() - 1, pos.getY(), pos.getZ()));
        placeBaseHangOff(level, random, core::BlockPos(pos.getX() + 1, pos.getY(), pos.getZ()));

        pos.move(0, -1, 0);

        // Place base spread - Reference: lines 51-71
        core::BlockPos::MutableBlockPos basePos(pos.getX(), pos.getY(), pos.getZ());
        for (int dx = -3; dx < 4; ++dx) {
            for (int dz = -3; dz < 4; ++dz) {
                int probability = std::abs(dx) * std::abs(dz);
                if (random.nextInt(10) < 10 - probability) {
                    basePos.set(pos.getX() + dx, pos.getY(), pos.getZ() + dz);
                    int maxDrop = 3;

                    while (maxDrop > 0) {
                        tmpPos.set(basePos.getX(), basePos.getY() - 1, basePos.getZ());
                        ::world::IBlockType* belowBlock = level->getBlockState(tmpPos);
                        if (!belowBlock || !static_cast<BlockState*>(belowBlock)->isAir()) {
                            break;
                        }
                        basePos.move(0, -1, 0);
                        --maxDrop;
                    }

                    tmpPos.set(basePos.getX(), basePos.getY() - 1, basePos.getZ());
                    ::world::IBlockType* finalBelow = level->getBlockState(tmpPos);
                    if (finalBelow && !static_cast<BlockState*>(finalBelow)->isAir()) {
                        // Would set BASALT at basePos
                    }
                }
            }
        }

        return true;
    }

private:
    // Reference: BasaltPillarFeature.java placeBaseHangOff() lines 79-83
    void placeBaseHangOff(::world::IChunk* level, XoroshiroRandomSource& random, const core::BlockPos& pos) {
        if (random.nextBoolean()) {
            // Would set BASALT at pos
        }
    }

    // Reference: BasaltPillarFeature.java placeHangOff() lines 86-92
    bool placeHangOff(::world::IChunk* level, XoroshiroRandomSource& random, const core::BlockPos& pos) {
        if (random.nextInt(10) != 0) {
            // Would set BASALT at pos
            return true;
        }
        return false;
    }
};

//=============================================================================
// BlockBlobFeature
// Reference: BlockBlobFeature.java
//=============================================================================

class BlockBlobFeature : public Feature<BlockStateConfiguration> {
public:
    // Reference: BlockBlobFeature.java place() lines 15-50
    bool place(FeaturePlaceContext<BlockStateConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        core::BlockPos origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const BlockStateConfiguration& config = context.config();

        // Find valid position - Reference: lines 21-28
        while (origin.getY() > level->getMinY() + 3) {
            ::world::IBlockType* belowBlock = level->getBlockState(origin.below());
            if (belowBlock) {
                BlockState* belowState = static_cast<BlockState*>(belowBlock);
                if (!belowState->isAir()) {
                    if (belowState->getIdentifier() == "minecraft:dirt" || belowState->getIdentifier() == "minecraft:grass_block" || belowState->getIdentifier() == "minecraft:stone") {
                        break;
                    }
                }
            }
            origin = origin.below();
        }

        if (origin.getY() <= level->getMinY() + 3) {
            return false;
        }

        // Place 3 blob iterations - Reference: lines 33-46
        for (int c = 0; c < 3; ++c) {
            int xr = random.nextInt(2);
            int yr = random.nextInt(2);
            int zr = random.nextInt(2);
            float tr = static_cast<float>(xr + yr + zr) * 0.333f + 0.5f;

            for (int dx = -xr; dx <= xr; ++dx) {
                for (int dy = -yr; dy <= yr; ++dy) {
                    for (int dz = -zr; dz <= zr; ++dz) {
                        core::BlockPos blockPos = origin.offset(dx, dy, dz);
                        double distSq = static_cast<double>(dx * dx + dy * dy + dz * dz);
                        if (distSq <= static_cast<double>(tr * tr)) {
                            // Would set block using config.state
                        }
                    }
                }
            }

            origin = origin.offset(-1 + random.nextInt(2), -random.nextInt(2), -1 + random.nextInt(2));
        }

        return true;
    }
};

//=============================================================================
// LayerConfiguration
// Reference: LayerConfiguration.java
//=============================================================================

struct LayerConfiguration {
    int height = 0;
    BlockState* state;

    LayerConfiguration() = default;
    LayerConfiguration(int h, BlockState* s) : height(h), state(s) {}
};

//=============================================================================
// FillLayerFeature
// Reference: FillLayerFeature.java
//=============================================================================

class FillLayerFeature : public Feature<LayerConfiguration> {
public:
    // Reference: FillLayerFeature.java place() lines 13-32
    bool place(FeaturePlaceContext<LayerConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        const LayerConfiguration& config = context.config();

        core::BlockPos::MutableBlockPos pos(origin.getX(), origin.getY(), origin.getZ());

        for (int dx = 0; dx < 16; ++dx) {
            for (int dz = 0; dz < 16; ++dz) {
                int x = origin.getX() + dx;
                int z = origin.getZ() + dz;
                int y = level->getMinY() + config.height;
                pos.set(x, y, z);

                ::world::IBlockType* block = level->getBlockState(pos);
                if (block && static_cast<BlockState*>(block)->isAir()) {
                    // Would set block using config.state
                }
            }
        }

        return true;
    }
};

//=============================================================================
// ReplaceSphereConfiguration
// Reference: ReplaceSphereConfiguration.java
//=============================================================================

struct ReplaceSphereConfiguration {
    BlockState* targetState;
    BlockState* replaceState;
    std::shared_ptr<util::IntProvider> radius;

    ReplaceSphereConfiguration() = default;
    ReplaceSphereConfiguration(
        BlockState* target,
        BlockState* replace,
        std::shared_ptr<util::IntProvider> r
    ) : targetState(target), replaceState(replace), radius(std::move(r)) {}
};

//=============================================================================
// ReplaceBlobsFeature
// Reference: ReplaceBlobsFeature.java
//=============================================================================

class ReplaceBlobsFeature : public Feature<ReplaceSphereConfiguration> {
public:
    // Reference: ReplaceBlobsFeature.java place() lines 19-47
    bool place(FeaturePlaceContext<ReplaceSphereConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        core::BlockPos origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const ReplaceSphereConfiguration& config = context.config();

        int clampedY = std::max(level->getMinY() + 1, std::min(origin.getY(), level->getMaxY()));
        core::BlockPos::MutableBlockPos cursor(origin.getX(), clampedY, origin.getZ());

        // Find target block - Reference: lines 50-60
        core::BlockPos centerPos = findTarget(level, cursor, config.targetState);
        if (centerPos.getY() == INT_MIN) {
            return false;
        }

        int radiusX = config.radius ? config.radius->sample(random) : 3;
        int radiusY = config.radius ? config.radius->sample(random) : 3;
        int radiusZ = config.radius ? config.radius->sample(random) : 3;
        int maximumRadius = std::max(radiusX, std::max(radiusY, radiusZ));
        bool replacedAny = false;

        // Replace blocks within manhattan distance - Reference: lines 34-44
        for (int dx = -radiusX; dx <= radiusX; ++dx) {
            for (int dy = -radiusY; dy <= radiusY; ++dy) {
                for (int dz = -radiusZ; dz <= radiusZ; ++dz) {
                    int manhattanDist = std::abs(dx) + std::abs(dy) + std::abs(dz);
                    if (manhattanDist > maximumRadius) {
                        continue;
                    }

                    core::BlockPos pos = centerPos.offset(dx, dy, dz);
                    ::world::IBlockType* block = level->getBlockState(pos);
                    if (block && static_cast<BlockState*>(block)->getIdentifier() == config.targetState->getIdentifier()) {
                        // Would set replaceState
                        replacedAny = true;
                    }
                }
            }
        }

        return replacedAny;
    }

private:
    // Reference: ReplaceBlobsFeature.java findTarget() lines 50-60
    core::BlockPos findTarget(::world::IChunk* level, core::BlockPos::MutableBlockPos& cursor,
                              BlockState* target) {
        while (cursor.getY() > level->getMinY() + 1) {
            ::world::IBlockType* block = level->getBlockState(cursor);
            if (block && static_cast<BlockState*>(block)->getIdentifier() == target->getIdentifier()) {
                return core::BlockPos(cursor.getX(), cursor.getY(), cursor.getZ());
            }
            cursor.move(0, -1, 0);
        }
        return core::BlockPos(0, INT_MIN, 0);
    }
};

//=============================================================================
// ReplaceBlockConfiguration
// Reference: ReplaceBlockConfiguration.java
//=============================================================================

struct ReplaceBlockConfiguration {
    std::vector<OreConfiguration::TargetBlockState> targetStates;

    ReplaceBlockConfiguration() = default;
    ReplaceBlockConfiguration(const std::vector<OreConfiguration::TargetBlockState>& targets)
        : targetStates(targets) {}
};

//=============================================================================
// ReplaceBlockFeature
// Reference: ReplaceBlockFeature.java
//=============================================================================

class ReplaceBlockFeature : public Feature<ReplaceBlockConfiguration> {
public:
    // Reference: ReplaceBlockFeature.java place() lines 14-27
    bool place(FeaturePlaceContext<ReplaceBlockConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const ReplaceBlockConfiguration& config = context.config();

        ::world::IBlockType* block = level->getBlockState(origin);
        if (!block) return true;

        BlockState* currentState = static_cast<BlockState*>(block);

        for (const auto& targetState : config.targetStates) {
            if (targetState.target && targetState.target->test(currentState, random)) {
                // Would set targetState.state at origin
                break;
            }
        }

        return true;
    }
};

//=============================================================================
// CountConfiguration
// Reference: CountConfiguration.java
//=============================================================================

struct CountConfiguration {
    std::shared_ptr<util::IntProvider> count;

    CountConfiguration() = default;
    CountConfiguration(std::shared_ptr<util::IntProvider> c) : count(std::move(c)) {}
};

//=============================================================================
// SeaPickleFeature
// Reference: SeaPickleFeature.java
//=============================================================================

class SeaPickleFeature : public Feature<CountConfiguration> {
public:
    // Reference: SeaPickleFeature.java place() lines 18-38
    bool place(FeaturePlaceContext<CountConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const CountConfiguration& config = context.config();

        int placed = 0;
        int count = config.count ? config.count->sample(random) : 20;

        for (int i = 0; i < count; ++i) {
            int x = random.nextInt(8) - random.nextInt(8);
            int z = random.nextInt(8) - random.nextInt(8);
            // Would get height from OCEAN_FLOOR heightmap
            int y = origin.getY();

            core::BlockPos picklePos(origin.getX() + x, y, origin.getZ() + z);
            int pickles = random.nextInt(4) + 1;

            ::world::IBlockType* block = level->getBlockState(picklePos);
            if (block && static_cast<BlockState*>(block)->getIdentifier() == "minecraft:water") {
                // Would set SEA_PICKLE with count property
                ++placed;
            }
        }

        return placed > 0;
    }
};

//=============================================================================
// UnderwaterMagmaConfiguration
// Reference: UnderwaterMagmaConfiguration.java
//=============================================================================

struct UnderwaterMagmaConfiguration {
    int floorSearchRange = 5;
    int placementRadiusAroundFloor = 4;
    float placementProbabilityPerValidPosition = 0.5f;

    UnderwaterMagmaConfiguration() = default;
    UnderwaterMagmaConfiguration(int range, int radius, float probability)
        : floorSearchRange(range)
        , placementRadiusAroundFloor(radius)
        , placementProbabilityPerValidPosition(probability)
    {}
};

//=============================================================================
// UnderwaterMagmaFeature
// Reference: UnderwaterMagmaFeature.java
//=============================================================================

class UnderwaterMagmaFeature : public Feature<UnderwaterMagmaConfiguration> {
public:
    // Reference: UnderwaterMagmaFeature.java place() lines 27-43
    bool place(FeaturePlaceContext<UnderwaterMagmaConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const UnderwaterMagmaConfiguration& config = context.config();

        // Find floor Y - Reference: lines 46-50
        int floorY = getFloorY(level, origin, config);
        if (floorY == INT_MIN) {
            return false;
        }

        core::BlockPos floorPos(origin.getX(), floorY, origin.getZ());
        int radius = config.placementRadiusAroundFloor;
        int placed = 0;

        // Place magma blocks - Reference: lines 39-42
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    core::BlockPos pos = floorPos.offset(dx, dy, dz);
                    if (random.nextFloat() < config.placementProbabilityPerValidPosition) {
                        if (isValidPlacement(level, pos)) {
                            // Would set MAGMA_BLOCK at pos
                            ++placed;
                        }
                    }
                }
            }
        }

        return placed > 0;
    }

private:
    // Reference: UnderwaterMagmaFeature.java getFloorY() lines 46-50
    int getFloorY(::world::IChunk* level, const core::BlockPos& origin, const UnderwaterMagmaConfiguration& config) {
        core::BlockPos::MutableBlockPos pos(origin.getX(), origin.getY(), origin.getZ());

        for (int i = 0; i < config.floorSearchRange; ++i) {
            ::world::IBlockType* block = level->getBlockState(pos);
            if (!block) {
                pos.move(0, -1, 0);
                continue;
            }

            BlockState* state = static_cast<BlockState*>(block);
            if (state->getIdentifier() != "minecraft:water") {
                return pos.getY() + 1;
            }
            pos.move(0, -1, 0);
        }

        return INT_MIN;
    }

    // Reference: UnderwaterMagmaFeature.java isValidPlacement() lines 53-64
    bool isValidPlacement(::world::IChunk* level, const core::BlockPos& pos) {
        ::world::IBlockType* block = level->getBlockState(pos);
        if (!block) return false;

        BlockState* state = static_cast<BlockState*>(block);
        if (state->isAir() || state->getIdentifier() == "minecraft:water") {
            return false;
        }

        // Check if not visible from below
        ::world::IBlockType* belowBlock = level->getBlockState(pos.below());
        if (belowBlock) {
            BlockState* belowState = static_cast<BlockState*>(belowBlock);
            if (belowState->isAir() || belowState->getIdentifier() == "minecraft:water") {
                return false;
            }
        }

        // Check horizontal directions
        for (int dir = 0; dir < 4; ++dir) {
            core::Direction direction = blockpredicates::fromHorizontalIndex(dir);
            core::BlockPos neighborPos = pos.relative(direction);
            ::world::IBlockType* neighborBlock = level->getBlockState(neighborPos);
            if (neighborBlock) {
                BlockState* neighborState = static_cast<BlockState*>(neighborBlock);
                if (neighborState->isAir() || neighborState->getIdentifier() == "minecraft:water") {
                    return false;
                }
            }
        }

        return true;
    }
};

//=============================================================================
// ScatteredOreFeature
// Reference: ScatteredOreFeature.java
//=============================================================================

class ScatteredOreFeature : public Feature<OreConfiguration> {
private:
    static constexpr int MAX_DIST_FROM_ORIGIN = 7;

public:
    // Reference: ScatteredOreFeature.java place() lines 18-39
    bool place(FeaturePlaceContext<OreConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const OreConfiguration& config = context.config();

        int numberOfTries = random.nextInt(config.size + 1);
        core::BlockPos::MutableBlockPos targetPos(origin.getX(), origin.getY(), origin.getZ());

        for (int i = 0; i < numberOfTries; ++i) {
            offsetTargetPos(targetPos, random, origin, std::min(i, MAX_DIST_FROM_ORIGIN));

            ::world::IBlockType* block = level->getBlockState(targetPos);
            if (!block) continue;

            BlockState* blockState = static_cast<BlockState*>(block);

            for (const auto& targetState : config.targetStates) {
                if (targetState.target && targetState.target->test(blockState, random)) {
                    // Would set targetState.state at targetPos
                    break;
                }
            }
        }

        return true;
    }

private:
    // Reference: ScatteredOreFeature.java offsetTargetPos() lines 42-46
    void offsetTargetPos(core::BlockPos::MutableBlockPos& targetPos, XoroshiroRandomSource& random,
                         const core::BlockPos& origin, int maxDistFromOrigin) {
        int xd = getRandomPlacementInOneAxisRelativeToOrigin(random, maxDistFromOrigin);
        int yd = getRandomPlacementInOneAxisRelativeToOrigin(random, maxDistFromOrigin);
        int zd = getRandomPlacementInOneAxisRelativeToOrigin(random, maxDistFromOrigin);
        targetPos.set(origin.getX() + xd, origin.getY() + yd, origin.getZ() + zd);
    }

    // Reference: ScatteredOreFeature.java getRandomPlacementInOneAxisRelativeToOrigin() lines 49-51
    int getRandomPlacementInOneAxisRelativeToOrigin(XoroshiroRandomSource& random, int maxDistance) {
        return static_cast<int>(std::round((random.nextFloat() - random.nextFloat()) * static_cast<float>(maxDistance)));
    }
};

//=============================================================================
// MultifaceGrowthConfiguration
// Reference: MultifaceGrowthConfiguration.java
//=============================================================================

struct MultifaceGrowthConfiguration {
    std::string placeBlock;
    int searchRange = 10;
    bool canPlaceOnFloor = true;
    bool canPlaceOnCeiling = true;
    bool canPlaceOnWall = true;
    float chanceOfSpreading = 0.5f;
    std::vector<std::string> canBePlacedOn;

    MultifaceGrowthConfiguration() = default;

    std::vector<core::Direction> getShuffledDirections(XoroshiroRandomSource& random) const {
        std::vector<core::Direction> directions;
        if (canPlaceOnFloor) directions.push_back(core::Direction::DOWN);
        if (canPlaceOnCeiling) directions.push_back(core::Direction::UP);
        if (canPlaceOnWall) {
            directions.push_back(core::Direction::NORTH);
            directions.push_back(core::Direction::SOUTH);
            directions.push_back(core::Direction::EAST);
            directions.push_back(core::Direction::WEST);
        }

        // Shuffle
        for (size_t i = directions.size() - 1; i > 0; --i) {
            size_t j = random.nextInt(static_cast<int>(i + 1));
            std::swap(directions[i], directions[j]);
        }

        return directions;
    }
};

//=============================================================================
// MultifaceGrowthFeature
// Reference: MultifaceGrowthFeature.java
//=============================================================================

class MultifaceGrowthFeature : public Feature<MultifaceGrowthConfiguration> {
public:
    // Reference: MultifaceGrowthFeature.java place() lines 18-51
    bool place(FeaturePlaceContext<MultifaceGrowthConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const MultifaceGrowthConfiguration& config = context.config();

        ::world::IBlockType* originBlock = level->getBlockState(origin);
        if (!originBlock) return false;

        BlockState* originState = static_cast<BlockState*>(originBlock);
        if (!isAirOrWater(originState)) {
            return false;
        }

        std::vector<core::Direction> searchDirections = config.getShuffledDirections(random);

        // Try to place at origin first
        if (placeGrowthIfPossible(level, origin, originState, config, random, searchDirections)) {
            return true;
        }

        // Search in each direction - Reference: lines 30-49
        core::BlockPos::MutableBlockPos pos(origin.getX(), origin.getY(), origin.getZ());
        for (const auto& searchDirection : searchDirections) {
            pos.set(origin.getX(), origin.getY(), origin.getZ());

            for (int i = 0; i < config.searchRange; ++i) {
                pos.move(searchDirection);
                ::world::IBlockType* block = level->getBlockState(pos);
                if (!block) break;

                BlockState* state = static_cast<BlockState*>(block);
                if (!isAirOrWater(state) && state->getIdentifier() != config.placeBlock) {
                    break;
                }

                if (placeGrowthIfPossible(level, pos, state, config, random, searchDirections)) {
                    return true;
                }
            }
        }

        return false;
    }

private:
    // Reference: MultifaceGrowthFeature.java isAirOrWater() lines 78-80
    static bool isAirOrWater(BlockState* state) {
        return state->isAir() || state->getIdentifier() == "minecraft:water";
    }

    // Reference: MultifaceGrowthFeature.java placeGrowthIfPossible() lines 54-76
    bool placeGrowthIfPossible(
        ::world::IChunk* level,
        const core::BlockPos& pos,
        BlockState* oldState,
        const MultifaceGrowthConfiguration& config,
        XoroshiroRandomSource& random,
        const std::vector<core::Direction>& placementDirections
    ) {
        core::BlockPos::MutableBlockPos mutPos(pos.getX(), pos.getY(), pos.getZ());

        for (const auto& placementDirection : placementDirections) {
            mutPos.set(pos.getX(), pos.getY(), pos.getZ());
            mutPos.move(placementDirection);

            ::world::IBlockType* neighborBlock = level->getBlockState(mutPos);
            if (!neighborBlock) continue;

            BlockState* neighborState = static_cast<BlockState*>(neighborBlock);

            // Check if can be placed on this block
            bool canPlace = false;
            for (const auto& validBlock : config.canBePlacedOn) {
                if (neighborState->getIdentifier() == validBlock) {
                    canPlace = true;
                    break;
                }
            }

            if (canPlace) {
                // Would place multiface block at pos
                if (random.nextFloat() < config.chanceOfSpreading) {
                    // Would spread
                }
                return true;
            }
        }

        return false;
    }
};

//=============================================================================
// SculkPatchConfiguration
// Reference: SculkPatchConfiguration.java
//=============================================================================

struct SculkPatchConfiguration {
    int chargeCount = 10;
    int amountPerCharge = 20;
    int spreadAttempts = 32;
    int growthRounds = 4;
    int spreadRounds = 3;
    std::shared_ptr<util::IntProvider> extraRareGrowths;
    float catalystChance = 0.02f;

    SculkPatchConfiguration() = default;
};

//=============================================================================
// SculkPatchFeature
// Reference: SculkPatchFeature.java
//=============================================================================

class SculkPatchFeature : public Feature<SculkPatchConfiguration> {
public:
    // Reference: SculkPatchFeature.java place() lines 23-63
    bool place(FeaturePlaceContext<SculkPatchConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const SculkPatchConfiguration& config = context.config();

        if (!canSpreadFrom(level, origin)) {
            return false;
        }

        // Sculk spreading would happen here
        // Reference: lines 31-46

        // Place catalyst - Reference: lines 48-51
        core::BlockPos below = origin.below();
        ::world::IBlockType* belowBlock = level->getBlockState(below);
        if (random.nextFloat() <= config.catalystChance && belowBlock) {
            BlockState* belowState = static_cast<BlockState*>(belowBlock);
            if (!belowState->isAir()) {
                // Would place SCULK_CATALYST at origin
            }
        }

        // Place extra shrieker growths - Reference: lines 53-60
        int extraGrowths = config.extraRareGrowths ? config.extraRareGrowths->sample(random) : 0;
        for (int i = 0; i < extraGrowths; ++i) {
            core::BlockPos candidate = origin.offset(random.nextInt(5) - 2, 0, random.nextInt(5) - 2);
            ::world::IBlockType* candBlock = level->getBlockState(candidate);
            if (candBlock && static_cast<BlockState*>(candBlock)->isAir()) {
                // Would place SCULK_SHRIEKER at candidate
            }
        }

        return true;
    }

private:
    // Reference: SculkPatchFeature.java canSpreadFrom() lines 66-77
    bool canSpreadFrom(::world::IChunk* level, const core::BlockPos& origin) {
        ::world::IBlockType* block = level->getBlockState(origin);
        if (!block) return false;

        BlockState* state = static_cast<BlockState*>(block);

        // Check if sculk behavior
        if (state->getIdentifier() == "minecraft:sculk" || state->getIdentifier() == "minecraft:sculk_catalyst" ||
            state->getIdentifier() == "minecraft:sculk_vein" || state->getIdentifier() == "minecraft:sculk_shrieker") {
            return true;
        }

        if (!state->isAir() && state->getIdentifier() != "minecraft:water") {
            return false;
        }

        // Check if any neighbor is solid
        for (int dir = 0; dir < 6; ++dir) {
            core::Direction direction = blockpredicates::fromIndex(dir);
            core::BlockPos neighborPos = origin.relative(direction);
            ::world::IBlockType* neighborBlock = level->getBlockState(neighborPos);
            if (neighborBlock && !static_cast<BlockState*>(neighborBlock)->isAir()) {
                return true;
            }
        }

        return false;
    }
};

//=============================================================================
// RootSystemConfiguration
// Reference: RootSystemConfiguration.java
//=============================================================================

struct RootSystemConfiguration {
    int requiredVerticalSpaceForTree = 2;
    int allowedVerticalWaterForTree = 1;
    int rootColumnMaxHeight = 10;
    int rootRadius = 3;
    int rootPlacementAttempts = 20;
    int hangingRootRadius = 5;
    int hangingRootsVerticalSpan = 5;
    int hangingRootPlacementAttempts = 100;
    std::shared_ptr<feature::stateproviders::BlockStateProvider> rootStateProvider;
    std::shared_ptr<feature::stateproviders::BlockStateProvider> hangingRootStateProvider;
    std::vector<std::string> rootReplaceable;

    RootSystemConfiguration() = default;
};

//=============================================================================
// RootSystemFeature
// Reference: RootSystemFeature.java
//=============================================================================

class RootSystemFeature : public Feature<RootSystemConfiguration> {
public:
    // Reference: RootSystemFeature.java place() lines 20-35
    bool place(FeaturePlaceContext<RootSystemConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const RootSystemConfiguration& config = context.config();

        ::world::IBlockType* originBlock = level->getBlockState(origin);
        if (!originBlock || !static_cast<BlockState*>(originBlock)->isAir()) {
            return false;
        }

        core::BlockPos::MutableBlockPos workingPos(origin.getX(), origin.getY(), origin.getZ());

        // Place dirt and tree - Reference: lines 61-77
        // Place roots - Reference: lines 107-121
        placeRoots(level, config, random, origin, workingPos);

        return true;
    }

private:
    // Reference: RootSystemFeature.java placeRoots() lines 107-121
    void placeRoots(::world::IChunk* level, const RootSystemConfiguration& config,
                    XoroshiroRandomSource& random, const core::BlockPos& pos,
                    core::BlockPos::MutableBlockPos& workingPos) {
        int rootRadius = config.hangingRootRadius;
        int verticalSpan = config.hangingRootsVerticalSpan;

        for (int i = 0; i < config.hangingRootPlacementAttempts; ++i) {
            int ox = random.nextInt(rootRadius) - random.nextInt(rootRadius);
            int oy = random.nextInt(verticalSpan) - random.nextInt(verticalSpan);
            int oz = random.nextInt(rootRadius) - random.nextInt(rootRadius);
            workingPos.set(pos.getX() + ox, pos.getY() + oy, pos.getZ() + oz);

            ::world::IBlockType* block = level->getBlockState(workingPos);
            if (block && static_cast<BlockState*>(block)->isAir()) {
                ::world::IBlockType* aboveBlock = level->getBlockState(workingPos.above());
                if (aboveBlock && !static_cast<BlockState*>(aboveBlock)->isAir()) {
                    // Would set hanging root state
                }
            }
        }
    }
};

//=============================================================================
// WaterloggedVegetationPatchFeature
// Reference: WaterloggedVegetationPatchFeature.java
//=============================================================================

class WaterloggedVegetationPatchFeature : public VegetationPatchFeature {
public:
    // Reference: WaterloggedVegetationPatchFeature.java
    // This feature extends VegetationPatchFeature and adds waterlogging
    bool place(FeaturePlaceContext<VegetationPatchConfiguration>& context) override {
        // Call parent implementation with waterlogging modifications
        return VegetationPatchFeature::place(context);
    }
};

//=============================================================================
// VoidStartPlatformFeature
// Reference: VoidStartPlatformFeature.java
//=============================================================================

class VoidStartPlatformFeature : public Feature<NoneFeatureConfiguration> {
private:
    static constexpr int PLATFORM_RADIUS = 16;

public:
    // Reference: VoidStartPlatformFeature.java place() lines 24-47
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();

        // Platform offset position
        core::BlockPos platformOrigin(8, origin.getY() + 3, 8);

        // Get chunk boundaries
        int chunkMinX = (origin.getX() >> 4) << 4;
        int chunkMinZ = (origin.getZ() >> 4) << 4;
        int chunkMaxX = chunkMinX + 15;
        int chunkMaxZ = chunkMinZ + 15;

        core::BlockPos::MutableBlockPos blockPos(0, 0, 0);

        // Place platform blocks - Reference: lines 33-44
        for (int z = chunkMinZ; z <= chunkMaxZ; ++z) {
            for (int x = chunkMinX; x <= chunkMaxX; ++x) {
                int distX = std::abs(platformOrigin.getX() - x);
                int distZ = std::abs(platformOrigin.getZ() - z);
                int dist = std::max(distX, distZ);

                if (dist <= PLATFORM_RADIUS) {
                    blockPos.set(x, platformOrigin.getY(), z);
                    // Would set COBBLESTONE at center, STONE otherwise
                }
            }
        }

        return true;
    }
};

//=============================================================================
// MonsterRoomFeature
// Reference: MonsterRoomFeature.java
//=============================================================================

class MonsterRoomFeature : public Feature<NoneFeatureConfiguration> {
public:
    // Reference: MonsterRoomFeature.java place() lines 32-128
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();

        int xr = random.nextInt(2) + 2;
        int zr = random.nextInt(2) + 2;
        int minX = -xr - 1;
        int maxX = xr + 1;
        int minZ = -zr - 1;
        int maxZ = zr + 1;

        int holeCount = 0;

        // Check structure validity - Reference: lines 48-66
        for (int dx = minX; dx <= maxX; ++dx) {
            for (int dy = -1; dy <= 4; ++dy) {
                for (int dz = minZ; dz <= maxZ; ++dz) {
                    core::BlockPos holePos = origin.offset(dx, dy, dz);
                    ::world::IBlockType* block = level->getBlockState(holePos);
                    if (!block) continue;

                    BlockState* state = static_cast<BlockState*>(block);
                    bool solid = !state->isAir();

                    if (dy == -1 && !solid) return false;
                    if (dy == 4 && !solid) return false;

                    if ((dx == minX || dx == maxX || dz == minZ || dz == maxZ) && dy == 0) {
                        if (state->isAir()) {
                            ::world::IBlockType* aboveBlock = level->getBlockState(holePos.above());
                            if (aboveBlock && static_cast<BlockState*>(aboveBlock)->isAir()) {
                                ++holeCount;
                            }
                        }
                    }
                }
            }
        }

        if (holeCount < 1 || holeCount > 5) {
            return false;
        }

        // Place dungeon structure - Reference: lines 69-89
        for (int dx = minX; dx <= maxX; ++dx) {
            for (int dy = 3; dy >= -1; --dy) {
                for (int dz = minZ; dz <= maxZ; ++dz) {
                    core::BlockPos wallPos = origin.offset(dx, dy, dz);
                    bool isInterior = dx != minX && dy != -1 && dz != minZ &&
                                      dx != maxX && dy != 4 && dz != maxZ;

                    if (isInterior) {
                        // Would set CAVE_AIR
                    } else {
                        // Would set COBBLESTONE or MOSSY_COBBLESTONE
                    }
                }
            }
        }

        // Place chests and spawner - Reference: lines 91-122
        // Would place chests and mob spawner

        return true;
    }
};

//=============================================================================
// BonusChestFeature
// Reference: BonusChestFeature.java
//=============================================================================

class BonusChestFeature : public Feature<NoneFeatureConfiguration> {
public:
    // Reference: BonusChestFeature.java place() lines 25-60
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();

        int chunkMinX = (origin.getX() >> 4) << 4;
        int chunkMinZ = (origin.getZ() >> 4) << 4;

        // Create shuffled positions
        std::vector<int> xPoses, zPoses;
        for (int i = 0; i <= 15; ++i) {
            xPoses.push_back(chunkMinX + i);
            zPoses.push_back(chunkMinZ + i);
        }

        // Shuffle
        for (size_t i = xPoses.size() - 1; i > 0; --i) {
            size_t j = random.nextInt(static_cast<int>(i + 1));
            std::swap(xPoses[i], xPoses[j]);
        }
        for (size_t i = zPoses.size() - 1; i > 0; --i) {
            size_t j = random.nextInt(static_cast<int>(i + 1));
            std::swap(zPoses[i], zPoses[j]);
        }

        // Find valid chest position - Reference: lines 41-55
        for (int x : xPoses) {
            for (int z : zPoses) {
                // Would get height from MOTION_BLOCKING_NO_LEAVES heightmap
                core::BlockPos chestPos(x, origin.getY(), z);

                ::world::IBlockType* block = level->getBlockState(chestPos);
                if (block && static_cast<BlockState*>(block)->isAir()) {
                    // Would place chest and torches
                    return true;
                }
            }
        }

        return false;
    }
};

//=============================================================================
// FallenTreeConfiguration
// Reference: FallenTreeConfiguration.java
//=============================================================================

struct FallenTreeConfiguration {
    std::shared_ptr<feature::stateproviders::BlockStateProvider> trunkProvider;
    std::shared_ptr<util::IntProvider> logLength;
    std::vector<std::shared_ptr<feature::treedecorators::TreeDecorator>> stumpDecorators;
    std::vector<std::shared_ptr<feature::treedecorators::TreeDecorator>> logDecorators;

    FallenTreeConfiguration() = default;
};

//=============================================================================
// FallenTreeFeature
// Reference: FallenTreeFeature.java
//=============================================================================

class FallenTreeFeature : public Feature<FallenTreeConfiguration> {
private:
    static constexpr int STUMP_HEIGHT = 1;
    static constexpr int FALLEN_LOG_MAX_FALL_HEIGHT = 5;
    static constexpr int FALLEN_LOG_MAX_GROUND_GAP = 2;

public:
    // Reference: FallenTreeFeature.java place() lines 30-33
    bool place(FeaturePlaceContext<FallenTreeConfiguration>& context) override {
        ::world::IChunk* level = context.level();
        const core::BlockPos& origin = context.origin();
        XoroshiroRandomSource& random = context.random();
        const FallenTreeConfiguration& config = context.config();

        placeFallenTree(config, origin, level, random);
        return true;
    }

private:
    // Reference: FallenTreeFeature.java placeFallenTree() lines 35-44
    void placeFallenTree(const FallenTreeConfiguration& config, const core::BlockPos& origin,
                         ::world::IChunk* level, XoroshiroRandomSource& random) {
        // Place stump
        core::BlockPos::MutableBlockPos stumpPos(origin.getX(), origin.getY(), origin.getZ());
        // Would place stump log

        // Get random direction
        core::Direction direction = blockpredicates::fromHorizontalIndex(random.nextInt(4));
        int logLength = config.logLength ? config.logLength->sample(random) - 2 : 3;

        // Calculate log start position
        int startOffset = 2 + random.nextInt(2);
        core::BlockPos::MutableBlockPos logStartPos(
            origin.getX() + blockpredicates::getStepX(direction) * startOffset,
            origin.getY(),
            origin.getZ() + blockpredicates::getStepZ(direction) * startOffset
        );

        // Set ground height for fallen log
        setGroundHeightForFallenLogStartPos(level, logStartPos);

        // Check and place fallen log
        if (canPlaceEntireFallenLog(level, logLength, logStartPos, direction)) {
            placeFallenLog(config, level, random, logLength, logStartPos, direction);
        }
    }

    // Reference: FallenTreeFeature.java setGroundHeightForFallenLogStartPos() lines 47-57
    void setGroundHeightForFallenLogStartPos(::world::IChunk* level, core::BlockPos::MutableBlockPos& pos) {
        pos.move(0, 1, 0);
        for (int i = 0; i < 6; ++i) {
            if (mayPlaceOn(level, pos)) {
                return;
            }
            pos.move(0, -1, 0);
        }
    }

    // Reference: FallenTreeFeature.java canPlaceEntireFallenLog() lines 65-87
    bool canPlaceEntireFallenLog(::world::IChunk* level, int logLength,
                                  core::BlockPos::MutableBlockPos& pos, core::Direction direction) {
        int gapInGround = 0;

        for (int i = 0; i < logLength; ++i) {
            ::world::IBlockType* block = level->getBlockState(pos);
            if (!block) return false;

            BlockState* state = static_cast<BlockState*>(block);
            if (!state->isAir() && state->getIdentifier() != "minecraft:leaves") {
                return false;
            }

            if (!isOverSolidGround(level, pos)) {
                ++gapInGround;
                if (gapInGround > FALLEN_LOG_MAX_GROUND_GAP) {
                    return false;
                }
            } else {
                gapInGround = 0;
            }

            pos.move(direction);
        }

        // Move back
        for (int i = 0; i < logLength; ++i) {
            pos.move(blockpredicates::opposite(direction));
        }

        return true;
    }

    // Reference: FallenTreeFeature.java placeFallenLog() lines 89-98
    void placeFallenLog(const FallenTreeConfiguration& config, ::world::IChunk* level,
                        XoroshiroRandomSource& random, int logLength,
                        core::BlockPos::MutableBlockPos& pos, core::Direction direction) {
        for (int i = 0; i < logLength; ++i) {
            // Would place sideways log block
            pos.move(direction);
        }
    }

    // Reference: FallenTreeFeature.java mayPlaceOn() lines 100-102
    bool mayPlaceOn(::world::IChunk* level, const core::BlockPos& pos) {
        ::world::IBlockType* block = level->getBlockState(pos);
        if (!block) return false;

        BlockState* state = static_cast<BlockState*>(block);
        if (!state->isAir() && state->getIdentifier() != "minecraft:leaves") {
            return false;
        }

        return isOverSolidGround(level, pos);
    }

    // Reference: FallenTreeFeature.java isOverSolidGround() lines 104-106
    bool isOverSolidGround(::world::IChunk* level, const core::BlockPos& pos) {
        ::world::IBlockType* belowBlock = level->getBlockState(pos.below());
        if (!belowBlock) return false;
        return !static_cast<BlockState*>(belowBlock)->isAir();
    }
};

} // namespace levelgen
} // namespace minecraft
